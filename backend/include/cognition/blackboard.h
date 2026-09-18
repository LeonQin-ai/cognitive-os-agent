/* blackboard.h — shared state space for multi-agent coordination.
 * Agents post partial results (facts, hypotheses, artifacts) to a shared
 * blackboard and read each other's contributions. Thread-safe. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct blackboard blackboard;

blackboard *blackboard_new(void);
void blackboard_free(blackboard *b);

/* Store a key->value entry (value is copied). */
void blackboard_put(blackboard *b, const char *key, const char *val);
/* Fetch a copy of the value for key (caller frees). NULL if absent. */
char *blackboard_get(blackboard *b, const char *key);
/* Remove an entry. Returns 1 if it existed, 0 otherwise. */
int blackboard_remove(blackboard *b, const char *key);
int blackboard_count(blackboard *b);

/* All entries as a JSON object (malloc'd; caller frees). */
char *blackboard_snapshot_json(blackboard *b);

/* Persistence: mirror every put/remove to <path> as a JSON object snapshot
 * (written under the board mutex; small files, low write frequency).
 * NULL path disables saving. Applies to later mutations only. */
void blackboard_set_persist(blackboard *b, const char *path);
/* Load entries from a JSON object snapshot written by the saver (merge:
 * existing keys with the same name are overwritten). Returns 0 ok, -1 on
 * missing/unparsable file. */
int blackboard_load_json(blackboard *b, const char *path);

#ifdef __cplusplus
}
#endif
