/* embedding.h — text embeddings with a pluggable provider.
 * Default provider is the deterministic local "hashing trick" bag-of-words
 * (vector-lite, no external deps). A remote provider can be enabled via
 * ca_embedding_use_remote() for OpenAI-compatible /embeddings endpoints
 * (DeepSeek / Ollama / vLLM / Gemini OpenAI-compat …). Also exposes a simple
 * rerank scoring hook for context reordering. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CA_EMBED_DIM 256

int ca_embed_dim(void);

/* Embed `text` into a CA_EMBED_DIM float vector (L2-normalized) using the
 * currently configured provider. */
void ca_embed_text(const char *text, float *out);

/* Cosine similarity between two CA_EMBED_DIM vectors. */
float ca_embed_cosine(const float *a, const float *b, int dim);

/* --- provider configuration --- */

/* Reset to the built-in local provider (default). */
void ca_embedding_use_local(void);

/* Switch to a remote OpenAI-compatible embeddings endpoint.
 * base_url like "https://api.deepseek.com/v1"; model like "deepseek-embed".
 * Pass NULL api_key/model to omit. Returns 0 ok, -1 bad base_url. */
int ca_embedding_use_remote(const char *base_url, const char *api_key, const char *model);

/* "local" or "remote" (or NULL if no provider). */
const char *ca_embedding_provider_name(void);

/* --- rerank --- */
/* Score each doc for relevance to query (larger = more relevant). scores_out
 * receives n floats. Uses token-overlap for local; remote providers may
 * upgrade this later. Returns 0 ok. */
int ca_embed_rerank(const char *query, const char **docs, size_t n, float *scores_out);

#ifdef __cplusplus
}
#endif
