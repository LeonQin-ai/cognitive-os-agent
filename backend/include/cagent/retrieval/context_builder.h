/* context_builder.h — assemble retrieval context from memory for an LLM prompt.
 * Merges recent working memory, keyword matches and vector nearest-neighbors
 * into a single, deduplicated context block. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_memory ca_memory;

/* Build a context block for `query` from memory. Returns a JSON array of
 * {kind,text,result,score} items (malloc'd; caller frees). max_items <= 0 = 8. */
char *ca_context_build(ca_memory *m, const char *query, int max_items);

/* Render a context JSON array (from ca_context_build) into a readable text
 * block suitable for injecting into a prompt. Caller frees. */
char *ca_context_render_text(const char *context_json);

#ifdef __cplusplus
}
#endif
