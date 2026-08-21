/* memory.h — cognitive memory facade.
 * Composes fine-grained sub-stores (see the cagent/memory headers):
 *  - working memory:  short-term ring buffer of recent items (inline)
 *  - long-term facts: ca_kvstore (memory/kv.h)
 *  - episodes:        ca_episodic (memory/episode.h)
 *  - vector-lite:     ca_vectorstore over working + episodes (memory/vector.h)
 * Long-term facts are persisted as JSON under the state root. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_memory ca_memory;

/* Load (or create) memory in <state_root>/memory. NULL on failure. */
ca_memory *ca_memory_new(const char *state_root);
void ca_memory_free(ca_memory *m);

/* Working memory (bounded ring buffer of recent strings). */
void ca_memory_working_push(ca_memory *m, const char *text);
int ca_memory_working_count(ca_memory *m);
/* Borrowed i-th working item (0 = newest). Do not free. */
const char *ca_memory_working_at(ca_memory *m, int i);

/* Long-term facts. value may be NULL to delete the key. */
void ca_memory_remember(ca_memory *m, const char *key, const char *value);
/* Returns a borrowed pointer, or NULL. */
const char *ca_memory_recall(ca_memory *m, const char *key);

/* Experience: record a completed episode. */
void ca_memory_record_experience(ca_memory *m, const char *task, const char *result);

/* Keyword search across working + episode items (JSON array of {kind,text,score}). */
char *ca_memory_search(ca_memory *m, const char *query, int limit);

/* Vector nearest-neighbor recall across mirrored working + episode items.
 * Returns a JSON array of {id,text,meta,score} (caller frees). */
char *ca_memory_retrieve(ca_memory *m, const char *query, int k);

/* Persist long-term facts to disk. */
void ca_memory_flush(ca_memory *m);

/* Render working memory as a JSON array of strings (caller frees). */
char *ca_memory_working_json(ca_memory *m);
/* Render long-term store as a JSON object (caller frees). */
char *ca_memory_longterm_json(ca_memory *m);
/* Render all episodes as a JSON array of {task,result} (caller frees). */
char *ca_memory_episodes_json(ca_memory *m);

#ifdef __cplusplus
}
#endif
