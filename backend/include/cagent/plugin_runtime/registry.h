/* registry.h — plugin metadata registry (the ⭐ Plugin Registry in the
 * architecture). Stores versioned plugin descriptors: capability list,
 * content signature (hash), dependencies, enabled state. Multiple versions of
 * the same plugin coexist; ca_plugin_registry_find returns the latest. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_plugin_registry ca_plugin_registry;

typedef struct ca_plugin_meta {
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
} ca_plugin_meta;

ca_plugin_registry *ca_plugin_registry_new(void);
void ca_plugin_registry_free(ca_plugin_registry *r);

/* Add a new version. Returns 0 ok, -1 error (invalid/duplicate same-version). */
int ca_plugin_registry_register(ca_plugin_registry *r, const ca_plugin_meta *meta);
int ca_plugin_registry_unregister(ca_plugin_registry *r, const char *name);
/* Enable/disable the LATEST version of a plugin. Returns 0 ok, -1 not found. */
int ca_plugin_registry_set_enabled(ca_plugin_registry *r, const char *name, int enabled);

/* Latest version of a plugin (borrowed). NULL if absent. */
const ca_plugin_meta *ca_plugin_registry_find(ca_plugin_registry *r, const char *name);
int ca_plugin_registry_count(ca_plugin_registry *r);
/* Versioned entry by index (all versions, borrowed). */
const ca_plugin_meta *ca_plugin_registry_get(ca_plugin_registry *r, size_t i);

/* 1 if every dependency (of the latest version) is registered. */
int ca_plugin_registry_deps_met(ca_plugin_registry *r, const char *name);
/* JSON grouped by plugin name with a "versions" array (caller frees). */
char *ca_plugin_registry_json(ca_plugin_registry *r);

#ifdef __cplusplus
}
#endif
