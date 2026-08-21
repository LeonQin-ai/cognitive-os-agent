/* embedding.h — lightweight text embeddings.
 * Deterministic "hashing trick" bag-of-words: tokens are hashed into a fixed
 * dimension and the resulting vector is L2-normalized. Good enough for
 * vector-lite nearest-neighbor recall without an external model. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define CA_EMBED_DIM 256

int ca_embed_dim(void);

/* Embed `text` into a CA_EMBED_DIM float vector (L2-normalized). */
void ca_embed_text(const char *text, float *out);

/* Cosine similarity between two CA_EMBED_DIM vectors. */
float ca_embed_cosine(const float *a, const float *b, int dim);

#ifdef __cplusplus
}
#endif
