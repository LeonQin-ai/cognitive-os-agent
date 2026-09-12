/* service.c — Memory Service: type<->backend decoupling layer.
 * Generic vtable plumbing plus the default backend that maps the four memory
 * types onto the existing memory facade. */
#include "cognitive-os-agent/memory/service.h"
#include "cognitive-os-agent/memory/memory.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

static const char *type_names[MEM_TYPE_COUNT] = {
    "working",
    "episodic",
    "semantic",
    "procedural",
};

const char *mem_type_name(mem_type t) {
    if ((int)t < 0 || (int)t >= MEM_TYPE_COUNT)
        return "?";
    return type_names[t];
}

int mem_type_parse(const char *s, mem_type *out) {
    if (!s || !out)
        return -1;
    for (int i = 0; i < MEM_TYPE_COUNT; i++)
        if (strcmp(type_names[i], s) == 0) {
            *out = (mem_type)i;
            return 0;
        }

    return -1;
}

/* ---------- generic plumbing ---------- */

struct memory_service {
    const memory_service_ops *ops;
    void *impl;
};

memory_service *memory_service_new(const memory_service_ops *ops, void *impl) {
    memory_service *ms;

    if (!ops || !ops->name || !impl)
        return NULL;
    ms = (memory_service *)calloc(1, sizeof(*ms));
    if (!ms)
        return NULL;
    ms->ops = ops;
    ms->impl = impl;
    return ms;
}

void memory_service_free(memory_service *ms) {
    if (!ms)
        return;
    if (ms->ops->destroy)
        ms->ops->destroy(ms->impl);
    free(ms);
}

const char *memory_service_backend(const memory_service *ms) {
    return (ms && ms->ops->name) ? ms->ops->name : "?";
}

int memory_service_remember(memory_service *ms, mem_type t, const char *key, const char *text) {
    if (!ms || (int)t < 0 || (int)t >= MEM_TYPE_COUNT || !text)
        return -1;
    if (!ms->ops->remember)
        return -1;
    return ms->ops->remember(ms->impl, t, key, text);
}

int memory_service_forget(memory_service *ms, mem_type t, const char *key) {
    if (!ms || (int)t < 0 || (int)t >= MEM_TYPE_COUNT)
        return -1;
    if (!ms->ops->forget)
        return -1;
    return ms->ops->forget(ms->impl, t, key);
}

int memory_service_recall_key(memory_service *ms, mem_type t, const char *key, char **text) {
    if (!ms || (int)t < 0 || (int)t >= MEM_TYPE_COUNT || !text)
        return -1;
    *text = NULL;
    if (!ms->ops->recall_key)
        return -1;
    return ms->ops->recall_key(ms->impl, t, key, text);
}

int memory_service_recall_query(memory_service *ms, mem_type t, const char *query, int k, char **json) {
    if (!ms || (int)t < 0 || (int)t >= MEM_TYPE_COUNT || !json)
        return -1;
    *json = NULL;
    if (!ms->ops->recall_query)
        return -1;
    return ms->ops->recall_query(ms->impl, t, query, k, json);
}

int memory_service_stats(memory_service *ms, char **json) {
    if (!ms || !json)
        return -1;
    *json = NULL;
    if (!ms->ops->stats)
        return -1;
    return ms->ops->stats(ms->impl, json);
}

/* ---------- default backend over the memory facade ---------- */

typedef struct {
    memory *m;
} def_impl; /* borrowed */

static int def_remember(void *impl, mem_type t, const char *key, const char *text) {
    def_impl *d = impl;
    switch (t) {
    case MEM_WORKING:
        memory_working_push(d->m, text);
        return 0;
    case MEM_EPISODIC:
        memory_record_experience(d->m, key ? key : text, text);
        return 0;
    case MEM_SEMANTIC:
    case MEM_PROCEDURAL:
        if (!key || !*key)
            return -1;
        char kbuf[256];
        if (t == MEM_PROCEDURAL && strncmp(key, "procedure.", 10) != 0)
            snprintf(kbuf, sizeof(kbuf), "procedure.%s", key);
        else
            snprintf(kbuf, sizeof(kbuf), "%s", key);
        memory_remember(d->m, kbuf, text);
        return 0;
    default:
        return -1;
    }
}

static int def_forget(void *impl, mem_type t, const char *key) {
    def_impl *d = impl;
    if (t == MEM_SEMANTIC || t == MEM_PROCEDURAL) {
        if (!key || !*key)
            return -1;
        char kbuf[256];
        if (t == MEM_PROCEDURAL && strncmp(key, "procedure.", 10) != 0)
            snprintf(kbuf, sizeof(kbuf), "procedure.%s", key);
        else
            snprintf(kbuf, sizeof(kbuf), "%s", key);
        memory_remember(d->m, kbuf, NULL);
        return 0;
    }

    (void)d;
    return -1; /* working ring / episodes are lifecycle-managed, not key-forgotten */
}

