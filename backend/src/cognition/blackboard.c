/* blackboard.c — thread-safe shared state space for multi-agent coordination. */
#include "cagent/cognition/blackboard.h"
#include "cagent/os/os_thread.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct ca_blackboard {
    ca_mutex mtx;
    ca_kv *items;
    size_t count;
    size_t cap;
};

ca_blackboard *ca_blackboard_new(void) {
    ca_blackboard *b = (ca_blackboard *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    ca_mutex_init(&b->mtx);
    return b;
}

void ca_blackboard_free(ca_blackboard *b) {
    if (!b) return;
    ca_mutex_lock(&b->mtx);
    for (size_t i = 0; i < b->count; i++) {
        free(b->items[i].key);
        free(b->items[i].val);
    }
    free(b->items);
    b->items = NULL;
    b->count = b->cap = 0;
    ca_mutex_unlock(&b->mtx);
    ca_mutex_destroy(&b->mtx);
    free(b);
}

void ca_blackboard_put(ca_blackboard *b, const char *key, const char *val) {
    if (!b || !key) return;
    ca_mutex_lock(&b->mtx);
    for (size_t i = 0; i < b->count; i++) {
        if (strcmp(b->items[i].key, key) == 0) {
            free(b->items[i].val);
            b->items[i].val = val ? ca_strdup(val) : NULL;
            ca_mutex_unlock(&b->mtx);
            return;
        }
    }
    if (b->count == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8;
        ca_kv *nb = (ca_kv *)realloc(b->items, cap * sizeof(ca_kv));
        if (!nb) { ca_mutex_unlock(&b->mtx); return; }
        b->items = nb;
        b->cap = cap;
    }
    b->items[b->count].key = ca_strdup(key);
    b->items[b->count].val = val ? ca_strdup(val) : NULL;
    b->count++;
    ca_mutex_unlock(&b->mtx);
}

char *ca_blackboard_get(ca_blackboard *b, const char *key) {
    if (!b || !key) return NULL;
    ca_mutex_lock(&b->mtx);
    char *r = NULL;
    for (size_t i = 0; i < b->count; i++) {
        if (strcmp(b->items[i].key, key) == 0) {
            r = b->items[i].val ? ca_strdup(b->items[i].val) : NULL;
            break;
        }
    }
    ca_mutex_unlock(&b->mtx);
    return r;
}

int ca_blackboard_remove(ca_blackboard *b, const char *key) {
    if (!b || !key) return 0;
    ca_mutex_lock(&b->mtx);
    int found = 0;
    for (size_t i = 0; i < b->count; i++) {
        if (strcmp(b->items[i].key, key) == 0) {
            free(b->items[i].key);
            free(b->items[i].val);
            if (b->count - i - 1 > 0)
                memmove(&b->items[i], &b->items[i + 1],
                        (b->count - i - 1) * sizeof(ca_kv));
            b->count--;
            found = 1;
            break;
        }
    }
    ca_mutex_unlock(&b->mtx);
    return found;
}

int ca_blackboard_count(ca_blackboard *b) {
    if (!b) return 0;
    ca_mutex_lock(&b->mtx);
    int n = (int)b->count;
    ca_mutex_unlock(&b->mtx);
    return n;
}

char *ca_blackboard_snapshot_json(ca_blackboard *b) {
    if (!b) return ca_strdup("{}");
    ca_mutex_lock(&b->mtx);
    cJSON *o = cJSON_CreateObject();
    if (o) {
        for (size_t i = 0; i < b->count; i++) {
            if (b->items[i].key)
                cJSON_AddStringToObject(o, b->items[i].key,
                                        b->items[i].val ? b->items[i].val : "");
        }
    }
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    if (o) cJSON_Delete(o);
    ca_mutex_unlock(&b->mtx);
    return s ? s : ca_strdup("{}");
}
