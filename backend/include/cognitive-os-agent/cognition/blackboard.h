/* blackboard.h — shared state space for multi-agent coordination.
 * Agents post partial results (facts, hypotheses, artifacts) to a shared
 * blackboard and read each other's contributions. Thread-safe. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_blackboard coa_blackboard;

coa_blackboard *coa_blackboard_new(void);
void coa_blackboard_free(coa_blackboard *b);

/* Store a key->value entry (value is copied). */
void coa_blackboard_put(coa_blackboard *b, const char *key, const char *val);
/* Fetch a copy of the value for key (caller frees). NULL if absent. */
char *coa_blackboard_get(coa_blackboard *b, const char *key);
/* Remove an entry. Returns 1 if it existed, 0 otherwise. */
int coa_blackboard_remove(coa_blackboard *b, const char *key);
int coa_blackboard_count(coa_blackboard *b);

/* All entries as a JSON object (malloc'd; caller frees). */
char *coa_blackboard_snapshot_json(coa_blackboard *b);

#ifdef __cplusplus
}
#endif
