/* blackboard.h — shared state space for multi-agent coordination.
 * Agents post partial results (facts, hypotheses, artifacts) to a shared
 * blackboard and read each other's contributions. Thread-safe. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_blackboard ca_blackboard;

ca_blackboard *ca_blackboard_new(void);
void ca_blackboard_free(ca_blackboard *b);

/* Store a key->value entry (value is copied). */
void ca_blackboard_put(ca_blackboard *b, const char *key, const char *val);
/* Fetch a copy of the value for key (caller frees). NULL if absent. */
char *ca_blackboard_get(ca_blackboard *b, const char *key);
/* Remove an entry. Returns 1 if it existed, 0 otherwise. */
int ca_blackboard_remove(ca_blackboard *b, const char *key);
int ca_blackboard_count(ca_blackboard *b);

/* All entries as a JSON object (malloc'd; caller frees). */
char *ca_blackboard_snapshot_json(ca_blackboard *b);

#ifdef __cplusplus
}
#endif
