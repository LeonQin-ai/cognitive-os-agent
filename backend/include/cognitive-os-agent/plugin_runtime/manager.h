/* plugin.h — dynamic module loading (dlopen / LoadLibrary). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_plugin coa_plugin;

/* Load a shared library (.so/.dll). NULL on failure (see coa_plugin_error). */
coa_plugin *coa_plugin_load(const char *path);
/* Look up an exported symbol. NULL if not found. */
void *coa_plugin_symbol(coa_plugin *p, const char *name);
/* Last error message (static buffer). */
const char *coa_plugin_error(void);
void coa_plugin_unload(coa_plugin *p);

#ifdef __cplusplus
}
#endif
