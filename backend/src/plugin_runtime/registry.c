/* registry.c — versioned plugin metadata registry. */
#include "cagent/plugin_runtime/registry.h"
#include "cagent/os/os_thread.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct ca_plugin_registry {
    ca_mutex mtx;
    ca_plugin_meta *items;
    size_t count, cap;
};

static void meta_free(ca_plugin_meta *m) {
    if (!m) return;
    free(m->name); free(m->version); free(m->signature); free(m->description);
    for (size_t i = 0; i < m->n_caps; i++) free(m->caps[i]);
    free(m->caps);
    for (size_t i = 0; i < m->n_deps; i++) free(m->deps[i]);
    free(m->deps);
    memset(m, 0, sizeof(*m));
}

static ca_plugin_meta meta_copy(const ca_plugin_meta *src) {
    ca_plugin_meta m;
    memset(&m, 0, sizeof(m));
    m.name = ca_strdup(src->name ? src->name : "");
    m.version = ca_strdup(src->version ? src->version : "0.0.0");
    m.signature = src->signature ? ca_strdup(src->signature) : NULL;
    m.description = src->description ? ca_strdup(src->description) : NULL;
    m.enabled = src->enabled;
    m.built_ms = src->built_ms;
    m.n_caps = src->n_caps;
    m.caps = (char **)calloc(m.n_caps ? m.n_caps : 1, sizeof(char *));
    for (size_t i = 0; i < m.n_caps; i++)
        m.caps[i] = ca_strdup(src->caps[i]);
    m.n_deps = src->n_deps;
    m.deps = (char **)calloc(m.n_deps ? m.n_deps : 1, sizeof(char *));
    for (size_t i = 0; i < m.n_deps; i++)
        m.deps[i] = ca_strdup(src->deps[i]);
    return m;
}

ca_plugin_registry *ca_plugin_registry_new(void) {
    ca_plugin_registry *r = (ca_plugin_registry *)calloc(1, sizeof(ca_plugin_registry));
    if (!r) return NULL;
    ca_mutex_init(&r->mtx);
    return r;
}

void ca_plugin_registry_free(ca_plugin_registry *r) {
    if (!r) return;
    ca_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) meta_free(&r->items[i]);
    free(r->items);
    ca_mutex_unlock(&r->mtx);
    ca_mutex_destroy(&r->mtx);
    free(r);
}

int ca_plugin_registry_register(ca_plugin_registry *r, const ca_plugin_meta *meta) {
    if (!r || !meta || !meta->name || !meta->version) return -1;
    ca_mutex_lock(&r->mtx);
    /* reject exact duplicate (same name+version) */
    for (size_t i = 0; i < r->count; i++)
        if (strcmp(r->items[i].name, meta->name) == 0 &&
            strcmp(r->items[i].version, meta->version) == 0) {
            ca_mutex_unlock(&r->mtx);
            return -1;
        }
    if (r->count == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 8;
        ca_plugin_meta *ni = (ca_plugin_meta *)realloc(r->items, ncap * sizeof(ca_plugin_meta));
        if (!ni) { ca_mutex_unlock(&r->mtx); return -1; }
        r->items = ni;
        r->cap = ncap;
    }
    r->items[r->count++] = meta_copy(meta);
    ca_mutex_unlock(&r->mtx);
    return 0;
}

int ca_plugin_registry_unregister(ca_plugin_registry *r, const char *name) {
    if (!r || !name) return -1;
    ca_mutex_lock(&r->mtx);
    int found = 0;
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->items[i].name, name) == 0) {
            meta_free(&r->items[i]);
            if (r->count - i - 1 > 0)
                memmove(&r->items[i], &r->items[i + 1], (r->count - i - 1) * sizeof(ca_plugin_meta));
            r->count--;
            i--;
            found = 1;
        }
    }
    ca_mutex_unlock(&r->mtx);
    return found ? 0 : -1;
}

