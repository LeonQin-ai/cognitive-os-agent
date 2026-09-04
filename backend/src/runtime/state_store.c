/* state_store.c — Context layer: unified KV / Task / Agent state store. */
#include "cagent/runtime/state_store.h"
#include "cagent/os/os_thread.h"
#include "cagent/infra/util.h"
#include "cagent/infra/logging.h"
#include "cagent/os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

typedef struct ss_entry {
    char *ns;
    char *key;
    char *val;
} ss_entry;

struct ca_state_store {
    ca_mutex mtx;
    ss_entry *items;
    size_t count, cap;
    char *path; /* set by save/load; enables auto-flush */
};

ca_state_store *ca_state_store_new(void) {
    ca_state_store *s = (ca_state_store *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    ca_mutex_init(&s->mtx);
    return s;
}

void ca_state_store_free(ca_state_store *s) {
    if (!s) return;
    ca_mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++) {
        free(s->items[i].ns);
        free(s->items[i].key);
        free(s->items[i].val);
    }
    free(s->items);
    free(s->path);
    ca_mutex_unlock(&s->mtx);
    ca_mutex_destroy(&s->mtx);
    free(s);
}

static long ss_find(ca_state_store *s, const char *ns, const char *key) {
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->items[i].ns, ns) == 0 && strcmp(s->items[i].key, key) == 0)
            return (long)i;
    return -1;
}

/* Caller holds mtx. 1 = mutated (needs flush). */
static int ss_put(ca_state_store *s, const char *ns, const char *key,
                  const char *val) {
    long i = ss_find(s, ns, key);
    if (i >= 0) {
        if (!val) { /* remove */
            free(s->items[i].ns); free(s->items[i].key); free(s->items[i].val);
            memmove(&s->items[i], &s->items[i + 1],
                    (s->count - (size_t)i - 1) * sizeof(ss_entry));
            s->count--;
            return 1;
        }
        if (strcmp(s->items[i].val, val) == 0) return 0;
        char *nv = ca_strdup(val);
        if (!nv) return 0;
        free(s->items[i].val);
        s->items[i].val = nv;
        return 1;
    }
    if (!val) return 0;
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        ss_entry *ni = (ss_entry *)realloc(s->items, cap * sizeof(ss_entry));
        if (!ni) return 0;
        s->items = ni;
        s->cap = cap;
    }
    ss_entry *e = &s->items[s->count++];
    memset(e, 0, sizeof(*e));
    e->ns = ca_strdup(ns);
    e->key = ca_strdup(key);
    e->val = ca_strdup(val);
    if (!e->ns || !e->key || !e->val) {
        free(e->ns); free(e->key); free(e->val);
        s->count--;
        return 0;
    }
    return 1;
}

/* Caller holds mtx. Serialize the store without re-locking (ss_flush runs
 * under the lock; ca_state_store_json would deadlock). */
static char *ss_json_unlocked(ca_state_store *s) {
    cJSON *root = cJSON_CreateObject();
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
    char *out = root ? cJSON_PrintUnformatted(root) : NULL;
    if (root) cJSON_Delete(root);
    return out ? out : ca_strdup("{}");
}

static void ss_flush(ca_state_store *s) {
    if (!s->path) return;
    char *js = ss_json_unlocked(s);
    if (!js) return;
    if (ca_fs_write_file(s->path, js, strlen(js)) != 0)
        ca_log_warn("state_store: flush to %s failed", s->path);
    free(js);
}

int ca_state_store_set(ca_state_store *s, const char *ns, const char *key,
                       const char *val) {
    if (!s || !ns || !*ns || !key || !*key) return -1;
    ca_mutex_lock(&s->mtx);
    int mutated = ss_put(s, ns, key, val);
    if (mutated > 0) ss_flush(s);
    ca_mutex_unlock(&s->mtx);
    return 0;
}

