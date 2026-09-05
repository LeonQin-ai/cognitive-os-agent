/* blackboard.c — thread-safe shared state space for multi-agent coordination. */
#include "cognitive-os-agent/cognition/blackboard.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct coa_blackboard {
    coa_mutex mtx;
    coa_kv *items;
    size_t count;
    size_t cap;
};

coa_blackboard *coa_blackboard_new(void) {
    coa_blackboard *b = (coa_blackboard *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    coa_mutex_init(&b->mtx);
    return b;
}

void coa_blackboard_free(coa_blackboard *b) {
    if (!b) return;
    coa_mutex_lock(&b->mtx);
    for (size_t i = 0; i < b->count; i++) {
        free(b->items[i].key);
        free(b->items[i].val);
    }
    free(b->items);
    b->items = NULL;
    b->count = b->cap = 0;
    coa_mutex_unlock(&b->mtx);
    coa_mutex_destroy(&b->mtx);
    free(b);
}

void coa_blackboard_put(coa_blackboard *b, const char *key, const char *val) {
    if (!b || !key) return;
    coa_mutex_lock(&b->mtx);
    for (size_t i = 0; i < b->count; i++) {
        if (strcmp(b->items[i].key, key) == 0) {
            free(b->items[i].val);
            b->items[i].val = val ? coa_strdup(val) : NULL;
            coa_mutex_unlock(&b->mtx);
            return;
        }
    }
    if (b->count == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8;
        coa_kv *nb = (coa_kv *)realloc(b->items, cap * sizeof(coa_kv));
        if (!nb) { coa_mutex_unlock(&b->mtx); return; }
        b->items = nb;
        b->cap = cap;
    }
    b->items[b->count].key = coa_strdup(key);
    b->items[b->count].val = val ? coa_strdup(val) : NULL;
    b->count++;
    coa_mutex_unlock(&b->mtx);
}

char *coa_blackboard_get(coa_blackboard *b, const char *key) {
    if (!b || !key) return NULL;
    coa_mutex_lock(&b->mtx);
    char *r = NULL;
    for (size_t i = 0; i < b->count; i++) {
        if (strcmp(b->items[i].key, key) == 0) {
            r = b->items[i].val ? coa_strdup(b->items[i].val) : NULL;
            break;
        }
    }
    coa_mutex_unlock(&b->mtx);
    return r;
}

int coa_blackboard_remove(coa_blackboard *b, const char *key) {
    if (!b || !key) return 0;
    coa_mutex_lock(&b->mtx);
    int found = 0;
    for (size_t i = 0; i < b->count; i++) {
        if (strcmp(b->items[i].key, key) == 0) {
            free(b->items[i].key);
            free(b->items[i].val);
            if (b->count - i - 1 > 0)
                memmove(&b->items[i], &b->items[i + 1],
                        (b->count - i - 1) * sizeof(coa_kv));
            b->count--;
            found = 1;
            break;
        }
    }
    coa_mutex_unlock(&b->mtx);
    return found;
}

int coa_blackboard_count(coa_blackboard *b) {
    if (!b) return 0;
    coa_mutex_lock(&b->mtx);
    int n = (int)b->count;
    coa_mutex_unlock(&b->mtx);
    return n;
}

char *coa_blackboard_snapshot_json(coa_blackboard *b) {
    if (!b) return coa_strdup("{}");
    coa_mutex_lock(&b->mtx);
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
    coa_mutex_unlock(&b->mtx);
    return s ? s : coa_strdup("{}");
}
