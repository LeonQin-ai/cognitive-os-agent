/* node.c — cluster node registry. */
#include "cognitive-os-agent/cluster/node.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct coa_cluster {
    coa_mutex mtx;
    coa_cluster_node *items;
    size_t count, cap;
};

static void node_free(coa_cluster_node *n) {
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

coa_cluster *coa_cluster_new(void) {
    coa_cluster *c = (coa_cluster *)calloc(1, sizeof(coa_cluster));
    if (!c) return NULL;
    coa_mutex_init(&c->mtx);
    return c;
}

void coa_cluster_free(coa_cluster *c) {
    if (!c) return;
    coa_mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++) node_free(&c->items[i]);
    free(c->items);
    coa_mutex_unlock(&c->mtx);
    coa_mutex_destroy(&c->mtx);
    free(c);
}

static int find_node(coa_cluster *c, const char *id) {
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->items[i].id, id) == 0) return (int)i;
    return -1;
}

int coa_cluster_upsert(coa_cluster *c, const char *id, const char *host,
                      uint16_t port, const char *role) {
    return coa_cluster_upsert_ex(c, id, host, port, role, NULL);
}

int coa_cluster_upsert_ex(coa_cluster *c, const char *id, const char *host,
                         uint16_t port, const char *role, const char *caps) {
    if (!c || !id || !*id || !host || !*host) return -1;
    const char *r = valid_role(role);
    coa_mutex_lock(&c->mtx);
    coa_cluster_node *e = NULL;
    int i = find_node(c, id);
    if (i < 0) {
        if (c->count == c->cap) {
            size_t ncap = c->cap ? c->cap * 2 : 8;
            coa_cluster_node *ni = (coa_cluster_node *)realloc(c->items, ncap * sizeof(*ni));
            if (!ni) { coa_mutex_unlock(&c->mtx); return -1; }
            c->items = ni;
            c->cap = ncap;
        }
        e = &c->items[c->count++];
        memset(e, 0, sizeof(*e));
        e->id = coa_strdup(id);
        e->status = coa_strdup("up");
        e->last_seen_ms = coa_time_now_ms();
    } else {
        e = &c->items[i];
        free(e->host); free(e->role); free(e->caps);
        e->host = NULL; e->role = NULL; e->caps = NULL;
    }
    e->host = coa_strdup(host);
    e->port = port;
    e->role = coa_strdup(r);
    e->caps = coa_strdup(caps && *caps ? caps : "");
    coa_mutex_unlock(&c->mtx);
    return 0;
}

int coa_cluster_remove(coa_cluster *c, const char *id) {
    if (!c || !id) return -1;
    coa_mutex_lock(&c->mtx);
    int i = find_node(c, id);
    if (i < 0) { coa_mutex_unlock(&c->mtx); return -1; }
    node_free(&c->items[i]);
    if (c->count - i - 1 > 0)
        memmove(&c->items[i], &c->items[i + 1], (c->count - i - 1) * sizeof(coa_cluster_node));
    c->count--;
    coa_mutex_unlock(&c->mtx);
    return 0;
}

int coa_cluster_heartbeat(coa_cluster *c, const char *id) {
    if (!c || !id) return -1;
    coa_mutex_lock(&c->mtx);
    int i = find_node(c, id);
    if (i < 0) { coa_mutex_unlock(&c->mtx); return -1; }
    c->items[i].last_seen_ms = coa_time_now_ms();
    free(c->items[i].status);
    c->items[i].status = coa_strdup("up");
    coa_mutex_unlock(&c->mtx);
    return 0;
}

void coa_cluster_mark_down(coa_cluster *c, int64_t stale_ms) {
    if (!c) return;
    int64_t now = coa_time_now_ms();
    coa_mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++) {
        coa_cluster_node *n = &c->items[i];
        if (now - n->last_seen_ms > stale_ms && strcmp(n->status, "up") == 0) {
            free(n->status);
            n->status = coa_strdup("down");
        }
    }
    coa_mutex_unlock(&c->mtx);
}

const coa_cluster_node *coa_cluster_find(coa_cluster *c, const char *id) {
    if (!c || !id) return NULL;
    coa_mutex_lock(&c->mtx);
    const coa_cluster_node *n = NULL;
    int i = find_node(c, id);
    if (i >= 0) n = &c->items[i];
    coa_mutex_unlock(&c->mtx);
    return n;
}

int coa_cluster_count(coa_cluster *c) {
    if (!c) return 0;
    coa_mutex_lock(&c->mtx);
    int n = (int)c->count;
    coa_mutex_unlock(&c->mtx);
    return n;
}

const coa_cluster_node *coa_cluster_get(coa_cluster *c, size_t i) {
    if (!c) return NULL;
    coa_mutex_lock(&c->mtx);
    const coa_cluster_node *n = (i < c->count) ? &c->items[i] : NULL;
    coa_mutex_unlock(&c->mtx);
    return n;
}

int coa_cluster_up_count(coa_cluster *c) {
    if (!c) return 0;
    coa_mutex_lock(&c->mtx);
    int up = 0;
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->items[i].status, "up") == 0) up++;
    coa_mutex_unlock(&c->mtx);
    return up;
}

char *coa_cluster_json(coa_cluster *c) {
    cJSON *arr = cJSON_CreateArray();
    if (!c) return cJSON_PrintUnformatted(arr);
    coa_mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++) {
        coa_cluster_node *n = &c->items[i];
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
    coa_mutex_unlock(&c->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}
