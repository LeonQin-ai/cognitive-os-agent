/* vector.h — in-memory vector store with cosine nearest-neighbor recall.
 * Entries carry an id, a text (embedded with ca_embed_text), and an optional
 * metadata string. This is the "vector-lite" memory over the retrieval layer. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_vectorstore ca_vectorstore;

ca_vectorstore *ca_vectorstore_new(void);
void ca_vectorstore_free(ca_vectorstore *v);

/* Add/update an entry (text embedded internally; meta may be NULL). Returns 0 ok. */
int ca_vectorstore_add(ca_vectorstore *v, const char *id, const char *text, const char *meta);
int ca_vectorstore_count(ca_vectorstore *v);

/* Top-k nearest entries to `query` by cosine similarity.
 * Returns a JSON array of {id,text,meta,score} (malloc'd; caller frees). */
char *ca_vectorstore_nearest(ca_vectorstore *v, const char *query, int k);

#ifdef __cplusplus
}
#endif
