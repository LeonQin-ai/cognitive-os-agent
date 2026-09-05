/* capability.h — capability tokens for fine-grained permission checks.
 * Named capabilities (e.g. "fs.read", "net", "proc.exec") are granted to
 * plugins; the runtime checks membership (with prefix wildcards) before
 * allowing a sensitive operation. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_capability coa_capability;

coa_capability *coa_capability_new(void);
void coa_capability_free(coa_capability *c);

/* Grant a capability. 0 ok, -1 duplicate/empty. */
int coa_capability_grant(coa_capability *c, const char *cap);
/* Revoke a capability. Returns 1 if it existed, 0 otherwise. */
int coa_capability_revoke(coa_capability *c, const char *cap);
int coa_capability_has(coa_capability *c, const char *cap);
int coa_capability_count(coa_capability *c);

/* Wildcard match: "fs.*" matches "fs.read". Returns 1 if any granted cap matches. */
int coa_capability_match(coa_capability *c, const char *pattern);

/* All granted caps as a JSON array (malloc'd; caller frees). */
char *coa_capability_json(coa_capability *c);

#ifdef __cplusplus
}
#endif
