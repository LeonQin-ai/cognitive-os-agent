#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cagent/infra/config.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "cagent/infra/util.h"
#include "cagent/os/os_fs.h"
#include "cJSON.h"

#if defined(_WIN32)
/* stdlib.h provides _environ (a macro over __p__environ on mingw, a plain
 * variable on MSVC); do not redeclare it here to avoid dllimport warnings. */
#define CA_ENVIRON _environ
#else
extern char **environ;
#define CA_ENVIRON environ
#endif

struct ca_config {
    cJSON *root;
};

ca_config *ca_config_new(void) {
    ca_config *c = calloc(1, sizeof(ca_config));
    if (!c) return NULL;
    c->root = cJSON_CreateObject();
    return c;
}

int ca_config_apply_json(ca_config *c, const char *json_text) {
    if (!c) return -1;
    cJSON *parsed = cJSON_Parse(json_text);
    if (!parsed) return -1;
    cJSON *target = parsed;
    if (cJSON_IsObject(parsed)) {
        /* merge into existing root */
        cJSON *item = parsed->child;
        while (item) {
            cJSON *next = item->next;
            cJSON_DetachItemViaPointer(parsed, item);
            cJSON_DeleteItemFromObject(c->root, item->string);
            cJSON_AddItemToObject(c->root, item->string, item);
            item = next;
        }
        cJSON_Delete(parsed);
    } else if (cJSON_IsArray(parsed)) {
        /* allow array configs? treat as replace of root */
        cJSON_Delete(c->root);
        c->root = parsed;
        (void)target;
    } else {
        cJSON_Delete(parsed);
        return -1;
    }
    return 0;
}

int ca_config_load_file(ca_config *c, const char *path) {
    char *text = ca_fs_read_file(path);
    if (!text) return -1;
    int r = ca_config_apply_json(c, text);
    free(text);
    return r;
}

void ca_config_apply_env(ca_config *c, const char *prefix) {
    if (!c || !prefix || !*prefix) return;
    size_t plen = strlen(prefix);
    for (char **e = CA_ENVIRON; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        size_t klen = (size_t)(eq - *e);
        if (klen < plen || strncmp(*e, prefix, plen) != 0) continue;
        /* build dotted key: "CA_LLM_PROVIDER" -> "llm.provider" */
        char key[512];
        size_t n = 0;
        for (size_t i = plen; i < klen && n < sizeof(key) - 1; i++) {
            char ch = (*e)[i];
            if (ch == '_') ch = '.';
            else ch = (char)tolower((unsigned char)ch);
            key[n++] = ch;
        }
        key[n] = '\0';
        cJSON_DeleteItemFromObject(c->root, key);
        cJSON_AddStringToObject(c->root, key, eq + 1);
    }
}

void ca_config_free(ca_config *c) {
    if (!c) return;
    cJSON_Delete(c->root);
    free(c);
}

/* Navigate a dotted path into the JSON tree. The built-in defaults store flat
 * dotted keys (e.g. "llm.base_url"), while env vars (ca_config_apply_env) write
 * underscore-flattened keys (e.g. "llm.base.url"). Since env vars must override
 * defaults, try the flattened form FIRST, then the exact key, then nested dot
 * navigation. Without this order an empty-string default like "llm.base_url":""
 * would shadow a CA_LLM_BASE_URL env value. */
static cJSON *config_get_path(const ca_config *c, const char *key) {
    if (!c || !c->root || !cJSON_IsObject(c->root)) return NULL;
    if (!key || !*key) return NULL;
    char flat[512];
    size_t j = 0;
    for (size_t i = 0; key[i] && j + 1 < sizeof(flat); i++)
        flat[j++] = (key[i] == '_') ? '.' : key[i];
    flat[j] = '\0';
    cJSON *n = cJSON_GetObjectItemCaseSensitive(c->root, flat);
    if (n) return n;
    n = cJSON_GetObjectItemCaseSensitive(c->root, key);
    if (n) return n;
    cJSON *node = c->root;
    char path[512];
    snprintf(path, sizeof(path), "%s", key);
    /* manual dot-splitting (strtok_r is not portable to MSVC) */
    char *seg = path;
    for (;;) {
        char *dot = strchr(seg, '.');
        if (dot) *dot = '\0';
        node = cJSON_GetObjectItemCaseSensitive(node, seg);
        if (!dot) break;
        if (!node) break;
        seg = dot + 1;
    }
    return node;
}

const char *ca_config_get_str(const ca_config *c, const char *key, const char *def) {
    cJSON *n = config_get_path(c, key);
    if (n && cJSON_IsString(n)) return n->valuestring;
    return def;
}

int64_t ca_config_get_int(const ca_config *c, const char *key, int64_t def) {
    cJSON *n = config_get_path(c, key);
    if (n && cJSON_IsNumber(n)) return (int64_t)n->valuedouble;
    return def;
}

double ca_config_get_dbl(const ca_config *c, const char *key, double def) {
    cJSON *n = config_get_path(c, key);
    if (n && cJSON_IsNumber(n)) return n->valuedouble;
    return def;
}

int ca_config_get_bool(const ca_config *c, const char *key, int def) {
    cJSON *n = config_get_path(c, key);
    if (n && cJSON_IsBool(n)) return cJSON_IsTrue(n);
    return def;
}

int ca_config_has(const ca_config *c, const char *key) {
    return config_get_path(c, key) != NULL;
}

char *ca_config_to_json(const ca_config *c) {
    if (!c || !c->root) return ca_strdup("{}");
    char *s = cJSON_PrintUnformatted(c->root);
    return s; /* cJSON returns malloc'd string */
}

void ca_config_set_str(ca_config *c, const char *key, const char *value) {
    if (!c || !c->root || !key || !*key) return;
    if (value) {
        cJSON_DeleteItemFromObject(c->root, key);
        cJSON_AddStringToObject(c->root, key, value);
    } else {
        cJSON_DeleteItemFromObject(c->root, key);
    }
}

int ca_config_save_file(ca_config *c, const char *path) {
    if (!c || !path) return -1;
    char *js = ca_config_to_json(c);
    if (!js) return -1;
    int rc = ca_fs_write_file(path, js, strlen(js));
    free(js);
    return rc;
}
