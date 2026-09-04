/* service.c — Memory Service: type<->backend decoupling layer.
 * Generic vtable plumbing plus the default backend that maps the four memory
 * types onto the existing ca_memory facade. */
#include "cagent/memory/service.h"
#include "cagent/memory/memory.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

static const char *type_names[CA_MEM_TYPE_COUNT] = {
    "working", "episodic", "semantic", "procedural",
};

const char *ca_mem_type_name(ca_mem_type t) {
    if ((int)t < 0 || (int)t >= CA_MEM_TYPE_COUNT) return "?";
    return type_names[t];
}

int ca_mem_type_parse(const char *s, ca_mem_type *out) {
    if (!s || !out) return -1;
    for (int i = 0; i < CA_MEM_TYPE_COUNT; i++)
        if (strcmp(type_names[i], s) == 0) { *out = (ca_mem_type)i; return 0; }
    return -1;
}

/* ---------- generic plumbing ---------- */

struct ca_memory_service {
    const ca_memory_service_ops *ops;
    void *impl;
};

ca_memory_service *ca_memory_service_new(const ca_memory_service_ops *ops, void *impl) {
    if (!ops || !ops->name || !impl) return NULL;
    ca_memory_service *ms = (ca_memory_service *)calloc(1, sizeof(*ms));
    if (!ms) return NULL;
    ms->ops = ops;
    ms->impl = impl;
    return ms;
}

void ca_memory_service_free(ca_memory_service *ms) {
    if (!ms) return;
    if (ms->ops->destroy) ms->ops->destroy(ms->impl);
    free(ms);
}

const char *ca_memory_service_backend(const ca_memory_service *ms) {
    return (ms && ms->ops->name) ? ms->ops->name : "?";
}

int ca_memory_service_remember(ca_memory_service *ms, ca_mem_type t,
                               const char *key, const char *text) {
    if (!ms || (int)t < 0 || (int)t >= CA_MEM_TYPE_COUNT || !text) return -1;
    if (!ms->ops->remember) return -1;
    return ms->ops->remember(ms->impl, t, key, text);
}

int ca_memory_service_forget(ca_memory_service *ms, ca_mem_type t, const char *key) {
    if (!ms || (int)t < 0 || (int)t >= CA_MEM_TYPE_COUNT) return -1;
    if (!ms->ops->forget) return -1;
    return ms->ops->forget(ms->impl, t, key);
}

int ca_memory_service_recall_key(ca_memory_service *ms, ca_mem_type t,
                                 const char *key, char **text) {
    if (!ms || (int)t < 0 || (int)t >= CA_MEM_TYPE_COUNT || !text) return -1;
    *text = NULL;
    if (!ms->ops->recall_key) return -1;
    return ms->ops->recall_key(ms->impl, t, key, text);
}

int ca_memory_service_recall_query(ca_memory_service *ms, ca_mem_type t,
                                   const char *query, int k, char **json) {
    if (!ms || (int)t < 0 || (int)t >= CA_MEM_TYPE_COUNT || !json) return -1;
    *json = NULL;
    if (!ms->ops->recall_query) return -1;
    return ms->ops->recall_query(ms->impl, t, query, k, json);
}

int ca_memory_service_stats(ca_memory_service *ms, char **json) {
    if (!ms || !json) return -1;
    *json = NULL;
    if (!ms->ops->stats) return -1;
    return ms->ops->stats(ms->impl, json);
}

/* ---------- default backend over the ca_memory facade ---------- */

typedef struct { ca_memory *m; } def_impl; /* borrowed */

static int def_remember(void *impl, ca_mem_type t, const char *key, const char *text) {
    def_impl *d = impl;
    switch (t) {
    case CA_MEM_WORKING:
        ca_memory_working_push(d->m, text);
        return 0;
    case CA_MEM_EPISODIC:
        ca_memory_record_experience(d->m, key ? key : text, text);
        return 0;
    case CA_MEM_SEMANTIC:
    case CA_MEM_PROCEDURAL:
        if (!key || !*key) return -1;
        char kbuf[256];
        if (t == CA_MEM_PROCEDURAL && strncmp(key, "procedure.", 10) != 0)
            snprintf(kbuf, sizeof(kbuf), "procedure.%s", key);
        else
            snprintf(kbuf, sizeof(kbuf), "%s", key);
        ca_memory_remember(d->m, kbuf, text);
        return 0;
    default:
        return -1;
    }
}

static int def_forget(void *impl, ca_mem_type t, const char *key) {
    def_impl *d = impl;
    if (t == CA_MEM_SEMANTIC || t == CA_MEM_PROCEDURAL) {
        if (!key || !*key) return -1;
        char kbuf[256];
        if (t == CA_MEM_PROCEDURAL && strncmp(key, "procedure.", 10) != 0)
            snprintf(kbuf, sizeof(kbuf), "procedure.%s", key);
        else
            snprintf(kbuf, sizeof(kbuf), "%s", key);
        ca_memory_remember(d->m, kbuf, NULL);
        return 0;
    }
    (void)d;
    return -1; /* working ring / episodes are lifecycle-managed, not key-forgotten */
}