int ca_plugin_registry_set_enabled(ca_plugin_registry *r, const char *name, int enabled) {
    if (!r || !name) return -1;
    ca_mutex_lock(&r->mtx);
    int rc = -1;
    for (size_t i = r->count; i-- > 0; ) {
        if (strcmp(r->items[i].name, name) == 0) { /* latest first */
            r->items[i].enabled = enabled ? 1 : 0;
            rc = 0;
            break;
        }
    }
    ca_mutex_unlock(&r->mtx);
    return rc;
}

const ca_plugin_meta *ca_plugin_registry_find(ca_plugin_registry *r, const char *name) {
    if (!r || !name) return NULL;
    ca_mutex_lock(&r->mtx);
    const ca_plugin_meta *m = NULL;
    for (size_t i = r->count; i-- > 0; )
        if (strcmp(r->items[i].name, name) == 0) { m = &r->items[i]; break; }
    ca_mutex_unlock(&r->mtx);
    return m;
}

int ca_plugin_registry_count(ca_plugin_registry *r) {
    if (!r) return 0;
    ca_mutex_lock(&r->mtx);
    int n = (int)r->count;
    ca_mutex_unlock(&r->mtx);
    return n;
}

const ca_plugin_meta *ca_plugin_registry_get(ca_plugin_registry *r, size_t i) {
    if (!r) return NULL;
    ca_mutex_lock(&r->mtx);
    const ca_plugin_meta *m = (i < r->count) ? &r->items[i] : NULL;
    ca_mutex_unlock(&r->mtx);
    return m;
}

int ca_plugin_registry_deps_met(ca_plugin_registry *r, const char *name) {
    if (!r || !name) return 0;
    ca_mutex_lock(&r->mtx);
    const ca_plugin_meta *m = NULL;
    for (size_t i = r->count; i-- > 0; )
        if (strcmp(r->items[i].name, name) == 0) { m = &r->items[i]; break; }
    int ok = 1;
    if (m) {
        for (size_t d = 0; d < m->n_deps; d++) {
            int found = 0;
            for (size_t i = 0; i < r->count; i++)
                if (strcmp(r->items[i].name, m->deps[d]) == 0) { found = 1; break; }
            if (!found) { ok = 0; break; }
        }
    } else {
        ok = 0;
    }
    ca_mutex_unlock(&r->mtx);
    return ok;
}

static void add_meta_json(cJSON *arr, const ca_plugin_meta *m) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "version", m->version);
    if (m->signature) cJSON_AddStringToObject(o, "signature", m->signature);
    if (m->description) cJSON_AddStringToObject(o, "description", m->description);
    cJSON_AddBoolToObject(o, "enabled", m->enabled ? 1 : 0);
    cJSON_AddNumberToObject(o, "built_ms", (double)m->built_ms);
    cJSON *caps = cJSON_CreateArray();
    for (size_t i = 0; i < m->n_caps; i++) cJSON_AddItemToArray(caps, cJSON_CreateString(m->caps[i]));
    cJSON_AddItemToObject(o, "capabilities", caps);
    cJSON *deps = cJSON_CreateArray();
    for (size_t i = 0; i < m->n_deps; i++) cJSON_AddItemToArray(deps, cJSON_CreateString(m->deps[i]));
    cJSON_AddItemToObject(o, "dependencies", deps);
    cJSON_AddItemToArray(arr, o);
}

char *ca_plugin_registry_json(ca_plugin_registry *r) {
    cJSON *root = cJSON_CreateObject();
    if (!r) return cJSON_PrintUnformatted(root);
    ca_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        ca_plugin_meta *m = &r->items[i];
        cJSON *node = cJSON_GetObjectItemCaseSensitive(root, m->name);
        if (!node) {
            node = cJSON_CreateObject();
            cJSON_AddItemToObject(root, m->name, node);
        }
        cJSON *vers = cJSON_GetObjectItemCaseSensitive(node, "versions");
        if (!vers) {
            vers = cJSON_CreateArray();
            cJSON_AddItemToObject(node, "versions", vers);
        }
        add_meta_json(vers, m);
    }
    ca_mutex_unlock(&r->mtx);
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return js;
}
