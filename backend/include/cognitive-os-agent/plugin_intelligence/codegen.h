/* codegen.h — plugin code generation.
 * Emits a C plugin skeleton implementing the cognitive-os-agent plugin entry contract. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Generate a C source skeleton for a plugin `name` with `description`.
 * Returns a malloc'd string (caller frees). */
char *coa_codegen_plugin(const char *name, const char *description);

#ifdef __cplusplus
}
#endif