const char *ca_state_store_get(ca_state_store *s, const char *ns, const char *key) {
    if (!s || !ns || !key) return NULL;
    ca_mutex_lock(&s->mtx);
    long i = ss_find(s, ns, key);
    const char *v = i >= 0 ? s->items[i].val : NULL;
    ca_mutex_unlock(&s->mtx);
    return v;
}

int ca_state_store_remove(ca_state_store *s, const char *ns, const char *key) {
    if (!s || !ns || !key) return -1;
    ca_mutex_lock(&s->mtx);
    int mutated = ss_put(s, ns, key, NULL);
    if (mutated > 0) ss_flush(s);
    ca_mutex_unlock(&s->mtx);
    return 0;
}

int ca_state_store_count(ca_state_store *s) {
    if (!s) return 0;
    ca_mutex_lock(&s->mtx);
    int n = (int)s->count;
    ca_mutex_unlock(&s->mtx);
    return n;
}

int ca_state_store_count_ns(ca_state_store *s, const char *ns) {
    if (!s || !ns) return 0;
    ca_mutex_lock(&s->mtx);
    int n = 0;
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->items[i].ns, ns) == 0) n++;
    ca_mutex_unlock(&s->mtx);
    return n;
}

int ca_state_store_task_set(ca_state_store *s, long long id, const char *status,
                            const char *input) {
    if (!s || !status) return -1;
    char key[32], val[512];
    snprintf(key, sizeof(key), "%lld", id);
    snprintf(val, sizeof(val), "%s|%.400s", status, input ? input : "");
    return ca_state_store_set(s, "task", key, val);
}

int ca_state_store_agent_set(ca_state_store *s, const char *name,
                             const char *role, const char *status) {
    if (!s || !name || !*name) return -1;
    char val[512];
    snprintf(val, sizeof(val), "%s|%s", role ? role : "", status ? status : "idle");
    return ca_state_store_set(s, "agent", name, val);
}

char *ca_state_store_json(ca_state_store *s) {
    if (!s) return ca_strdup("{}");
    ca_mutex_lock(&s->mtx);
    char *out = ss_json_unlocked(s);
    ca_mutex_unlock(&s->mtx);
    return out;
}

int ca_state_store_load_json(ca_state_store *s, const char *json) {
    if (!s || !json) return -1;
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return -1;
    }
    int applied = 0;
    ca_mutex_lock(&s->mtx);
    cJSON *nsobj;
    cJSON_ArrayForEach(nsobj, root) {
        if (!cJSON_IsObject(nsobj) || !nsobj->string) continue;
        cJSON *it;
        cJSON_ArrayForEach(it, nsobj) {
            if (it->string && cJSON_IsString(it)) {
                if (ss_put(s, nsobj->string, it->string, it->valuestring) > 0)
                    applied++;
            }
        }
    }
    ca_mutex_unlock(&s->mtx);
    cJSON_Delete(root);
    return applied;
}

int ca_state_store_save(ca_state_store *s, const char *path) {
    if (!s || !path || !*path) return -1;
    char *js = ca_state_store_json(s); /* takes mtx itself — no outer lock */
    if (!js) return -1;
    int rc = ca_fs_write_file(path, js, strlen(js));
    free(js);
    if (rc != 0) return -1;
    ca_mutex_lock(&s->mtx);
    free(s->path);
    s->path = ca_strdup(path);
    ca_mutex_unlock(&s->mtx);
    return 0;
}

int ca_state_store_load(ca_state_store *s, const char *path) {
    if (!s || !path || !*path) return -1;
    char *js = ca_fs_read_file(path);
    if (!js) return -1;
    int applied = ca_state_store_load_json(s, js);
    free(js);
    if (applied < 0) return -1;
    ca_mutex_lock(&s->mtx);
    free(s->path);
    s->path = ca_strdup(path);
    ca_mutex_unlock(&s->mtx);
    return 0;
}
