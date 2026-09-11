/* node.c — cluster node registry. */
#include "cognitive-os-agent/cluster/node.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct cluster {
    mutex_t mtx;
    cluster_node *items;
    size_t count, cap;
};

static void node_free(cluster_node *n) {
    free(n->id);
    free(n->host);
    free(n->role);
    free(n->status);
    free(n->caps);
}

static const char *valid_role(const char *role) {
    if (!role)
        return "worker";
    if (strcmp(role, "coordinator") == 0)
        return "coordinator";
    if (strcmp(role, "observer") == 0)
        return "observer";
    return "worker";
}

cluster *cluster_new(void) {
    cluster *c = (cluster *)calloc(1, sizeof(cluster));
    if (!c)
        return NULL;
    mutex_init(&c->mtx);
    return c;
}

void cluster_free(cluster *c) {
    if (!c)
        return;
    mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++)
        node_free(&c->items[i]);
    free(c->items);
    mutex_unlock(&c->mtx);
    mutex_destroy(&c->mtx);
    free(c);
}

static int find_node(cluster *c, const char *id) {
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->items[i].id, id) == 0)
            return (int)i;
    return -1;
}

int cluster_upsert(cluster *c, const char *id, const char *host, uint16_t port, const char *role) {
    return cluster_upsert_ex(c, id, host, port, role, NULL);
}

int cluster_upsert_ex(cluster *c, const char *id, const char *host, uint16_t port, const char *role,
                          const char *caps) {
    if (!c || !id || !*id || !host || !*host)
        return -1;
    const char *r = valid_role(role);
    mutex_lock(&c->mtx);
    cluster_node *e = NULL;
    int i = find_node(c, id);
    if (i < 0) {
        if (c->count == c->cap) {
            size_t ncap = c->cap ? c->cap * 2 : 8;
            cluster_node *ni = (cluster_node *)realloc(c->items, ncap * sizeof(*ni));
            if (!ni) {
                mutex_unlock(&c->mtx);
                return -1;
            }
            c->items = ni;
            c->cap = ncap;
        }
        e = &c->items[c->count++];
        memset(e, 0, sizeof(*e));
        e->id = xstrdup(id);
        e->status = xstrdup("up");
        e->last_seen_ms = time_now_ms();
    } else {
        e = &c->items[i];
        free(e->host);
        free(e->role);
        free(e->caps);
        e->host = NULL;
        e->role = NULL;
        e->caps = NULL;
    }
    e->host = xstrdup(host);
    e->port = port;
    e->role = xstrdup(r);
    e->caps = xstrdup(caps && *caps ? caps : "");
    mutex_unlock(&c->mtx);
    return 0;
}

int cluster_remove(cluster *c, const char *id) {
    if (!c || !id)
        return -1;
    mutex_lock(&c->mtx);
    int i = find_node(c, id);
    if (i < 0) {
        mutex_unlock(&c->mtx);
        return -1;
    }
    node_free(&c->items[i]);
    if (c->count - i - 1 > 0)
        memmove(&c->items[i], &c->items[i + 1], (c->count - i - 1) * sizeof(cluster_node));
    c->count--;
    mutex_unlock(&c->mtx);
    return 0;
}

int cluster_heartbeat(cluster *c, const char *id) {
    if (!c || !id)
        return -1;
    mutex_lock(&c->mtx);
    int i = find_node(c, id);
    if (i < 0) {
        mutex_unlock(&c->mtx);
        return -1;
    }
    c->items[i].last_seen_ms = time_now_ms();
    free(c->items[i].status);
    c->items[i].status = xstrdup("up");
    mutex_unlock(&c->mtx);
    return 0;
}

void cluster_mark_down(cluster *c, int64_t stale_ms) {
    if (!c)
        return;
    int64_t now = time_now_ms();
    mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++) {
        cluster_node *n = &c->items[i];
        if (now - n->last_seen_ms > stale_ms && strcmp(n->status, "up") == 0) {
            free(n->status);
            n->status = xstrdup("down");
        }
    }
    mutex_unlock(&c->mtx);
}

const cluster_node *cluster_find(cluster *c, const char *id) {
    if (!c || !id)
        return NULL;
    mutex_lock(&c->mtx);
    const cluster_node *n = NULL;
    int i = find_node(c, id);
    if (i >= 0)
        n = &c->items[i];
    mutex_unlock(&c->mtx);
    return n;
}

int cluster_count(cluster *c) {
    if (!c)
        return 0;
    mutex_lock(&c->mtx);
    int n = (int)c->count;
    mutex_unlock(&c->mtx);
    return n;
}

int cluster_up_count(cluster *c) {
    if (!c)
        return 0;
    mutex_lock(&c->mtx);
    int up = 0;
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->items[i].status, "up") == 0)
            up++;
    mutex_unlock(&c->mtx);
    return up;
}

char *cluster_json(cluster *c) {
    cJSON *arr = cJSON_CreateArray();
    if (!c)
        return cJSON_PrintUnformatted(arr);
    mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++) {
        cluster_node *n = &c->items[i];
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
    mutex_unlock(&c->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}
