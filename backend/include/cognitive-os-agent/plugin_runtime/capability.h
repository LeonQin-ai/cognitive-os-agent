/* capability.h — capability tokens for fine-grained permission checks.
 * Named capabilities (e.g. "fs.read", "net", "proc.exec") are granted to
 * plugins; the runtime checks membership (with prefix wildcards) before
 * allowing a sensitive operation. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct capability capability;

capability *capability_new(void);
void capability_free(capability *c);

/* Grant a capability. 0 ok, -1 duplicate/empty. */
int capability_grant(capability *c, const char *cap);
/* Revoke a capability. Returns 1 if it existed, 0 otherwise. */
int capability_revoke(capability *c, const char *cap);
int capability_has(capability *c, const char *cap);
int capability_count(capability *c);

/* Wildcard match: "fs.*" matches "fs.read". Returns 1 if any granted cap matches. */
int capability_match(capability *c, const char *pattern);

/* All granted caps as a JSON array (malloc'd; caller frees). */
char *capability_json(capability *c);

#ifdef __cplusplus
}
#endif
