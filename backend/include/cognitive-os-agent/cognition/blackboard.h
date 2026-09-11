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

#ifdef __cplusplus
}
#endif
