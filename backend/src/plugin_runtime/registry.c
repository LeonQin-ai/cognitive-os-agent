/* registry.c — versioned plugin metadata registry. */
#include "cognitive-os-agent/plugin_runtime/registry.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct plugin_registry {
    mutex_t mtx;
    plugin_meta *items;
    size_t count, cap;
};

static void meta_free(plugin_meta *m) {
    if (!m)
        return;
    free(m->name);
    free(m->version);
    free(m->signature);
    free(m->description);
    for (size_t i = 0; i < m->n_caps; i++)
        free(m->caps[i]);
    free(m->caps);
    for (size_t i = 0; i < m->n_deps; i++)
        free(m->deps[i]);
    free(m->deps);
    memset(m, 0, sizeof(*m));
}

static plugin_meta meta_copy(const plugin_meta *src) {
    plugin_meta m;
    memset(&m, 0, sizeof(m));
    m.name = xstrdup(src->name ? src->name : "");
    m.version = xstrdup(src->version ? src->version : "0.0.0");
    m.signature = src->signature ? xstrdup(src->signature) : NULL;
    m.description = src->description ? xstrdup(src->description) : NULL;
    m.enabled = src->enabled;
    m.built_ms = src->built_ms;
    m.n_caps = src->n_caps;
    m.caps = (char **)calloc(m.n_caps ? m.n_caps : 1, sizeof(char *));
    for (size_t i = 0; i < m.n_caps; i++)
        m.caps[i] = xstrdup(src->caps[i]);
    m.n_deps = src->n_deps;
    m.deps = (char **)calloc(m.n_deps ? m.n_deps : 1, sizeof(char *));
    for (size_t i = 0; i < m.n_deps; i++)
        m.deps[i] = xstrdup(src->deps[i]);
    return m;
}

plugin_registry *plugin_registry_new(void) {
    plugin_registry *r = (plugin_registry *)calloc(1, sizeof(plugin_registry));
    if (!r)
        return NULL;
    mutex_init(&r->mtx);
    return r;
}

void plugin_registry_free(plugin_registry *r) {
    if (!r)
        return;
    mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++)
        meta_free(&r->items[i]);
    free(r->items);
    mutex_unlock(&r->mtx);
    mutex_destroy(&r->mtx);
    free(r);
}

int plugin_registry_register(plugin_registry *r, const plugin_meta *meta) {
    if (!r || !meta || !meta->name || !meta->version)
        return -1;
    mutex_lock(&r->mtx);
    /* reject exact duplicate (same name+version) */
    for (size_t i = 0; i < r->count; i++)
        if (strcmp(r->items[i].name, meta->name) == 0 && strcmp(r->items[i].version, meta->version) == 0) {
            mutex_unlock(&r->mtx);
            return -1;
        }
    if (r->count == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 8;
        plugin_meta *ni = (plugin_meta *)realloc(r->items, ncap * sizeof(plugin_meta));
        if (!ni) {
            mutex_unlock(&r->mtx);
            return -1;
        }
        r->items = ni;
        r->cap = ncap;
    }
    r->items[r->count++] = meta_copy(meta);
    mutex_unlock(&r->mtx);
    return 0;
}

int plugin_registry_unregister(plugin_registry *r, const char *name) {
    if (!r || !name)
        return -1;
    mutex_lock(&r->mtx);
    int found = 0;
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->items[i].name, name) == 0) {
            meta_free(&r->items[i]);
            if (r->count - i - 1 > 0)
                memmove(&r->items[i], &r->items[i + 1], (r->count - i - 1) * sizeof(plugin_meta));
            r->count--;
            i--;
            found = 1;
        }
    }
    mutex_unlock(&r->mtx);
    return found ? 0 : -1;
}

int plugin_registry_set_enabled(plugin_registry *r, const char *name, int enabled) {
    if (!r || !name)
        return -1;
    mutex_lock(&r->mtx);
    int rc = -1;
    for (size_t i = r->count; i-- > 0;) {
        if (strcmp(r->items[i].name, name) == 0) { /* latest first */
            r->items[i].enabled = enabled ? 1 : 0;
            rc = 0;
            break;
        }
    }
    mutex_unlock(&r->mtx);
    return rc;
}

const plugin_meta *plugin_registry_find(plugin_registry *r, const char *name) {
    if (!r || !name)
        return NULL;
    mutex_lock(&r->mtx);
    const plugin_meta *m = NULL;
    for (size_t i = r->count; i-- > 0;)
        if (strcmp(r->items[i].name, name) == 0) {
            m = &r->items[i];
            break;
        }
    mutex_unlock(&r->mtx);
    return m;
}

int plugin_registry_count(plugin_registry *r) {
    if (!r)
        return 0;
    mutex_lock(&r->mtx);
    int n = (int)r->count;
    mutex_unlock(&r->mtx);
    return n;
}

int plugin_registry_deps_met(plugin_registry *r, const char *name) {
    if (!r || !name)
        return 0;
    mutex_lock(&r->mtx);
    const plugin_meta *m = NULL;
    for (size_t i = r->count; i-- > 0;)
        if (strcmp(r->items[i].name, name) == 0) {
            m = &r->items[i];
            break;
        }
    int ok = 1;
    if (m) {
        for (size_t d = 0; d < m->n_deps; d++) {
            int found = 0;
            for (size_t i = 0; i < r->count; i++)
                if (strcmp(r->items[i].name, m->deps[d]) == 0) {
                    found = 1;
                    break;
                }
            if (!found) {
                ok = 0;
                break;
            }
        }
    } else {
        ok = 0;
    }
    mutex_unlock(&r->mtx);
    return ok;
}

