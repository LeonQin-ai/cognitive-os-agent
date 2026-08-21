/* agent.c — multi-agent coordinator sharing a blackboard. */
#include "cagent/runtime/agent.h"
#include "cagent/os/os_thread.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

typedef struct ca_agent_entry {
    char *name;
    char *role;
} ca_agent_entry;

struct ca_agent_pool {
    ca_mutex mtx;
    ca_blackboard *bb;
    ca_agent_entry *agents;
    size_t count;
    size_t cap;
};

ca_agent_pool *ca_agent_pool_new(void) {
    ca_agent_pool *p = (ca_agent_pool *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    ca_mutex_init(&p->mtx);
    p->bb = ca_blackboard_new();
    if (!p->bb) {
        ca_mutex_destroy(&p->mtx);
        free(p);
        return NULL;
    }
    return p;
}

void ca_agent_pool_free(ca_agent_pool *p) {
    if (!p) return;
    ca_mutex_lock(&p->mtx);
    for (size_t i = 0; i < p->count; i++) {
        free(p->agents[i].name);
        free(p->agents[i].role);
    }
    free(p->agents);
    p->agents = NULL;
    p->count = p->cap = 0;
    ca_mutex_unlock(&p->mtx);
    ca_blackboard_free(p->bb);
    ca_mutex_destroy(&p->mtx);
    free(p);
}

/* Returns index of name, or -1. Caller must hold p->mtx. */
static int find_agent(ca_agent_pool *p, const char *name) {
    for (size_t i = 0; i < p->count; i++)
        if (strcmp(p->agents[i].name, name) == 0) return (int)i;
    return -1;
}

int ca_agent_pool_add(ca_agent_pool *p, const char *name, const char *role) {
    if (!p || !name || !*name) return -1;
    ca_mutex_lock(&p->mtx);
    if (find_agent(p, name) >= 0) {
        ca_mutex_unlock(&p->mtx);
        return -1; /* duplicate */
    }
    if (p->count == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 8;
        ca_agent_entry *na = (ca_agent_entry *)realloc(p->agents, cap * sizeof(ca_agent_entry));
        if (!na) { ca_mutex_unlock(&p->mtx); return -1; }
        p->agents = na;
        p->cap = cap;
    }
    p->agents[p->count].name = ca_strdup(name);
    p->agents[p->count].role = role ? ca_strdup(role) : ca_strdup("");
    int idx = (int)p->count;
    p->count++;
    ca_mutex_unlock(&p->mtx);
    return idx;
}

int ca_agent_pool_count(ca_agent_pool *p) {
    if (!p) return 0;
    ca_mutex_lock(&p->mtx);
    int n = (int)p->count;
    ca_mutex_unlock(&p->mtx);
    return n;
}

ca_blackboard *ca_agent_pool_blackboard(ca_agent_pool *p) {
    return p ? p->bb : NULL;
}

int ca_agent_post(ca_agent_pool *p, const char *agent, const char *key, const char *val) {
    if (!p || !agent || !key || !val) return -1;
    ca_mutex_lock(&p->mtx);
    int ok = find_agent(p, agent) >= 0;
    ca_mutex_unlock(&p->mtx);
    if (!ok) return -1;
    ca_blackboard_put(p->bb, key, val);
    return 0;
}

char *ca_agent_pool_snapshot_json(ca_agent_pool *p) {
    if (!p) return ca_strdup("{}");
    ca_mutex_lock(&p->mtx);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (root && arr) {
        cJSON_AddItemToObject(root, "agents", arr);
        for (size_t i = 0; i < p->count; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", p->agents[i].name ? p->agents[i].name : "");
            cJSON_AddStringToObject(o, "role", p->agents[i].role ? p->agents[i].role : "");
            cJSON_AddItemToArray(arr, o);
        }
    }
    ca_mutex_unlock(&p->mtx);

    char *facts = ca_blackboard_snapshot_json(p->bb);
    if (root && facts) {
        cJSON *fj = cJSON_Parse(facts);
        cJSON_AddItemToObject(root, "facts", fj ? fj : cJSON_CreateObject());
    }
    free(facts);

    char *s = root ? cJSON_PrintUnformatted(root) : NULL;
    if (root) cJSON_Delete(root);
    return s ? s : ca_strdup("{}");
}
