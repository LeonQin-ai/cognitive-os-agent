/* mcp_conn.c — MCP connection manager. */
#include "cagent/action/mcp_conn.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/http.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct ca_mcp_manager {
    ca_mutex mtx;
    ca_mcp_conn *items;
    size_t count, cap;
};

static long g_jsonrpc_id = 1;

static void conn_free(ca_mcp_conn *c) {
    free(c->name);
    free(c->url);
    free(c->token);
}

ca_mcp_manager *ca_mcp_manager_new(void) {
    ca_mcp_manager *m = (ca_mcp_manager *)calloc(1, sizeof(ca_mcp_manager));
    if (!m) return NULL;
    ca_mutex_init(&m->mtx);
    return m;
}

void ca_mcp_manager_free(ca_mcp_manager *m) {
    if (!m) return;
    ca_mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->count; i++) conn_free(&m->items[i]);
    free(m->items);
    ca_mutex_unlock(&m->mtx);
    ca_mutex_destroy(&m->mtx);
    free(m);
}

static int find_conn(ca_mcp_manager *m, const char *name) {
    for (size_t i = 0; i < m->count; i++)
        if (strcmp(m->items[i].name, name) == 0) return (int)i;
    return -1;
}

int ca_mcp_manager_add(ca_mcp_manager *m, const char *name, const char *url, const char *token) {
    if (!m || !name || !*name || !url || !*url) return -1;
    ca_mutex_lock(&m->mtx);
    ca_mcp_conn *e = NULL;
    int i = find_conn(m, name);
    if (i >= 0) {
        e = &m->items[i];
        free(e->url); free(e->token);
        e->url = NULL; e->token = NULL;
    } else {
        if (m->count == m->cap) {
            size_t ncap = m->cap ? m->cap * 2 : 8;
            ca_mcp_conn *ni = (ca_mcp_conn *)realloc(m->items, ncap * sizeof(*ni));
            if (!ni) { ca_mutex_unlock(&m->mtx); return -1; }
            m->items = ni;
            m->cap = ncap;
        }
        e = &m->items[m->count++];
        memset(e, 0, sizeof(*e));
        e->name = ca_strdup(name);
    }
    e->url = ca_strdup(url);
    e->token = token ? ca_strdup(token) : NULL;
    ca_mutex_unlock(&m->mtx);
    return 0;
}

int ca_mcp_manager_remove(ca_mcp_manager *m, const char *name) {
    if (!m || !name) return -1;
    ca_mutex_lock(&m->mtx);
    int i = find_conn(m, name);
    if (i < 0) { ca_mutex_unlock(&m->mtx); return -1; }
    conn_free(&m->items[i]);
    if (m->count - i - 1 > 0)
        memmove(&m->items[i], &m->items[i + 1], (m->count - i - 1) * sizeof(ca_mcp_conn));
    m->count--;
    ca_mutex_unlock(&m->mtx);
    return 0;
}

const ca_mcp_conn *ca_mcp_manager_find(ca_mcp_manager *m, const char *name) {
    if (!m || !name) return NULL;
    ca_mutex_lock(&m->mtx);
    const ca_mcp_conn *c = NULL;
    int i = find_conn(m, name);
    if (i >= 0) c = &m->items[i];
    ca_mutex_unlock(&m->mtx);
    return c;
}

int ca_mcp_manager_count(ca_mcp_manager *m) {
    if (!m) return 0;
    ca_mutex_lock(&m->mtx);
    int n = (int)m->count;
    ca_mutex_unlock(&m->mtx);
    return n;
}

const ca_mcp_conn *ca_mcp_manager_get(ca_mcp_manager *m, size_t i) {
    if (!m) return NULL;
    ca_mutex_lock(&m->mtx);
    const ca_mcp_conn *c = (i < m->count) ? &m->items[i] : NULL;
    ca_mutex_unlock(&m->mtx);
    return c;
}

/* Split a full URL into base (scheme://host:port) and path. */
static void split_url(const char *url, char *base, size_t base_sz, char *path, size_t path_sz) {
    const char *slash = strstr(url, "://");
    const char *pathstart = slash ? strchr(slash + 3, '/') : strchr(url, '/');
    if (pathstart) {
        size_t blen = (size_t)(pathstart - url);
        if (blen >= base_sz) blen = base_sz - 1;
        memcpy(base, url, blen);
        base[blen] = '\0';
        snprintf(path, path_sz, "%s", pathstart);
    } else {
        snprintf(base, base_sz, "%s", url);
        snprintf(path, path_sz, "/");
    }
}

char *ca_mcp_manager_call(ca_mcp_manager *m, const char *name,
                          const char *tool, const char *args_json) {
    if (!m || !name) return NULL;
    const ca_mcp_conn *c = ca_mcp_manager_find(m, name);
    if (!c) return NULL;

    cJSON *tool_args = cJSON_Parse(args_json ? args_json : "{}");
    if (!tool_args) tool_args = cJSON_CreateObject();

    cJSON *rpc = cJSON_CreateObject();
    cJSON_AddStringToObject(rpc, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(rpc, "id", g_jsonrpc_id++);
    cJSON *method = cJSON_AddObjectToObject(rpc, "method");
    cJSON_AddStringToObject(method, "name", tool ? tool : "");
    cJSON_AddItemToObject(method, "arguments", tool_args);

    char *body = cJSON_PrintUnformatted(rpc);
    cJSON_Delete(rpc);

    ca_strmap hdrs;
    memset(&hdrs, 0, sizeof(hdrs));
    if (c->token && *c->token) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Bearer %s", c->token);
        ca_strmap_set(&hdrs, "Authorization", auth);
    }

    char base[512], path[512];
    split_url(c->url, base, sizeof(base), path, sizeof(path));

    ca_http_response *r = ca_http_post(base, path, body, "application/json",
                                       c->token ? &hdrs : NULL, 10000);
    free(body);
    ca_strmap_free(&hdrs);
    if (!r) return NULL;
    char *out = ca_strdup(r->body ? r->body : "");
    ca_http_response_free(r);
    return out;
}

char *ca_mcp_manager_json(ca_mcp_manager *m) {
    cJSON *arr = cJSON_CreateArray();
    if (!m) return cJSON_PrintUnformatted(arr);
    ca_mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->count; i++) {
        ca_mcp_conn *e = &m->items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "url", e->url);
        cJSON_AddBoolToObject(o, "has_token", (e->token && *e->token) ? 1 : 0);
        cJSON_AddItemToArray(arr, o);
    }
    ca_mutex_unlock(&m->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}
