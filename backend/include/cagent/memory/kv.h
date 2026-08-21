/* kv.h — thread-safe key/value store (long-term fact memory). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_kvstore ca_kvstore;

ca_kvstore *ca_kvstore_new(void);
void ca_kvstore_free(ca_kvstore *k);

/* Set a key to val (copied). val == NULL removes the key. */
void ca_kvstore_set(ca_kvstore *k, const char *key, const char *val);
/* Borrowed pointer, or NULL if absent. Do not free. */
const char *ca_kvstore_get(ca_kvstore *k, const char *key);
int ca_kvstore_remove(ca_kvstore *k, const char *key);
int ca_kvstore_count(ca_kvstore *k);

/* All entries as a JSON object (malloc'd; caller frees). */
char *ca_kvstore_snapshot_json(ca_kvstore *k);

#ifdef __cplusplus
}
#endif
