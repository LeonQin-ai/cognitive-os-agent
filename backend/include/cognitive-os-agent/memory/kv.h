/* kv.h — thread-safe key/value store (long-term fact memory). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_kvstore coa_kvstore;

coa_kvstore *coa_kvstore_new(void);
void coa_kvstore_free(coa_kvstore *k);

/* Set a key to val (copied). val == NULL removes the key. */
void coa_kvstore_set(coa_kvstore *k, const char *key, const char *val);
/* Borrowed pointer, or NULL if absent. Do not free. */
const char *coa_kvstore_get(coa_kvstore *k, const char *key);
int coa_kvstore_remove(coa_kvstore *k, const char *key);
int coa_kvstore_count(coa_kvstore *k);

/* All entries as a JSON object (malloc'd; caller frees). */
char *coa_kvstore_snapshot_json(coa_kvstore *k);

#ifdef __cplusplus
}
#endif
