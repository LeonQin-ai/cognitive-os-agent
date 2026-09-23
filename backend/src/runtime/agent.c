/* agent.c — multi-agent coordinator sharing a blackboard. */
#include "runtime/agent.h"
#include "os/os_fs.h"
#include "os/os_thread.h"
#include "infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include "cJSON.h"

typedef struct agent_entry {
    char *name;
    char *role;
    char *provider;
    char *model;
} agent_entry;

struct agent_pool {
    mutex_t mtx;
    blackboard *bb;
    int owns_bb; /* 1 = pool created (and frees) the blackboard */
    agent_entry *agents;
    size_t count;
    size_t cap;
    size_t *name_slots; /* open-addressed name -> index+1; 0 means empty */
    size_t name_cap;
};

agent_pool *agent_pool_new(void) {
    agent_pool *p = (agent_pool *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    mutex_init(&p->mtx);
    p->owns_bb = 1;
    p->bb = blackboard_new();
    if (!p->bb) {
        mutex_destroy(&p->mtx);
        free(p);
        return NULL;
    }

    return p;
}

void agent_pool_free(agent_pool *p) {
    if (!p)
        return;
    mutex_lock(&p->mtx);
    for (size_t i = 0; i < p->count; i++) {
        free(p->agents[i].name);
        free(p->agents[i].role);
        free(p->agents[i].provider);
        free(p->agents[i].model);
    }

    free(p->agents);
    free(p->name_slots);
    p->agents = NULL;
    p->count = p->cap = 0;
    mutex_unlock(&p->mtx);
    if (p->owns_bb)
        blackboard_free(p->bb);
    mutex_destroy(&p->mtx);
    free(p);
}

/* Replace the pool's blackboard with an externally owned one (ctx owns and
 * frees it; the pool only borrows). Lets /v1/blackboard and agent runs share
 * a single state space. */
void agent_pool_adopt_blackboard(agent_pool *p, blackboard *b) {
    if (!p || !b || p->bb == b)
        return;
    if (p->owns_bb)
        blackboard_free(p->bb);
    p->bb = b;
    p->owns_bb = 0;
}

/* O(1) average lookup for large virtual-agent rosters. Caller holds mtx. */
static int find_agent(agent_pool *p, const char *name) {
    if (!p->name_cap) return -1;
    size_t slot = (size_t)hash64(name, strlen(name)) & (p->name_cap - 1);
    while (p->name_slots[slot]) {
        size_t idx = p->name_slots[slot] - 1;
        if (strcmp(p->agents[idx].name, name) == 0) return (int)idx;
        slot = (slot + 1) & (p->name_cap - 1);
    }
    return -1;
}

static void name_index_insert(agent_pool *p, size_t idx) {
    const char *name = p->agents[idx].name;
    size_t slot = (size_t)hash64(name, strlen(name)) & (p->name_cap - 1);
    while (p->name_slots[slot]) slot = (slot + 1) & (p->name_cap - 1);
    p->name_slots[slot] = idx + 1;
}

static int name_index_reserve(agent_pool *p, size_t future_count) {
    if (p->name_cap && future_count * 10 < p->name_cap * 7) return 0;
    size_t cap = p->name_cap ? p->name_cap * 2 : 16;
    if (cap < p->name_cap || cap > SIZE_MAX / sizeof(size_t)) return -1;
    size_t *slots = calloc(cap, sizeof(size_t));
    if (!slots) return -1;
    free(p->name_slots);
    p->name_slots = slots;
    p->name_cap = cap;
    for (size_t i = 0; i < p->count; i++) name_index_insert(p, i);
    return 0;
}

int agent_pool_add(agent_pool *p, const char *name, const char *role) {
    return agent_pool_add_model(p, name, role, NULL, NULL);
}

int agent_pool_add_model(agent_pool *p, const char *name, const char *role, const char *provider,
                             const char *model) {
    int idx;

    if (!p || !name || !*name)
        return -1;
    mutex_lock(&p->mtx);
    if (find_agent(p, name) >= 0) {
        mutex_unlock(&p->mtx);
        return -1; /* duplicate */
    }

    if (p->count >= INT_MAX || name_index_reserve(p, p->count + 1) != 0) {
        mutex_unlock(&p->mtx);
        return -1;
    }
    if (p->count == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 8;
        agent_entry *na = (agent_entry *)realloc(p->agents, cap * sizeof(agent_entry));
        if (!na) {
            mutex_unlock(&p->mtx);
            return -1;
        }
        p->agents = na;
        p->cap = cap;
    }

    agent_entry next = {xstrdup(name), role ? xstrdup(role) : xstrdup(""),
                        provider ? xstrdup(provider) : NULL,
                        model ? xstrdup(model) : NULL};
    if (!next.name || !next.role || (provider && !next.provider) || (model && !next.model)) {
        free(next.name); free(next.role); free(next.provider); free(next.model);
        mutex_unlock(&p->mtx);
        return -1;
    }
    p->agents[p->count] = next;
    idx = (int)p->count;
    p->count++;
    name_index_insert(p, (size_t)idx);
    mutex_unlock(&p->mtx);
    return idx;
}

/* Change the provider/model an existing agent uses (agent 换模型).
 * NULL or "" clears the stored value so the agent falls back to the globally
 * active model. Returns 0 ok, -1 on unknown name / bad args. */
int agent_pool_set_model(agent_pool *p, const char *name, const char *provider, const char *model) {
    int idx;
    agent_entry *a;

    if (!p || !name || !*name)
        return -1;
    mutex_lock(&p->mtx);
    idx = find_agent(p, name);
    if (idx < 0) {
        mutex_unlock(&p->mtx);
        return -1;
    }
    a = &p->agents[idx];
    free(a->provider);
    free(a->model);
    a->provider = (provider && *provider) ? xstrdup(provider) : NULL;
    a->model = (model && *model) ? xstrdup(model) : NULL;
    mutex_unlock(&p->mtx);
    return 0;
}

/* Remove a registered agent by name (frees its strings, shifts the tail).
 * Returns 0 on success, -1 when the pool or name is unknown. */
int agent_pool_remove(agent_pool *p, const char *name) {
    int idx;

    if (!p || !name || !*name)
        return -1;
    mutex_lock(&p->mtx);
    idx = find_agent(p, name);
    if (idx < 0) {
        mutex_unlock(&p->mtx);
        return -1;
    }

    free(p->agents[idx].name);
    free(p->agents[idx].role);
    free(p->agents[idx].provider);
    free(p->agents[idx].model);
    memmove(&p->agents[idx], &p->agents[idx + 1], (p->count - (size_t)idx - 1) * sizeof(agent_entry));
    p->count--;
    memset(p->name_slots, 0, p->name_cap * sizeof(size_t));
    for (size_t i = 0; i < p->count; i++) name_index_insert(p, i);
    mutex_unlock(&p->mtx);
    return 0;
}

int agent_pool_count(agent_pool *p) {
    int n;

    if (!p)
        return 0;
    mutex_lock(&p->mtx);
    n = (int)p->count;
    mutex_unlock(&p->mtx);
    return n;
}

int agent_pool_find(agent_pool *p, const char *name) {
    int idx;

    if (!p || !name)
        return -1;
    mutex_lock(&p->mtx);
    idx = find_agent(p, name);
    mutex_unlock(&p->mtx);
    return idx;
}

blackboard *agent_pool_blackboard(agent_pool *p) {
    return p ? p->bb : NULL;
}

int agent_post(agent_pool *p, const char *agent, const char *key, const char *val) {
    int ok;

    if (!p || !agent || !key || !val)
        return -1;
    mutex_lock(&p->mtx);
    ok = find_agent(p, agent) >= 0;
    mutex_unlock(&p->mtx);
    if (!ok)
        return -1;
    blackboard_put(p->bb, key, val);
    return 0;
}

char *agent_pool_snapshot_json(agent_pool *p) {
    cJSON *root;
    cJSON *arr;
    char *facts;
    char *s;

    if (!p)
        return xstrdup("{}");
    mutex_lock(&p->mtx);
    root = cJSON_CreateObject();
    arr = cJSON_CreateArray();
    if (root && arr) {
        cJSON_AddItemToObject(root, "agents", arr);
        for (size_t i = 0; i < p->count; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", p->agents[i].name ? p->agents[i].name : "");
            cJSON_AddStringToObject(o, "role", p->agents[i].role ? p->agents[i].role : "");
            cJSON_AddStringToObject(o, "provider", p->agents[i].provider ? p->agents[i].provider : "");
            cJSON_AddStringToObject(o, "model", p->agents[i].model ? p->agents[i].model : "");
            cJSON_AddItemToArray(arr, o);
        }
    }

    mutex_unlock(&p->mtx);

    facts = blackboard_snapshot_json(p->bb);
    if (root && facts) {
        cJSON *fj = cJSON_Parse(facts);
        cJSON_AddItemToObject(root, "facts", fj ? fj : cJSON_CreateObject());
    }

    free(facts);

    s = root ? cJSON_PrintUnformatted(root) : NULL;
    if (root)
        cJSON_Delete(root);
    return s ? s : xstrdup("{}");
}

/* ---------- roster persistence (<state_root>/agents.json) ---------- */

int agent_pool_save(agent_pool *p, const char *dir) {
    cJSON *root;
    cJSON *arr;
    int ok = 0;

    if (!p || !dir || !*dir)
        return -1;
    mutex_lock(&p->mtx);
    root = cJSON_CreateObject();
    arr = cJSON_CreateArray();
    if (root && arr) {
        cJSON_AddItemToObject(root, "agents", arr);
        for (size_t i = 0; i < p->count; i++) {
            cJSON *o = cJSON_CreateObject();
            if (!o)
                continue;
            cJSON_AddStringToObject(o, "name", p->agents[i].name ? p->agents[i].name : "");
            cJSON_AddStringToObject(o, "role", p->agents[i].role ? p->agents[i].role : "");
            if (p->agents[i].provider)
                cJSON_AddStringToObject(o, "provider", p->agents[i].provider);
            if (p->agents[i].model)
                cJSON_AddStringToObject(o, "model", p->agents[i].model);
            cJSON_AddItemToArray(arr, o);
        }
        char *s = cJSON_PrintUnformatted(root);
        if (s) {
            char path[512];
            if (snprintf(path, sizeof(path), "%s/agents.json", dir) < (int)sizeof(path))
                ok = fs_write_file(path, s, strlen(s)) == 0;
            free(s);
        }
    } else if (arr) {
        cJSON_Delete(arr);
    }

    if (root)
        cJSON_Delete(root);
    mutex_unlock(&p->mtx);
    return ok ? 0 : -1;
}

int agent_pool_load(agent_pool *p, const char *dir) {
    char path[512];
    char *s;
    cJSON *root;
    cJSON *arr;
    int loaded = 0;

    if (!p || !dir || !*dir)
        return -1;
    if (snprintf(path, sizeof(path), "%s/agents.json", dir) >= (int)sizeof(path))
        return -1;
    s = fs_read_file(path);
    if (!s)
        return -1;
    root = cJSON_Parse(s);
    free(s);
    if (!root)
        return -1;
    arr = cJSON_GetObjectItemCaseSensitive(root, "agents");
    if (cJSON_IsArray(arr)) {
        cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            cJSON *n = cJSON_GetObjectItemCaseSensitive(it, "name");
    cJSON *r;
    cJSON *prov;
    cJSON *mod;

            if (!n || !cJSON_IsString(n) || !n->valuestring || !*n->valuestring)
                continue;
            r = cJSON_GetObjectItemCaseSensitive(it, "role");
            prov = cJSON_GetObjectItemCaseSensitive(it, "provider");
            mod = cJSON_GetObjectItemCaseSensitive(it, "model");
            if (agent_pool_add_model(p, n->valuestring, (r && cJSON_IsString(r)) ? r->valuestring : "",
                                         (prov && cJSON_IsString(prov)) ? prov->valuestring : NULL,
                                         (mod && cJSON_IsString(mod)) ? mod->valuestring : NULL) >= 0)
                loaded++;
        }
    }

    cJSON_Delete(root);
    return loaded;
}
