/* registry.h — plugin metadata registry (the ⭐ Plugin Registry in the
 * architecture). Stores versioned plugin descriptors: capability list,
 * content signature (hash), dependencies, enabled state. Multiple versions of
 * the same plugin coexist; plugin_registry_find returns the latest. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct plugin_registry plugin_registry;

typedef struct plugin_meta {
    char *name;
    char *version;
    char *signature; /* hex digest of the artifact */
    char *description;
    char **caps; /* capability tokens */
    size_t n_caps;
    char **deps; /* dependency plugin names (any version) */
    size_t n_deps;
    int enabled;
    int64_t built_ms;
} plugin_meta;

plugin_registry *plugin_registry_new(void);
void plugin_registry_free(plugin_registry *r);

/* Add a new version. Returns 0 ok, -1 error (invalid/duplicate same-version). */
int plugin_registry_register(plugin_registry *r, const plugin_meta *meta);
int plugin_registry_unregister(plugin_registry *r, const char *name);
/* Enable/disable the LATEST version of a plugin. Returns 0 ok, -1 not found. */
int plugin_registry_set_enabled(plugin_registry *r, const char *name, int enabled);

/* Latest version of a plugin (borrowed). NULL if absent. */
const plugin_meta *plugin_registry_find(plugin_registry *r, const char *name);
int plugin_registry_count(plugin_registry *r);

/* 1 if every dependency (of the latest version) is registered. */
int plugin_registry_deps_met(plugin_registry *r, const char *name);
/* JSON grouped by plugin name with a "versions" array (caller frees). */
char *plugin_registry_json(plugin_registry *r);

/* Persist all registered plugins to <state_root>/plugins.json and reload on
 * startup. Load skips exact (name+version) duplicates. */
int plugin_registry_persist(plugin_registry *r, const char *state_root);
int plugin_registry_load(plugin_registry *r, const char *state_root);

#ifdef __cplusplus
}
#endif
