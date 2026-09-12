/* state_store.c — Context layer: unified KV / Task / Agent state store. */
#include "cognitive-os-agent/runtime/state_store.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/infra/logging.h"
#include "cognitive-os-agent/os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

typedef struct ss_entry {
    char *ns;
    char *key;
    char *val;
} ss_entry;

struct state_store {
    mutex_t mtx;
    ss_entry *items;
    size_t count, cap;
    char *path; /* set by save/load; enables auto-flush */
};

state_store *state_store_new(void) {
    state_store *s = (state_store *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    mutex_init(&s->mtx);
    return s;
}

void state_store_free(state_store *s) {
    if (!s)
        return;
    mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++) {
        free(s->items[i].ns);
        free(s->items[i].key);
        free(s->items[i].val);
    }

    free(s->items);
    free(s->path);
    mutex_unlock(&s->mtx);
    mutex_destroy(&s->mtx);
    free(s);
}

static long ss_find(state_store *s, const char *ns, const char *key) {
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->items[i].ns, ns) == 0 && strcmp(s->items[i].key, key) == 0)
            return (long)i;
    return -1;
}

/* Caller holds mtx. 1 = mutated (needs flush). */
static int ss_put(state_store *s, const char *ns, const char *key, const char *val) {
    long i = ss_find(s, ns, key);
    if (i >= 0) {
        if (!val) { /* remove */
            free(s->items[i].ns);
            free(s->items[i].key);
            free(s->items[i].val);
            memmove(&s->items[i], &s->items[i + 1], (s->count - (size_t)i - 1) * sizeof(ss_entry));
            s->count--;
            return 1;
        }
        if (strcmp(s->items[i].val, val) == 0)
            return 0;
        char *nv = xstrdup(val);
        if (!nv)
            return 0;
        free(s->items[i].val);
        s->items[i].val = nv;
        return 1;
    }

    if (!val)
        return 0;
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        ss_entry *ni = (ss_entry *)realloc(s->items, cap * sizeof(ss_entry));
        if (!ni)
            return 0;
        s->items = ni;
        s->cap = cap;
    }

    ss_entry *e = &s->items[s->count++];
    memset(e, 0, sizeof(*e));
    e->ns = xstrdup(ns);
    e->key = xstrdup(key);
    e->val = xstrdup(val);
    if (!e->ns || !e->key || !e->val) {
        free(e->ns);
        free(e->key);
        free(e->val);
        s->count--;
        return 0;
    }

    return 1;
}

/* Caller holds mtx. Serialize the store without re-locking (ss_flush runs
 * under the lock; state_store_json would deadlock). */
static char *ss_json_unlocked(state_store *s) {
    cJSON *root = cJSON_CreateObject();
    char *out;

    if (root) {
        for (size_t i = 0; i < s->count; i++) {
            cJSON *nsobj = cJSON_GetObjectItemCaseSensitive(root, s->items[i].ns);
            if (!nsobj || !cJSON_IsObject(nsobj)) {
                nsobj = cJSON_CreateObject();
                cJSON_AddItemToObject(root, s->items[i].ns, nsobj);
            }
            if (nsobj)
                cJSON_AddStringToObject(nsobj, s->items[i].key, s->items[i].val);
        }
    }

    out = root ? cJSON_PrintUnformatted(root) : NULL;
    if (root)
        cJSON_Delete(root);
    return out ? out : xstrdup("{}");
}

static void ss_flush(state_store *s) {
    char *js;

    if (!s->path)
        return;
    js = ss_json_unlocked(s);
    if (!js)
        return;
    if (fs_write_file(s->path, js, strlen(js)) != 0)
        log_warn("state_store: flush to %s failed", s->path);
    free(js);
}

int state_store_set(state_store *s, const char *ns, const char *key, const char *val) {
    int mutated;

    if (!s || !ns || !*ns || !key || !*key)
        return -1;
    mutex_lock(&s->mtx);
    mutated = ss_put(s, ns, key, val);
    if (mutated > 0)
        ss_flush(s);
    mutex_unlock(&s->mtx);
    return 0;
}

