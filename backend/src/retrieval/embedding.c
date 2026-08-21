/* embedding.c — hashing-trick bag-of-words embeddings. */
#include "cagent/retrieval/embedding.h"
#include "cagent/infra/util.h"

#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>

int ca_embed_dim(void) { return CA_EMBED_DIM; }

void ca_embed_text(const char *text, float *out) {
    memset(out, 0, CA_EMBED_DIM * sizeof(float));
    if (!text) return;

    const char *p = text;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        size_t len = (size_t)(p - s);
        if (len >= 2) {
            uint64_t h = ca_hash64(s, len);
            /* fold 64-bit hash into a bucket index */
            int bucket = (int)(h % (uint64_t)CA_EMBED_DIM);
            out[bucket] += 1.0f;
        }
    }

    /* L2 normalize */
    float norm = 0.0f;
    for (int i = 0; i < CA_EMBED_DIM; i++) norm += out[i] * out[i];
    norm = sqrtf(norm);
    if (norm > 1e-9f) {
        for (int i = 0; i < CA_EMBED_DIM; i++) out[i] /= norm;
    }
}

float ca_embed_cosine(const float *a, const float *b, int dim) {
    if (!a || !b || dim <= 0) return 0.0f;
    double dot = 0.0;
    for (int i = 0; i < dim; i++) dot += (double)a[i] * (double)b[i];
    return (float)dot; /* inputs are unit-normalized, so dot == cosine */
}
