/* blackboard.c — thread-safe shared state space for multi-agent coordination. */
#include "cognition/blackboard.h"
#include "os/os_thread.h"
#include "os/os_fs.h"
#include "infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct blackboard {
    mutex_t mtx;
    kv *items;
    size_t count;
    size_t cap;
    char *persist_path; /* NULL = no save-on-mutation */
};

static char *snapshot_locked(blackboard *b) {
    cJSON *o;
    char *s;

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
    return s ? s : xstrdup("{}");
}

/* rewrite the persist file from current state (call under mtx) */
static void save_locked(blackboard *b) {
    char *s;

    if (!b->persist_path)
        return;
    s = snapshot_locked(b);
    if (s) {
        fs_write_file(b->persist_path, s, strlen(s));
        free(s);
    }
}

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
    free(b->persist_path);
    b->persist_path = NULL;
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
            save_locked(b);
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
    save_locked(b);
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
            save_locked(b);
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

void blackboard_set_persist(blackboard *b, const char *path) {
    if (!b)
        return;
    mutex_lock(&b->mtx);
    free(b->persist_path);
    b->persist_path = path && *path ? xstrdup(path) : NULL;
    mutex_unlock(&b->mtx);
}

int blackboard_load_json(blackboard *b, const char *path) {
    int rc = -1;
    char *s;
    cJSON *root;

    if (!b || !path || !*path)
        return -1;
    s = fs_read_file(path);
    root = s ? cJSON_Parse(s) : NULL;
    free(s);
    if (!cJSON_IsObject(root))
        return -1;
    mutex_lock(&b->mtx);
    for (cJSON *it = root->child; it; it = it->next) {
        if (!it->string)
            continue;
        /* upsert (same semantics as blackboard_put) */
        size_t i;
        for (i = 0; i < b->count; i++) {
            if (strcmp(b->items[i].key, it->string) == 0)
                break;
        }
        if (i < b->count) {
            free(b->items[i].val);
            b->items[i].val = cJSON_IsString(it) && it->valuestring ? xstrdup(it->valuestring) : NULL;
            continue;
        }
        if (b->count == b->cap) {
            size_t cap = b->cap ? b->cap * 2 : 8;
            kv *nb = (kv *)realloc(b->items, cap * sizeof(kv));
            if (!nb)
                break;
            b->items = nb;
            b->cap = cap;
        }
        b->items[b->count].key = xstrdup(it->string);
        b->items[b->count].val = cJSON_IsString(it) && it->valuestring ? xstrdup(it->valuestring) : NULL;
        b->count++;
    }

    rc = 0;
    mutex_unlock(&b->mtx);
    cJSON_Delete(root);
    return rc;
}

char *blackboard_snapshot_json(blackboard *b) {
    char *s;

    if (!b)
        return xstrdup("{}");
    mutex_lock(&b->mtx);
    s = snapshot_locked(b);
    mutex_unlock(&b->mtx);
    return s;
}
