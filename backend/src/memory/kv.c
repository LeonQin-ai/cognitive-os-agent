/* kv.c — thread-safe key/value store. */
#include "cognitive-os-agent/memory/kv.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct coa_kvstore {
    coa_mutex mtx;
    coa_kv *items;
    size_t count;
    size_t cap;
};

coa_kvstore *coa_kvstore_new(void) {
    coa_kvstore *k = (coa_kvstore *)calloc(1, sizeof(*k));
    if (!k) return NULL;
    coa_mutex_init(&k->mtx);
    return k;
}

void coa_kvstore_free(coa_kvstore *k) {
    if (!k) return;
    coa_mutex_lock(&k->mtx);
    for (size_t i = 0; i < k->count; i++) { free(k->items[i].key); free(k->items[i].val); }
    free(k->items);
    k->items = NULL;
    k->count = k->cap = 0;
    coa_mutex_unlock(&k->mtx);
    coa_mutex_destroy(&k->mtx);
    free(k);
}

void coa_kvstore_set(coa_kvstore *k, const char *key, const char *val) {
    if (!k || !key || !*key) return;
    coa_mutex_lock(&k->mtx);
    for (size_t i = 0; i < k->count; i++) {
        if (strcmp(k->items[i].key, key) == 0) {
            if (!val) { /* delete */
                free(k->items[i].key);
                free(k->items[i].val);
                if (k->count - i - 1 > 0)
                    memmove(&k->items[i], &k->items[i + 1], (k->count - i - 1) * sizeof(coa_kv));
                k->count--;
            } else {
                free(k->items[i].val);
                k->items[i].val = coa_strdup(val);
            }
            coa_mutex_unlock(&k->mtx);
            return;
        }
    }
    if (!val) { coa_mutex_unlock(&k->mtx); return; } /* deleting absent key: no-op */
    if (k->count == k->cap) {
        size_t cap = k->cap ? k->cap * 2 : 8;
        coa_kv *nb = (coa_kv *)realloc(k->items, cap * sizeof(coa_kv));
        if (!nb) { coa_mutex_unlock(&k->mtx); return; }
        k->items = nb;
        k->cap = cap;
    }
    k->items[k->count].key = coa_strdup(key);
    k->items[k->count].val = coa_strdup(val);
    k->count++;
    coa_mutex_unlock(&k->mtx);
}

const char *coa_kvstore_get(coa_kvstore *k, const char *key) {
    if (!k || !key) return NULL;
    coa_mutex_lock(&k->mtx);
    const char *v = NULL;
    for (size_t i = 0; i < k->count; i++)
        if (strcmp(k->items[i].key, key) == 0) { v = k->items[i].val; break; }
    coa_mutex_unlock(&k->mtx);
    return v;
}

int coa_kvstore_remove(coa_kvstore *k, const char *key) {
    if (!k || !key) return 0;
    coa_mutex_lock(&k->mtx);
    int found = 0;
    for (size_t i = 0; i < k->count; i++) {
        if (strcmp(k->items[i].key, key) == 0) {
            free(k->items[i].key);
            free(k->items[i].val);
            if (k->count - i - 1 > 0)
                memmove(&k->items[i], &k->items[i + 1], (k->count - i - 1) * sizeof(coa_kv));
            k->count--;
            found = 1;
            break;
        }
    }
    coa_mutex_unlock(&k->mtx);
    return found;
}

int coa_kvstore_count(coa_kvstore *k) {
    if (!k) return 0;
    coa_mutex_lock(&k->mtx);
    int n = (int)k->count;
    coa_mutex_unlock(&k->mtx);
    return n;
}

char *coa_kvstore_snapshot_json(coa_kvstore *k) {
    if (!k) return coa_strdup("{}");
    coa_mutex_lock(&k->mtx);
    cJSON *o = cJSON_CreateObject();
    if (o)
        for (size_t i = 0; i < k->count; i++)
            cJSON_AddStringToObject(o, k->items[i].key, k->items[i].val ? k->items[i].val : "");
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    if (o) cJSON_Delete(o);
    coa_mutex_unlock(&k->mtx);
    return s ? s : coa_strdup("{}");
}
