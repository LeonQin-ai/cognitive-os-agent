/* plugin.h — dynamic module loading (dlopen / LoadLibrary). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct plugin plugin;

/* Load a shared library (.so/.dll). NULL on failure (see plugin_error). */
plugin *plugin_load(const char *path);
/* Look up an exported symbol. NULL if not found. */
void *plugin_symbol(plugin *p, const char *name);
/* Last error message (static buffer). */
const char *plugin_error(void);
void plugin_unload(plugin *p);

#ifdef __cplusplus
}
#endif
