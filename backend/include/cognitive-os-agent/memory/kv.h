/* kv.h — thread-safe key/value store (long-term fact memory). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kvstore kvstore;

kvstore *kvstore_new(void);
void kvstore_free(kvstore *k);

/* Set a key to val (copied). val == NULL removes the key. */
void kvstore_set(kvstore *k, const char *key, const char *val);
/* Borrowed pointer, or NULL if absent. Do not free. */
const char *kvstore_get(kvstore *k, const char *key);
int kvstore_remove(kvstore *k, const char *key);
int kvstore_count(kvstore *k);

/* All entries as a JSON object (malloc'd; caller frees). */
char *kvstore_snapshot_json(kvstore *k);

#ifdef __cplusplus
}
#endif