static void add_meta_json(cJSON *arr, const plugin_meta *m) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "version", m->version);
    if (m->signature)
        cJSON_AddStringToObject(o, "signature", m->signature);
    if (m->description)
        cJSON_AddStringToObject(o, "description", m->description);
    cJSON_AddBoolToObject(o, "enabled", m->enabled ? 1 : 0);
    cJSON_AddNumberToObject(o, "built_ms", (double)m->built_ms);
    cJSON *caps = cJSON_CreateArray();
    for (size_t i = 0; i < m->n_caps; i++)
        cJSON_AddItemToArray(caps, cJSON_CreateString(m->caps[i]));
    cJSON_AddItemToObject(o, "capabilities", caps);
    cJSON *deps = cJSON_CreateArray();
    for (size_t i = 0; i < m->n_deps; i++)
        cJSON_AddItemToArray(deps, cJSON_CreateString(m->deps[i]));
    cJSON_AddItemToObject(o, "dependencies", deps);
    cJSON_AddItemToArray(arr, o);
}

char *plugin_registry_json(plugin_registry *r) {
    cJSON *root = cJSON_CreateObject();
    if (!r)
        return cJSON_PrintUnformatted(root);
    mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        plugin_meta *m = &r->items[i];
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
    mutex_unlock(&r->mtx);
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return js;
}

static char *slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static int dump_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return 0;
}

int plugin_registry_persist(plugin_registry *r, const char *state_root) {
    if (!r || !state_root)
        return -1;
    char path[1024];
    path_join(path, sizeof(path), state_root, "plugins.json");
    cJSON *arr = cJSON_CreateArray();
    mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        plugin_meta *m = &r->items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", m->name);
        cJSON_AddStringToObject(o, "version", m->version);
        if (m->signature)
            cJSON_AddStringToObject(o, "signature", m->signature);
        if (m->description)
            cJSON_AddStringToObject(o, "description", m->description);
        cJSON_AddBoolToObject(o, "enabled", m->enabled ? 1 : 0);
        cJSON_AddNumberToObject(o, "built_ms", (double)m->built_ms);
        cJSON *caps = cJSON_CreateArray();
        for (size_t c = 0; c < m->n_caps; c++)
            cJSON_AddItemToArray(caps, cJSON_CreateString(m->caps[c]));
        cJSON_AddItemToObject(o, "capabilities", caps);
        cJSON *deps = cJSON_CreateArray();
        for (size_t d = 0; d < m->n_deps; d++)
            cJSON_AddItemToArray(deps, cJSON_CreateString(m->deps[d]));
        cJSON_AddItemToObject(o, "dependencies", deps);
        cJSON_AddItemToArray(arr, o);
    }
    mutex_unlock(&r->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    int rc = dump_file(path, s ? s : "[]");
    free(s);
    return rc;
}

int plugin_registry_load(plugin_registry *r, const char *state_root) {
    if (!r || !state_root)
        return -1;
    char path[1024];
    path_join(path, sizeof(path), state_root, "plugins.json");
    char *txt = slurp_file(path);
    if (!txt)
        return 0;
    cJSON *arr = cJSON_Parse(txt);
    free(txt);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr)
            cJSON_Delete(arr);
        return 0;
    }
    for (int i = 0; i < cJSON_GetArraySize(arr); i++) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(o))
            continue;
        cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "name");
        cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "version");
        if (!n || !cJSON_IsString(n) || !v || !cJSON_IsString(v))
            continue;
        plugin_meta m;
        memset(&m, 0, sizeof(m));
        m.name = n->valuestring;
        m.version = v->valuestring;
        cJSON *sig = cJSON_GetObjectItemCaseSensitive(o, "signature");
        m.signature = (sig && cJSON_IsString(sig)) ? sig->valuestring : NULL;
        cJSON *desc = cJSON_GetObjectItemCaseSensitive(o, "description");
        m.description = (desc && cJSON_IsString(desc)) ? desc->valuestring : NULL;
        cJSON *en = cJSON_GetObjectItemCaseSensitive(o, "enabled");
        m.enabled = (en && cJSON_IsBool(en)) ? (en->valueint ? 1 : 0) : 1;
        cJSON *caps = cJSON_GetObjectItemCaseSensitive(o, "capabilities");
        if (caps && cJSON_IsArray(caps)) {
            m.n_caps = (size_t)cJSON_GetArraySize(caps);
            m.caps = (char **)calloc(m.n_caps ? m.n_caps : 1, sizeof(char *));
            for (size_t c = 0; c < m.n_caps; c++) {
                cJSON *ci = cJSON_GetArrayItem(caps, c);
                m.caps[c] = (ci && cJSON_IsString(ci)) ? xstrdup(ci->valuestring) : xstrdup("");
            }
        }
        /* dependencies are persisted too: without loading them back,
         * dependency enforcement silently disappears after a restart */
        cJSON *deps = cJSON_GetObjectItemCaseSensitive(o, "dependencies");
        if (deps && cJSON_IsArray(deps)) {
            m.n_deps = (size_t)cJSON_GetArraySize(deps);
            m.deps = (char **)calloc(m.n_deps ? m.n_deps : 1, sizeof(char *));
            for (size_t d = 0; d < m.n_deps; d++) {
                cJSON *di = cJSON_GetArrayItem(deps, d);
                m.deps[d] = (di && cJSON_IsString(di)) ? xstrdup(di->valuestring) : xstrdup("");
            }
        }
        plugin_registry_register(r, &m);
        for (size_t c = 0; c < m.n_caps; c++)
            free(m.caps[c]);
        free(m.caps);
        for (size_t d = 0; d < m.n_deps; d++)
            free(m.deps[d]);
        free(m.deps);
    }
    cJSON_Delete(arr);
    return 0;
}
