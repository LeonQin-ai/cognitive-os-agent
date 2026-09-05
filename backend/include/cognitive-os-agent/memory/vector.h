/* vector.h — in-memory vector store with cosine nearest-neighbor recall.
 * Entries carry an id, a text (embedded with coa_embed_text), and an optional
 * metadata string. This is the "vector-lite" memory over the retrieval layer. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_vectorstore coa_vectorstore;

coa_vectorstore *coa_vectorstore_new(void);
void coa_vectorstore_free(coa_vectorstore *v);

/* Add/update an entry (text embedded internally; meta may be NULL). Returns 0 ok. */
int coa_vectorstore_add(coa_vectorstore *v, const char *id, const char *text, const char *meta);
int coa_vectorstore_count(coa_vectorstore *v);

/* Top-k nearest entries to `query` by cosine similarity.
 * Returns a JSON array of {id,text,meta,score} (malloc'd; caller frees). */
char *coa_vectorstore_nearest(coa_vectorstore *v, const char *query, int k);

/* Hybrid retrieval: score = w_vec*cosine + (1-w_vec)*keyword_overlap
 * (rerank-style token+bigram blend). w_vec clamped to [0,1].
 * Same JSON shape as coa_vectorstore_nearest. */
char *coa_vectorstore_nearest_hybrid(coa_vectorstore *v, const char *query,
                                    int k, float w_vec);

/* Multi-query (MQE) merge: retrieve per query variant and merge per entry
 * with the max score. Same JSON shape as coa_vectorstore_nearest. */
char *coa_vectorstore_nearest_multi(coa_vectorstore *v, const char *const *queries,
                                   int nq, int k);

#ifdef __cplusplus
}
#endif