static int def_recall_key(void *impl, ca_mem_type t, const char *key, char **text) {
    def_impl *d = impl;
    if (t == CA_MEM_SEMANTIC || t == CA_MEM_PROCEDURAL) {
        if (!key || !*key) return -1;
        char kbuf[256];
        if (t == CA_MEM_PROCEDURAL && strncmp(key, "procedure.", 10) != 0)
            snprintf(kbuf, sizeof(kbuf), "procedure.%s", key);
        else
            snprintf(kbuf, sizeof(kbuf), "%s", key);
        const char *v = ca_memory_recall(d->m, kbuf);
        if (!v) return -1;
        *text = ca_strdup(v);
        return *text ? 0 : -1;
    }
    if (t == CA_MEM_WORKING) {
        int n = ca_memory_working_count(d->m);
        for (int i = 0; i < n; i++) {
            const char *w = ca_memory_working_at(d->m, i);
            if (w && strcmp(w, key) == 0) {
                *text = ca_strdup(w);
                return *text ? 0 : -1;
            }
        }
        return -1;
    }
    return -1;
}

static int def_recall_query(void *impl, ca_mem_type t, const char *query, int k,
                            char **json) {
    def_impl *d = impl;
    if (!query) return -1;
    if (k <= 0) k = 5;
    /* the facade's keyword search covers working + episodes; semantic/procedural
     * facts come back through the same {kind,text,score} shape */
    char *arr = ca_memory_search(d->m, query, k);
    if (!arr) return -1;
    if (t == CA_MEM_WORKING || t == CA_MEM_EPISODIC) {
        /* filter to the matching kinds: search entries carry kind
         * "working" / "experience" (see ca_memory_search) */
        cJSON *root = cJSON_Parse(arr);
        if (!root) { free(arr); return -1; }
        cJSON *out = cJSON_CreateArray();
        const char *want = (t == CA_MEM_WORKING) ? "working" : "experience";
        cJSON *it;
        cJSON_ArrayForEach(it, root) {
            cJSON *kd = cJSON_GetObjectItemCaseSensitive(it, "kind");
            if (kd && cJSON_IsString(kd) && strcmp(kd->valuestring, want) == 0) {
                cJSON *dup = cJSON_Duplicate(it, 1);
                if (dup) cJSON_AddItemToArray(out, dup);
            }
        }
        cJSON_Delete(root);
        free(arr);
        *json = out ? cJSON_PrintUnformatted(out) : ca_strdup("[]");
        if (out) cJSON_Delete(out);
        return *json ? 0 : -1;
    }
    /* semantic / procedural: keyword search has no fact kind — fall back to
     * the whole long-term store (small, bounded) */
    free(arr);
    *json = ca_memory_longterm_json(d->m);
    return *json ? 0 : -1;
}

static int def_stats(void *impl, char **json) {
    def_impl *d = impl;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return -1;
    for (int ty = 0; ty < CA_MEM_TYPE_COUNT; ty++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", ca_mem_type_name((ca_mem_type)ty));
        int n = 0;
        if (ty == (int)CA_MEM_WORKING) {
            n = ca_memory_working_count(d->m);
        } else if (ty == (int)CA_MEM_EPISODIC) {
            n = ca_memory_episode_count(d->m);
        } else { /* semantic / procedural: count long-term facts by prefix */
            char *lt = ca_memory_longterm_json(d->m);
            cJSON *root = lt ? cJSON_Parse(lt) : NULL;
            if (root && cJSON_IsObject(root)) {
                cJSON *it;
                cJSON_ArrayForEach(it, root) {
                    int proc = it->string && strncmp(it->string, "procedure.", 10) == 0;
                    if (ty == (int)CA_MEM_PROCEDURAL ? proc : !proc) n++;
                }
            }
            if (root) cJSON_Delete(root);
            free(lt);
        }
        cJSON_AddNumberToObject(o, "count", n);
        cJSON_AddItemToArray(arr, o);
    }
    *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return *json ? 0 : -1;
}

static void def_destroy(void *impl) { free(impl); }

static const ca_memory_service_ops def_ops = {
    "default", def_remember, def_forget, def_recall_key,
    def_recall_query, def_stats, def_destroy,
};

ca_memory_service *ca_memory_service_new_default(ca_memory *m) {
    if (!m) return NULL;
    def_impl *d = (def_impl *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->m = m;
    ca_memory_service *ms = ca_memory_service_new(&def_ops, d);
    if (!ms) free(d);
    return ms;
}
