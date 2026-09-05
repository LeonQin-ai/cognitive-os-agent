/* agent.c — multi-agent coordinator sharing a blackboard. */
#include "cognitive-os-agent/runtime/agent.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

typedef struct coa_agent_entry {
    char *name;
    char *role;
    char *provider;
    char *model;
} coa_agent_entry;

struct coa_agent_pool {
    coa_mutex mtx;
    coa_blackboard *bb;
    int owns_bb;           /* 1 = pool created (and frees) the blackboard */
    coa_agent_entry *agents;
    size_t count;
    size_t cap;
};

coa_agent_pool *coa_agent_pool_new(void) {
    coa_agent_pool *p = (coa_agent_pool *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    coa_mutex_init(&p->mtx);
    p->owns_bb = 1;
    p->bb = coa_blackboard_new();
    if (!p->bb) {
        coa_mutex_destroy(&p->mtx);
        free(p);
        return NULL;
    }
    return p;
}

void coa_agent_pool_free(coa_agent_pool *p) {
    if (!p) return;
    coa_mutex_lock(&p->mtx);
    for (size_t i = 0; i < p->count; i++) {
        free(p->agents[i].name);
        free(p->agents[i].role);
        free(p->agents[i].provider);
        free(p->agents[i].model);
    }
    free(p->agents);
    p->agents = NULL;
    p->count = p->cap = 0;
    coa_mutex_unlock(&p->mtx);
    if (p->owns_bb) coa_blackboard_free(p->bb);
    coa_mutex_destroy(&p->mtx);
    free(p);
}

/* Replace the pool's blackboard with an externally owned one (ctx owns and
 * frees it; the pool only borrows). Lets /v1/blackboard and agent runs share
 * a single state space. */
void coa_agent_pool_adopt_blackboard(coa_agent_pool *p, coa_blackboard *b) {
    if (!p || !b || p->bb == b) return;
    if (p->owns_bb) coa_blackboard_free(p->bb);
    p->bb = b;
    p->owns_bb = 0;
}

/* Returns index of name, or -1. Caller must hold p->mtx. */
static int find_agent(coa_agent_pool *p, const char *name) {
    for (size_t i = 0; i < p->count; i++)
        if (strcmp(p->agents[i].name, name) == 0) return (int)i;
    return -1;
}

int coa_agent_pool_add(coa_agent_pool *p, const char *name, const char *role) {
    return coa_agent_pool_add_model(p, name, role, NULL, NULL);
}

int coa_agent_pool_add_model(coa_agent_pool *p, const char *name, const char *role,
                            const char *provider, const char *model) {
    if (!p || !name || !*name) return -1;
    coa_mutex_lock(&p->mtx);
    if (find_agent(p, name) >= 0) {
        coa_mutex_unlock(&p->mtx);
        return -1; /* duplicate */
    }
    if (p->count == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 8;
        coa_agent_entry *na = (coa_agent_entry *)realloc(p->agents, cap * sizeof(coa_agent_entry));
        if (!na) { coa_mutex_unlock(&p->mtx); return -1; }
        p->agents = na;
        p->cap = cap;
    }
    p->agents[p->count].name = coa_strdup(name);
    p->agents[p->count].role = role ? coa_strdup(role) : coa_strdup("");
    p->agents[p->count].provider = provider ? coa_strdup(provider) : NULL;
    p->agents[p->count].model = model ? coa_strdup(model) : NULL;
    int idx = (int)p->count;
    p->count++;
    coa_mutex_unlock(&p->mtx);
    return idx;
}

int coa_agent_pool_count(coa_agent_pool *p) {
    if (!p) return 0;
    coa_mutex_lock(&p->mtx);
    int n = (int)p->count;
    coa_mutex_unlock(&p->mtx);
    return n;
}

int coa_agent_pool_find(coa_agent_pool *p, const char *name) {
    if (!p || !name) return -1;
    coa_mutex_lock(&p->mtx);
    int idx = find_agent(p, name);
    coa_mutex_unlock(&p->mtx);
    return idx;
}

coa_blackboard *coa_agent_pool_blackboard(coa_agent_pool *p) {
    return p ? p->bb : NULL;
}

int coa_agent_post(coa_agent_pool *p, const char *agent, const char *key, const char *val) {
    if (!p || !agent || !key || !val) return -1;
    coa_mutex_lock(&p->mtx);
    int ok = find_agent(p, agent) >= 0;
    coa_mutex_unlock(&p->mtx);
    if (!ok) return -1;
    coa_blackboard_put(p->bb, key, val);
    return 0;
}

char *coa_agent_pool_snapshot_json(coa_agent_pool *p) {
    if (!p) return coa_strdup("{}");
    coa_mutex_lock(&p->mtx);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
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
    coa_mutex_unlock(&p->mtx);

    char *facts = coa_blackboard_snapshot_json(p->bb);
    if (root && facts) {
        cJSON *fj = cJSON_Parse(facts);
        cJSON_AddItemToObject(root, "facts", fj ? fj : cJSON_CreateObject());
    }
    free(facts);

    char *s = root ? cJSON_PrintUnformatted(root) : NULL;
    if (root) cJSON_Delete(root);
    return s ? s : coa_strdup("{}");
}

/* ---------- roster persistence (<state_root>/agents.json) ---------- */

int coa_agent_pool_save(coa_agent_pool *p, const char *dir) {
    if (!p || !dir || !*dir) return -1;
    coa_mutex_lock(&p->mtx);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int ok = 0;
    if (root && arr) {
        cJSON_AddItemToObject(root, "agents", arr);
        for (size_t i = 0; i < p->count; i++) {
            cJSON *o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddStringToObject(o, "name", p->agents[i].name ? p->agents[i].name : "");
            cJSON_AddStringToObject(o, "role", p->agents[i].role ? p->agents[i].role : "");
            if (p->agents[i].provider) cJSON_AddStringToObject(o, "provider", p->agents[i].provider);
            if (p->agents[i].model) cJSON_AddStringToObject(o, "model", p->agents[i].model);
            cJSON_AddItemToArray(arr, o);
        }
        char *s = cJSON_PrintUnformatted(root);
        if (s) {
            char path[512];
            if (snprintf(path, sizeof(path), "%s/agents.json", dir) < (int)sizeof(path))
                ok = coa_fs_write_file(path, s, strlen(s)) == 0;
            free(s);
        }
    } else if (arr) {
        cJSON_Delete(arr);
    }
    if (root) cJSON_Delete(root);
    coa_mutex_unlock(&p->mtx);
    return ok ? 0 : -1;
}

int coa_agent_pool_load(coa_agent_pool *p, const char *dir) {
    if (!p || !dir || !*dir) return -1;
    char path[512];
    if (snprintf(path, sizeof(path), "%s/agents.json", dir) >= (int)sizeof(path)) return -1;
    char *s = coa_fs_read_file(path);
    if (!s) return -1;
    cJSON *root = cJSON_Parse(s);
    free(s);
    if (!root) return -1;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "agents");
    int loaded = 0;
    if (cJSON_IsArray(arr)) {
        cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            cJSON *n = cJSON_GetObjectItemCaseSensitive(it, "name");
            if (!n || !cJSON_IsString(n) || !n->valuestring || !*n->valuestring) continue;
            cJSON *r = cJSON_GetObjectItemCaseSensitive(it, "role");
            cJSON *prov = cJSON_GetObjectItemCaseSensitive(it, "provider");
            cJSON *mod = cJSON_GetObjectItemCaseSensitive(it, "model");
            if (coa_agent_pool_add_model(p, n->valuestring,
                                        (r && cJSON_IsString(r)) ? r->valuestring : "",
                                        (prov && cJSON_IsString(prov)) ? prov->valuestring : NULL,
                                        (mod && cJSON_IsString(mod)) ? mod->valuestring : NULL) >= 0)
                loaded++;
        }
    }
    cJSON_Delete(root);
    return loaded;
}
