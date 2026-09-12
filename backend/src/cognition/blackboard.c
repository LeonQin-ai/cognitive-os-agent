/* blackboard.c — thread-safe shared state space for multi-agent coordination. */
#include "cognitive-os-agent/cognition/blackboard.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct blackboard {
    mutex_t mtx;
    kv *items;
    size_t count;
    size_t cap;
};

blackboard *blackboard_new(void) {
    blackboard *b = (blackboard *)calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    mutex_init(&b->mtx);
    return b;
}

void blackboard_free(blackboard *b) {
    if (!b)
        return;
    mutex_lock(&b->mtx);
    for (size_t i = 0; i < b->count; i++) {
        free(b->items[i].key);
        free(b->items[i].val);
    }

    free(b->items);
    b->items = NULL;
    b->count = b->cap = 0;
    mutex_unlock(&b->mtx);
    mutex_destroy(&b->mtx);
    free(b);
}

void blackboard_put(blackboard *b, const char *key, const char *val) {
    if (!b || !key)
        return;
    mutex_lock(&b->mtx);
    for (size_t i = 0; i < b->count; i++) {
        if (strcmp(b->items[i].key, key) == 0) {
            free(b->items[i].val);
            b->items[i].val = val ? xstrdup(val) : NULL;
            mutex_unlock(&b->mtx);
            return;
        }
    }

    if (b->count == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8;
        kv *nb = (kv *)realloc(b->items, cap * sizeof(kv));
        if (!nb) {
            mutex_unlock(&b->mtx);
            return;
        }
        b->items = nb;
        b->cap = cap;
    }

    b->items[b->count].key = xstrdup(key);
    b->items[b->count].val = val ? xstrdup(val) : NULL;
    b->count++;
    mutex_unlock(&b->mtx);
}

char *blackboard_get(blackboard *b, const char *key) {
    char *r = NULL;

    if (!b || !key)
        return NULL;
    mutex_lock(&b->mtx);
    for (size_t i = 0; i < b->count; i++) {
        if (strcmp(b->items[i].key, key) == 0) {
            r = b->items[i].val ? xstrdup(b->items[i].val) : NULL;
            break;
        }
    }

    mutex_unlock(&b->mtx);
    return r;
}

int blackboard_remove(blackboard *b, const char *key) {
    int found = 0;

    if (!b || !key)
        return 0;
    mutex_lock(&b->mtx);
    for (size_t i = 0; i < b->count; i++) {
        if (strcmp(b->items[i].key, key) == 0) {
            free(b->items[i].key);
            free(b->items[i].val);
            if (b->count - i - 1 > 0)
                memmove(&b->items[i], &b->items[i + 1], (b->count - i - 1) * sizeof(kv));
            b->count--;
            found = 1;
            break;
        }
    }

    mutex_unlock(&b->mtx);
    return found;
}

int blackboard_count(blackboard *b) {
    int n;

    if (!b)
        return 0;
    mutex_lock(&b->mtx);
    n = (int)b->count;
    mutex_unlock(&b->mtx);
    return n;
}

char *blackboard_snapshot_json(blackboard *b) {
    cJSON *o;
    char *s;

    if (!b)
        return xstrdup("{}");
    mutex_lock(&b->mtx);
    o = cJSON_CreateObject();
    if (o) {
        for (size_t i = 0; i < b->count; i++) {
            if (b->items[i].key)
                cJSON_AddStringToObject(o, b->items[i].key, b->items[i].val ? b->items[i].val : "");
        }
    }

    s = o ? cJSON_PrintUnformatted(o) : NULL;
    if (o)
        cJSON_Delete(o);
    mutex_unlock(&b->mtx);
    return s ? s : xstrdup("{}");
}