const char *state_store_get(state_store *s, const char *ns, const char *key) {
    long i;

    if (!s || !ns || !key)
        return NULL;
    mutex_lock(&s->mtx);
    i = ss_find(s, ns, key);
    const char *v = i >= 0 ? s->items[i].val : NULL;
    mutex_unlock(&s->mtx);
    return v;
}

int state_store_remove(state_store *s, const char *ns, const char *key) {
    int mutated;

    if (!s || !ns || !key)
        return -1;
    mutex_lock(&s->mtx);
    mutated = ss_put(s, ns, key, NULL);
    if (mutated > 0)
        ss_flush(s);
    mutex_unlock(&s->mtx);
    return 0;
}

int state_store_count(state_store *s) {
    int n;

    if (!s)
        return 0;
    mutex_lock(&s->mtx);
    n = (int)s->count;
    mutex_unlock(&s->mtx);
    return n;
}

int state_store_count_ns(state_store *s, const char *ns) {
    int n = 0;

    if (!s || !ns)
        return 0;
    mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->items[i].ns, ns) == 0)
            n++;
    mutex_unlock(&s->mtx);
    return n;
}

int state_store_task_set(state_store *s, long long id, const char *status, const char *input) {
    if (!s || !status)
        return -1;
    char key[32], val[512];
    snprintf(key, sizeof(key), "%lld", id);
    snprintf(val, sizeof(val), "%s|%.400s", status, input ? input : "");
    return state_store_set(s, "task", key, val);
}

int state_store_agent_set(state_store *s, const char *name, const char *role, const char *status) {
    char val[512];

    if (!s || !name || !*name)
        return -1;
    snprintf(val, sizeof(val), "%s|%s", role ? role : "", status ? status : "idle");
    return state_store_set(s, "agent", name, val);
}

char *state_store_json(state_store *s) {
    char *out;

    if (!s)
        return xstrdup("{}");
    mutex_lock(&s->mtx);
    out = ss_json_unlocked(s);
    mutex_unlock(&s->mtx);
    return out;
}

int state_store_load_json(state_store *s, const char *json) {
    cJSON *root;
    int applied = 0;
    cJSON *nsobj;

    if (!s || !json)
        return -1;
    root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        if (root)
            cJSON_Delete(root);
        return -1;
    }

    mutex_lock(&s->mtx);
    cJSON_ArrayForEach(nsobj, root) {
    cJSON *it;

        if (!cJSON_IsObject(nsobj) || !nsobj->string)
            continue;
        cJSON_ArrayForEach(it, nsobj) {
            if (it->string && cJSON_IsString(it)) {
                if (ss_put(s, nsobj->string, it->string, it->valuestring) > 0)
                    applied++;
            }
        }
    }

    mutex_unlock(&s->mtx);
    cJSON_Delete(root);
    return applied;
}

int state_store_save(state_store *s, const char *path) {
    char *js;
    int rc;

    if (!s || !path || !*path)
        return -1;
    js = state_store_json(s); /* takes mtx itself — no outer lock */
    if (!js)
        return -1;
    rc = fs_write_file(path, js, strlen(js));
    free(js);
    if (rc != 0)
        return -1;
    mutex_lock(&s->mtx);
    free(s->path);
    s->path = xstrdup(path);
    mutex_unlock(&s->mtx);
    return 0;
}

int state_store_load(state_store *s, const char *path) {
    char *js;
    int applied;

    if (!s || !path || !*path)
        return -1;
    js = fs_read_file(path);
    if (!js)
        return -1;
    applied = state_store_load_json(s, js);
    free(js);
    if (applied < 0)
        return -1;
    mutex_lock(&s->mtx);
    free(s->path);
    s->path = xstrdup(path);
    mutex_unlock(&s->mtx);
    return 0;
}
