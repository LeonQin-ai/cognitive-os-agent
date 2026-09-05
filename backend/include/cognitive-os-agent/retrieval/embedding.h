/* embedding.h — text embeddings with a pluggable provider.
 * Default provider is the deterministic local "hashing trick" bag-of-words
 * (vector-lite, no external deps). A remote provider can be enabled via
 * coa_embedding_use_remote() for OpenAI-compatible /embeddings endpoints
 * (DeepSeek / Ollama / vLLM / Gemini OpenAI-compat …). Also exposes a simple
 * rerank scoring hook for context reordering. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COA_EMBED_DIM 256

int coa_embed_dim(void);

/* Embed `text` into a COA_EMBED_DIM float vector (L2-normalized) using the
 * currently configured provider. */
void coa_embed_text(const char *text, float *out);

/* Cosine similarity between two COA_EMBED_DIM vectors. */
float coa_embed_cosine(const float *a, const float *b, int dim);

/* --- provider configuration --- */

/* Reset to the built-in local provider (default). */
void coa_embedding_use_local(void);

/* Switch to a remote OpenAI-compatible embeddings endpoint.
 * base_url like "https://api.deepseek.com/v1"; model like "deepseek-embed".
 * Pass NULL api_key/model to omit. Returns 0 ok, -1 bad base_url. */
int coa_embedding_use_remote(const char *base_url, const char *api_key, const char *model);

/* "local" or "remote" (or NULL if no provider). */
const char *coa_embedding_provider_name(void);

/* --- rerank --- */
/* Score each doc for relevance to query (larger = more relevant). scores_out
 * receives n floats: 0.6*token-overlap + 0.4*char-bigram Jaccard. Returns 0 ok. */
int coa_embed_rerank(const char *query, const char **docs, size_t n, float *scores_out);

/* Keyword relevance of a single doc (the same scorer coa_embed_rerank applies
 * per doc), for hybrid retrieval blending. Returns 0 ok. */
int coa_embed_keyword_score(const char *query, const char *doc, float *score_out);

#ifdef __cplusplus
}
#endif
