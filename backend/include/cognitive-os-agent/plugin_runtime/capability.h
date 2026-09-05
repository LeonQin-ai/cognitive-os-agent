/* capability.h — capability tokens for fine-grained permission checks.
 * Named capabilities (e.g. "fs.read", "net", "proc.exec") are granted to
 * plugins; the runtime checks membership (with prefix wildcards) before
 * allowing a sensitive operation. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_capability ca_capability;

ca_capability *ca_capability_new(void);
void ca_capability_free(ca_capability *c);

/* Grant a capability. 0 ok, -1 duplicate/empty. */
int ca_capability_grant(ca_capability *c, const char *cap);
/* Revoke a capability. Returns 1 if it existed, 0 otherwise. */
int ca_capability_revoke(ca_capability *c, const char *cap);
int ca_capability_has(ca_capability *c, const char *cap);
int ca_capability_count(ca_capability *c);

/* Wildcard match: "fs.*" matches "fs.read". Returns 1 if any granted cap matches. */
int ca_capability_match(ca_capability *c, const char *pattern);

/* All granted caps as a JSON array (malloc'd; caller frees). */
char *ca_capability_json(ca_capability *c);

#ifdef __cplusplus
}
#endif
