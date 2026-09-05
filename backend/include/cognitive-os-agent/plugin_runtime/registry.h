/* registry.h — plugin metadata registry (the ⭐ Plugin Registry in the
 * architecture). Stores versioned plugin descriptors: capability list,
 * content signature (hash), dependencies, enabled state. Multiple versions of
 * the same plugin coexist; coa_plugin_registry_find returns the latest. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_plugin_registry coa_plugin_registry;

typedef struct coa_plugin_meta {
    char *name;
    char *version;
    char *signature;    /* hex digest of the artifact */
    char *description;
    char **caps;        /* capability tokens */
    size_t n_caps;
    char **deps;        /* dependency plugin names (any version) */
    size_t n_deps;
    int enabled;
    int64_t built_ms;
} coa_plugin_meta;

coa_plugin_registry *coa_plugin_registry_new(void);
void coa_plugin_registry_free(coa_plugin_registry *r);

/* Add a new version. Returns 0 ok, -1 error (invalid/duplicate same-version). */
int coa_plugin_registry_register(coa_plugin_registry *r, const coa_plugin_meta *meta);
int coa_plugin_registry_unregister(coa_plugin_registry *r, const char *name);
/* Enable/disable the LATEST version of a plugin. Returns 0 ok, -1 not found. */
int coa_plugin_registry_set_enabled(coa_plugin_registry *r, const char *name, int enabled);

/* Latest version of a plugin (borrowed). NULL if absent. */
const coa_plugin_meta *coa_plugin_registry_find(coa_plugin_registry *r, const char *name);
int coa_plugin_registry_count(coa_plugin_registry *r);
/* Versioned entry by index (all versions, borrowed). */
const coa_plugin_meta *coa_plugin_registry_get(coa_plugin_registry *r, size_t i);

/* 1 if every dependency (of the latest version) is registered. */
int coa_plugin_registry_deps_met(coa_plugin_registry *r, const char *name);
/* JSON grouped by plugin name with a "versions" array (caller frees). */
char *coa_plugin_registry_json(coa_plugin_registry *r);

/* Persist all registered plugins to <state_root>/plugins.json and reload on
 * startup. Load skips exact (name+version) duplicates. */
int coa_plugin_registry_persist(coa_plugin_registry *r, const char *state_root);
int coa_plugin_registry_load(coa_plugin_registry *r, const char *state_root);

#ifdef __cplusplus
}
#endif
