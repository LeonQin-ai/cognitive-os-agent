/* plugin.h — dynamic module loading (dlopen / LoadLibrary). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_plugin ca_plugin;

/* Load a shared library (.so/.dll). NULL on failure (see ca_plugin_error). */
ca_plugin *ca_plugin_load(const char *path);
/* Look up an exported symbol. NULL if not found. */
void *ca_plugin_symbol(ca_plugin *p, const char *name);
/* Last error message (static buffer). */
const char *ca_plugin_error(void);
void ca_plugin_unload(ca_plugin *p);

#ifdef __cplusplus
}
#endif
