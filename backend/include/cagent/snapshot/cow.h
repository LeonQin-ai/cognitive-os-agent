/* cow.h — content-addressed block store (Copy-On-Write).
 * Blobs are stored by their FNV-1a-64 content hash (hex, 16 chars), deduplicated
 * by hash. Used by the snapshot engine to preserve original file content. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_cow ca_cow;

/* Open the store rooted at blocks_dir (created if missing). NULL on failure. */
ca_cow *ca_cow_open(const char *blocks_dir);
void ca_cow_close(ca_cow *c);

/* Store a blob. Returns a pointer to the hash string (static, valid until next call). */
const char *ca_cow_put(ca_cow *c, const void *data, size_t len);
/* Fetch a blob by 16-char hex hash. Returns malloc'd data (caller frees) or NULL. */
char *ca_cow_get(ca_cow *c, const char *hash, size_t *len);

#ifdef __cplusplus
}
#endif