static int def_recall_key(void *impl, mem_type t, const char *key, char **text) {
    def_impl *d = impl;
    if (t == MEM_SEMANTIC || t == MEM_PROCEDURAL) {
        if (!key || !*key)
            return -1;
        char kbuf[256];
        if (t == MEM_PROCEDURAL && strncmp(key, "procedure.", 10) != 0)
            snprintf(kbuf, sizeof(kbuf), "procedure.%s", key);
        else
            snprintf(kbuf, sizeof(kbuf), "%s", key);
        const char *v = memory_recall(d->m, kbuf);
        if (!v)
            return -1;
        *text = xstrdup(v);
        return *text ? 0 : -1;
    }

    if (t == MEM_WORKING) {
        int n = memory_working_count(d->m);
        for (int i = 0; i < n; i++) {
            const char *w = memory_working_at(d->m, i);
            if (w && strcmp(w, key) == 0) {
                *text = xstrdup(w);
                return *text ? 0 : -1;
            }
        }
        return -1;
    }

    return -1;
}

static int def_recall_query(void *impl, mem_type t, const char *query, int k, char **json) {
    char *arr;

    def_impl *d = impl;
    if (!query)
        return -1;
    if (k <= 0)
        k = 5;
    /* the facade's keyword search covers working + episodes; semantic/procedural
     * facts come back through the same {kind,text,score} shape */
    arr = memory_search(d->m, query, k);
    if (!arr)
        return -1;
    if (t == MEM_WORKING || t == MEM_EPISODIC) {
        /* filter to the matching kinds: search entries carry kind
         * "working" / "experience" (see memory_search) */
        cJSON *root = cJSON_Parse(arr);
        if (!root) {
            free(arr);
            return -1;
        }
        cJSON *out = cJSON_CreateArray();
        const char *want = (t == MEM_WORKING) ? "working" : "experience";
        cJSON *it;
        cJSON_ArrayForEach(it, root) {
            cJSON *kd = cJSON_GetObjectItemCaseSensitive(it, "kind");
            if (kd && cJSON_IsString(kd) && strcmp(kd->valuestring, want) == 0) {
                cJSON *dup = cJSON_Duplicate(it, 1);
                if (dup)
                    cJSON_AddItemToArray(out, dup);
            }
        }
        cJSON_Delete(root);
        free(arr);
        *json = out ? cJSON_PrintUnformatted(out) : xstrdup("[]");
        if (out)
            cJSON_Delete(out);
        return *json ? 0 : -1;
    }

    /* semantic / procedural: keyword search has no fact kind — fall back to
     * the whole long-term store (small, bounded) */
    free(arr);
    *json = memory_longterm_json(d->m);
    return *json ? 0 : -1;
}

static int def_stats(void *impl, char **json) {
    cJSON *arr;

    def_impl *d = impl;
    arr = cJSON_CreateArray();
    if (!arr)
        return -1;
    for (int ty = 0; ty < MEM_TYPE_COUNT; ty++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", mem_type_name((mem_type)ty));
        int n = 0;
        if (ty == (int)MEM_WORKING) {
            n = memory_working_count(d->m);
        } else if (ty == (int)MEM_EPISODIC) {
            n = memory_episode_count(d->m);
        } else { /* semantic / procedural: count long-term facts by prefix */
            char *lt = memory_longterm_json(d->m);
            cJSON *root = lt ? cJSON_Parse(lt) : NULL;
            if (root && cJSON_IsObject(root)) {
                cJSON *it;
                cJSON_ArrayForEach(it, root) {
                    int proc = it->string && strncmp(it->string, "procedure.", 10) == 0;
                    if (ty == (int)MEM_PROCEDURAL ? proc : !proc)
                        n++;
                }
            }
            if (root)
                cJSON_Delete(root);
            free(lt);
        }
        cJSON_AddNumberToObject(o, "count", n);
        cJSON_AddItemToArray(arr, o);
    }

    *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return *json ? 0 : -1;
}

static void def_destroy(void *impl) {
    free(impl);
}

static const memory_service_ops def_ops = {
    "default", def_remember, def_forget, def_recall_key, def_recall_query, def_stats, def_destroy,
};

memory_service *memory_service_new_default(memory *m) {
    memory_service *ms;

    if (!m)
        return NULL;
    def_impl *d = (def_impl *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->m = m;
    ms = memory_service_new(&def_ops, d);
    if (!ms)
        free(d);
    return ms;
}
