/* kv.c — thread-safe key/value store. */
#include "cagent/memory/kv.h"
#include "cagent/os/os_thread.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct ca_kvstore {
    ca_mutex mtx;
    ca_kv *items;
    size_t count;
    size_t cap;
};

ca_kvstore *ca_kvstore_new(void) {
    ca_kvstore *k = (ca_kvstore *)calloc(1, sizeof(*k));
    if (!k) return NULL;
    ca_mutex_init(&k->mtx);
    return k;
}

void ca_kvstore_free(ca_kvstore *k) {
    if (!k) return;
    ca_mutex_lock(&k->mtx);
    for (size_t i = 0; i < k->count; i++) { free(k->items[i].key); free(k->items[i].val); }
    free(k->items);
    k->items = NULL;
    k->count = k->cap = 0;
    ca_mutex_unlock(&k->mtx);
    ca_mutex_destroy(&k->mtx);
    free(k);
}

void ca_kvstore_set(ca_kvstore *k, const char *key, const char *val) {
    if (!k || !key || !*key) return;
    ca_mutex_lock(&k->mtx);
    for (size_t i = 0; i < k->count; i++) {
        if (strcmp(k->items[i].key, key) == 0) {
            if (!val) { /* delete */
                free(k->items[i].key);
                free(k->items[i].val);
                if (k->count - i - 1 > 0)
                    memmove(&k->items[i], &k->items[i + 1], (k->count - i - 1) * sizeof(ca_kv));
                k->count--;
            } else {
                free(k->items[i].val);
                k->items[i].val = ca_strdup(val);
            }
            ca_mutex_unlock(&k->mtx);
            return;
        }
    }
    if (!val) { ca_mutex_unlock(&k->mtx); return; } /* deleting absent key: no-op */
    if (k->count == k->cap) {
        size_t cap = k->cap ? k->cap * 2 : 8;
        ca_kv *nb = (ca_kv *)realloc(k->items, cap * sizeof(ca_kv));
        if (!nb) { ca_mutex_unlock(&k->mtx); return; }
        k->items = nb;
        k->cap = cap;
    }
    k->items[k->count].key = ca_strdup(key);
    k->items[k->count].val = ca_strdup(val);
    k->count++;
    ca_mutex_unlock(&k->mtx);
}

const char *ca_kvstore_get(ca_kvstore *k, const char *key) {
    if (!k || !key) return NULL;
    ca_mutex_lock(&k->mtx);
    const char *v = NULL;
    for (size_t i = 0; i < k->count; i++)
        if (strcmp(k->items[i].key, key) == 0) { v = k->items[i].val; break; }
    ca_mutex_unlock(&k->mtx);
    return v;
}

int ca_kvstore_remove(ca_kvstore *k, const char *key) {
    if (!k || !key) return 0;
    ca_mutex_lock(&k->mtx);
    int found = 0;
    for (size_t i = 0; i < k->count; i++) {
        if (strcmp(k->items[i].key, key) == 0) {
            free(k->items[i].key);
            free(k->items[i].val);
            if (k->count - i - 1 > 0)
                memmove(&k->items[i], &k->items[i + 1], (k->count - i - 1) * sizeof(ca_kv));
            k->count--;
            found = 1;
            break;
        }
    }
    ca_mutex_unlock(&k->mtx);
    return found;
}

int ca_kvstore_count(ca_kvstore *k) {
    if (!k) return 0;
    ca_mutex_lock(&k->mtx);
    int n = (int)k->count;
    ca_mutex_unlock(&k->mtx);
    return n;
}

char *ca_kvstore_snapshot_json(ca_kvstore *k) {
    if (!k) return ca_strdup("{}");
    ca_mutex_lock(&k->mtx);
    cJSON *o = cJSON_CreateObject();
    if (o)
        for (size_t i = 0; i < k->count; i++)
            cJSON_AddStringToObject(o, k->items[i].key, k->items[i].val ? k->items[i].val : "");
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    if (o) cJSON_Delete(o);
    ca_mutex_unlock(&k->mtx);
    return s ? s : ca_strdup("{}");
}
