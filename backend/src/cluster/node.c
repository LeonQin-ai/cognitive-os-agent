/* node.c — cluster node registry. */
#include "cagent/cluster/node.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/os_time.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct ca_cluster {
    ca_mutex mtx;
    ca_cluster_node *items;
    size_t count, cap;
};

static void node_free(ca_cluster_node *n) {
    free(n->id);
    free(n->host);
    free(n->role);
    free(n->status);
    free(n->caps);
}

static const char *valid_role(const char *role) {
    if (!role) return "worker";
    if (strcmp(role, "coordinator") == 0) return "coordinator";
    if (strcmp(role, "observer") == 0) return "observer";
    return "worker";
}

ca_cluster *ca_cluster_new(void) {
    ca_cluster *c = (ca_cluster *)calloc(1, sizeof(ca_cluster));
    if (!c) return NULL;
    ca_mutex_init(&c->mtx);
    return c;
}

void ca_cluster_free(ca_cluster *c) {
    if (!c) return;
    ca_mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++) node_free(&c->items[i]);
    free(c->items);
    ca_mutex_unlock(&c->mtx);
    ca_mutex_destroy(&c->mtx);
    free(c);
}

static int find_node(ca_cluster *c, const char *id) {
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->items[i].id, id) == 0) return (int)i;
    return -1;
}

int ca_cluster_upsert(ca_cluster *c, const char *id, const char *host,
                      uint16_t port, const char *role) {
    return ca_cluster_upsert_ex(c, id, host, port, role, NULL);
}

int ca_cluster_upsert_ex(ca_cluster *c, const char *id, const char *host,
                         uint16_t port, const char *role, const char *caps) {
    if (!c || !id || !*id || !host || !*host) return -1;
    const char *r = valid_role(role);
    ca_mutex_lock(&c->mtx);
    ca_cluster_node *e = NULL;
    int i = find_node(c, id);
    if (i < 0) {
        if (c->count == c->cap) {
            size_t ncap = c->cap ? c->cap * 2 : 8;
            ca_cluster_node *ni = (ca_cluster_node *)realloc(c->items, ncap * sizeof(*ni));
            if (!ni) { ca_mutex_unlock(&c->mtx); return -1; }
            c->items = ni;
            c->cap = ncap;
        }
        e = &c->items[c->count++];
        memset(e, 0, sizeof(*e));
        e->id = ca_strdup(id);
        e->status = ca_strdup("up");
        e->last_seen_ms = ca_time_now_ms();
    } else {
        e = &c->items[i];
        free(e->host); free(e->role); free(e->caps);
        e->host = NULL; e->role = NULL; e->caps = NULL;
    }
    e->host = ca_strdup(host);
    e->port = port;
    e->role = ca_strdup(r);
    e->caps = ca_strdup(caps && *caps ? caps : "");
    ca_mutex_unlock(&c->mtx);
    return 0;
}

int ca_cluster_remove(ca_cluster *c, const char *id) {
    if (!c || !id) return -1;
    ca_mutex_lock(&c->mtx);
    int i = find_node(c, id);
    if (i < 0) { ca_mutex_unlock(&c->mtx); return -1; }
    node_free(&c->items[i]);
    if (c->count - i - 1 > 0)
        memmove(&c->items[i], &c->items[i + 1], (c->count - i - 1) * sizeof(ca_cluster_node));
    c->count--;
    ca_mutex_unlock(&c->mtx);
    return 0;
}

int ca_cluster_heartbeat(ca_cluster *c, const char *id) {
    if (!c || !id) return -1;
    ca_mutex_lock(&c->mtx);
    int i = find_node(c, id);
    if (i < 0) { ca_mutex_unlock(&c->mtx); return -1; }
    c->items[i].last_seen_ms = ca_time_now_ms();
    free(c->items[i].status);
    c->items[i].status = ca_strdup("up");
    ca_mutex_unlock(&c->mtx);
    return 0;
}

void ca_cluster_mark_down(ca_cluster *c, int64_t stale_ms) {
    if (!c) return;
    int64_t now = ca_time_now_ms();
    ca_mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++) {
        ca_cluster_node *n = &c->items[i];
        if (now - n->last_seen_ms > stale_ms && strcmp(n->status, "up") == 0) {
            free(n->status);
            n->status = ca_strdup("down");
        }
    }
    ca_mutex_unlock(&c->mtx);
}

const ca_cluster_node *ca_cluster_find(ca_cluster *c, const char *id) {
    if (!c || !id) return NULL;
    ca_mutex_lock(&c->mtx);
    const ca_cluster_node *n = NULL;
    int i = find_node(c, id);
    if (i >= 0) n = &c->items[i];
    ca_mutex_unlock(&c->mtx);
    return n;
}

int ca_cluster_count(ca_cluster *c) {
    if (!c) return 0;
    ca_mutex_lock(&c->mtx);
    int n = (int)c->count;
    ca_mutex_unlock(&c->mtx);
    return n;
}

const ca_cluster_node *ca_cluster_get(ca_cluster *c, size_t i) {
    if (!c) return NULL;
    ca_mutex_lock(&c->mtx);
    const ca_cluster_node *n = (i < c->count) ? &c->items[i] : NULL;
    ca_mutex_unlock(&c->mtx);
    return n;
}

int ca_cluster_up_count(ca_cluster *c) {
    if (!c) return 0;
    ca_mutex_lock(&c->mtx);
    int up = 0;
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->items[i].status, "up") == 0) up++;
    ca_mutex_unlock(&c->mtx);
    return up;
}

char *ca_cluster_json(ca_cluster *c) {
    cJSON *arr = cJSON_CreateArray();
    if (!c) return cJSON_PrintUnformatted(arr);
    ca_mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++) {
        ca_cluster_node *n = &c->items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", n->id);
        cJSON_AddStringToObject(o, "host", n->host);
        cJSON_AddNumberToObject(o, "port", (double)n->port);
        cJSON_AddStringToObject(o, "role", n->role);
        cJSON_AddStringToObject(o, "status", n->status);
        cJSON_AddStringToObject(o, "caps", n->caps ? n->caps : "");
        cJSON_AddNumberToObject(o, "last_seen_ms", (double)n->last_seen_ms);
        cJSON_AddItemToArray(arr, o);
    }
    ca_mutex_unlock(&c->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}
