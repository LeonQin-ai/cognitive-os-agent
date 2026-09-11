/* kv.c — thread-safe key/value store. */
#include "cognitive-os-agent/memory/kv.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct kvstore {
    mutex_t mtx;
    kv *items;
    size_t count;
    size_t cap;
};

kvstore *kvstore_new(void) {
    kvstore *k = (kvstore *)calloc(1, sizeof(*k));
    if (!k)
        return NULL;
    mutex_init(&k->mtx);
    return k;
}

void kvstore_free(kvstore *k) {
    if (!k)
        return;
    mutex_lock(&k->mtx);
    for (size_t i = 0; i < k->count; i++) {
        free(k->items[i].key);
        free(k->items[i].val);
    }
    free(k->items);
    k->items = NULL;
    k->count = k->cap = 0;
    mutex_unlock(&k->mtx);
    mutex_destroy(&k->mtx);
    free(k);
}

void kvstore_set(kvstore *k, const char *key, const char *val) {
    if (!k || !key || !*key)
        return;
    mutex_lock(&k->mtx);
    for (size_t i = 0; i < k->count; i++) {
        if (strcmp(k->items[i].key, key) == 0) {
            if (!val) { /* delete */
                free(k->items[i].key);
                free(k->items[i].val);
                if (k->count - i - 1 > 0)
                    memmove(&k->items[i], &k->items[i + 1], (k->count - i - 1) * sizeof(kv));
                k->count--;
            } else {
                free(k->items[i].val);
                k->items[i].val = xstrdup(val);
            }
            mutex_unlock(&k->mtx);
            return;
        }
    }
    if (!val) {
        mutex_unlock(&k->mtx);
        return;
    } /* deleting absent key: no-op */
    if (k->count == k->cap) {
        size_t cap = k->cap ? k->cap * 2 : 8;
        kv *nb = (kv *)realloc(k->items, cap * sizeof(kv));
        if (!nb) {
            mutex_unlock(&k->mtx);
            return;
        }
        k->items = nb;
        k->cap = cap;
    }
    k->items[k->count].key = xstrdup(key);
    k->items[k->count].val = xstrdup(val);
    k->count++;
    mutex_unlock(&k->mtx);
}

const char *kvstore_get(kvstore *k, const char *key) {
    if (!k || !key)
        return NULL;
    mutex_lock(&k->mtx);
    const char *v = NULL;
    for (size_t i = 0; i < k->count; i++)
        if (strcmp(k->items[i].key, key) == 0) {
            v = k->items[i].val;
            break;
        }
    mutex_unlock(&k->mtx);
    return v;
}

int kvstore_remove(kvstore *k, const char *key) {
    if (!k || !key)
        return 0;
    mutex_lock(&k->mtx);
    int found = 0;
    for (size_t i = 0; i < k->count; i++) {
        if (strcmp(k->items[i].key, key) == 0) {
            free(k->items[i].key);
            free(k->items[i].val);
            if (k->count - i - 1 > 0)
                memmove(&k->items[i], &k->items[i + 1], (k->count - i - 1) * sizeof(kv));
            k->count--;
            found = 1;
            break;
        }
    }
    mutex_unlock(&k->mtx);
    return found;
}

int kvstore_count(kvstore *k) {
    if (!k)
        return 0;
    mutex_lock(&k->mtx);
    int n = (int)k->count;
    mutex_unlock(&k->mtx);
    return n;
}

char *kvstore_snapshot_json(kvstore *k) {
    if (!k)
        return xstrdup("{}");
    mutex_lock(&k->mtx);
    cJSON *o = cJSON_CreateObject();
    if (o)
        for (size_t i = 0; i < k->count; i++)
            cJSON_AddStringToObject(o, k->items[i].key, k->items[i].val ? k->items[i].val : "");
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    if (o)
        cJSON_Delete(o);
    mutex_unlock(&k->mtx);
    return s ? s : xstrdup("{}");
}
