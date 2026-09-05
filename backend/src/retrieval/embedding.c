/* embedding.c — embeddings with a pluggable provider.
 * Local provider: deterministic "hashing trick" bag-of-words into a fixed
 * COA_EMBED_DIM vector, L2-normalized (works fully offline).
 * Remote provider: OpenAI-compatible POST {base}/embeddings.
 * A simple token-overlap rerank scorer is provided for context reordering. */
#include "cognitive-os-agent/retrieval/embedding.h"
#include "cognitive-os-agent/os/http.h"
#include "cognitive-os-agent/infra/util.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include "cJSON.h"

/* ---------- local provider ---------- */

static int utf8_char_len(unsigned char c) {
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static void local_embed(const char *text, float *out) {
    memset(out, 0, COA_EMBED_DIM * sizeof(float));
    if (!text) return;
    const char *p = text;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        size_t len = (size_t)(p - s);
        if (len >= 2) {
            uint64_t h = coa_hash64(s, len);
            int bucket = (int)(h % (uint64_t)COA_EMBED_DIM);
            out[bucket] += 1.0f;
        }
    }
    /* CJK fallback: non-ASCII text produces no alnum tokens (an all-zero
     * vector), so hash overlapping UTF-8 character bigrams — Chinese queries
     * then share buckets with documents containing the same words. */
    const unsigned char *q = (const unsigned char *)text;
    while (*q) {
        if (*q < 0x80) { q++; continue; }
        int cl = utf8_char_len(*q);
        int nl = q[cl] ? utf8_char_len(q[cl]) : 0;
        if (nl > 0 && cl + nl >= 2) {
            uint64_t h = coa_hash64((const char *)q, (size_t)(cl + nl));
            out[h % (uint64_t)COA_EMBED_DIM] += 1.0f;
        }
        q += (size_t)cl;
    }
    float norm = 0.0f;
    for (int i = 0; i < COA_EMBED_DIM; i++) norm += out[i] * out[i];
    norm = sqrtf(norm);
    if (norm > 1e-9f) {
        for (int i = 0; i < COA_EMBED_DIM; i++) out[i] /= norm;
    }
}

/* ---------- remote provider ---------- */

static int g_remote = 0;
static char g_base[512] = "";
static char g_key[512]  = "";
static char g_model[256] = "";

static void l2norm(float *v, int dim) {
    double s = 0;
    for (int i = 0; i < dim; i++) s += (double)v[i] * v[i];
    s = sqrt(s);
    if (s > 1e-9)
        for (int i = 0; i < dim; i++) v[i] = (float)(v[i] / s);
}

static int remote_embed(const char *text, float *out) {
    memset(out, 0, COA_EMBED_DIM * sizeof(float));
    if (!g_base[0]) return -1;

    cJSON *body = cJSON_CreateObject();
    if (g_model[0]) cJSON_AddStringToObject(body, "model", g_model);
    cJSON *input = cJSON_AddArrayToObject(body, "input");
    cJSON_AddItemToArray(input, cJSON_CreateString(text ? text : ""));
    char *js = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!js) return -1;

    coa_strmap headers;
    memset(&headers, 0, sizeof(headers));
    if (g_key[0]) coa_strmap_set(&headers, "Authorization", g_key); /* caller prefixes "Bearer " */

    coa_http_response *r = coa_http_post(g_base, "/embeddings", js, "application/json",
                                       &headers, 15000);
    free(js);
    coa_strmap_free(&headers);
    if (!r || r->status != 200 || !r->body) {
        if (r) coa_http_response_free(r);
        return -1;
    }

    cJSON *root = cJSON_Parse(r->body);
    coa_http_response_free(r);
    if (!root) return -1;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *first = data && cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : NULL;
    cJSON *emb = first ? cJSON_GetObjectItemCaseSensitive(first, "embedding") : NULL;
    int rc = -1;
    if (emb && cJSON_IsArray(emb)) {
        int n = cJSON_GetArraySize(emb);
        if (n > COA_EMBED_DIM) n = COA_EMBED_DIM;
        for (int i = 0; i < n; i++) {
            cJSON *v = cJSON_GetArrayItem(emb, i);
            if (v && cJSON_IsNumber(v)) out[i] = (float)v->valuedouble;
        }
        l2norm(out, COA_EMBED_DIM);
        rc = 0;
    }
    cJSON_Delete(root);
    return rc;
}

/* ---------- public API ---------- */

int coa_embed_dim(void) { return COA_EMBED_DIM; }

void coa_embed_text(const char *text, float *out) {
    if (!out) return;
    if (g_remote && remote_embed(text, out) == 0) return;
    local_embed(text, out);
}

float coa_embed_cosine(const float *a, const float *b, int dim) {
    if (!a || !b || dim <= 0) return 0.0f;
    double dot = 0.0;
    for (int i = 0; i < dim; i++) dot += (double)a[i] * (double)b[i];
    return (float)dot; /* unit-normalized: dot == cosine */
}

void coa_embedding_use_local(void) {
    g_remote = 0;
    g_base[0] = g_key[0] = g_model[0] = '\0';
}

int coa_embedding_use_remote(const char *base_url, const char *api_key, const char *model) {
    if (!base_url || !*base_url) return -1;
    snprintf(g_base, sizeof(g_base), "%s", base_url);
    /* strip a trailing slash so we always build {base}/embeddings */
    size_t l = strlen(g_base);
    while (l > 0 && g_base[l - 1] == '/') g_base[--l] = '\0';
    g_key[0] = '\0';
    if (api_key && *api_key) {
        if (strncmp(api_key, "Bearer ", 7) == 0 || strncmp(api_key, "bearer ", 7) == 0)
            snprintf(g_key, sizeof(g_key), "%s", api_key);
        else
            snprintf(g_key, sizeof(g_key), "Bearer %s", api_key);
    }
    snprintf(g_model, sizeof(g_model), "%s", model ? model : "");
    g_remote = 1;
    return 0;
}

const char *coa_embedding_provider_name(void) {
    return g_remote ? "remote" : "local";
}

/* ---------- rerank (token overlap + char-bigram blend) ---------- */

/* Jaccard similarity over character bigrams of the alphanumeric lowercased
 * text. Catches morphological variants the exact token match misses
 * ("connect"~"connects"). Result in [0,1]. */
static float char_bigram_jaccard(const char *a, const char *b) {
    /* bigram sets bounded to a fixed cap (256 each) — enough for rerank */
    unsigned short A[256], B[256];
    int na = 0, nb = 0;
    const char *srcs[2] = {a, b};
    unsigned short *dsts[2] = {A, B};
    int *cnts[2] = {&na, &nb};
    for (int s = 0; s < 2; s++) {
        unsigned char prev = 0;
        const char *p = srcs[s] ? srcs[s] : "";
        while (*p) {
            unsigned char c = (unsigned char)*p;
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
            if (isalnum(c)) {
                if (prev && *cnts[s] < 256)
                    dsts[s][(*cnts[s])++] = (unsigned short)((prev << 8) | c);
                prev = c;
            } else {
                prev = 0;
            }
            p++;
        }
    }
    if (na == 0 || nb == 0) return 0.0f;
    int inter = 0;
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (A[i] == B[j]) { inter++; break; }
        }
    }
    int uni = na + nb - inter;
    return uni > 0 ? (float)inter / (float)uni : 0.0f;
}

static int count_tokens(const char *s) {
    int n = 0;
    const char *p = s ? s : "";
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        if ((size_t)(p - start) >= 2) n++;
    }
    return n;
}

static int token_overlap(const char *q, const char *doc) {
    int hits = 0;
    const char *p = q ? q : "";
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        size_t len = (size_t)(p - s);
        if (len >= 2) {
            /* case-insensitive substring-at-word-boundary match */
            const char *d = doc ? doc : "";
            while (*d) {
                while (*d && !isalnum((unsigned char)*d)) d++;
                if (!*d) break;
                const char *ds = d;
                while (*d && isalnum((unsigned char)*d)) d++;
                size_t dlen = (size_t)(d - ds);
                if (dlen == len) {
                    int eq = 1;
                    for (size_t i = 0; i < len; i++) {
                        int a = (unsigned char)s[i], b = (unsigned char)ds[i];
                        if (a >= 'A' && a <= 'Z') a += 32;
                        if (b >= 'A' && b <= 'Z') b += 32;
                        if (a != b) { eq = 0; break; }
                    }
                    if (eq) { hits++; break; }
                }
            }
        }
    }
    return hits;
}

int coa_embed_keyword_score(const char *query, const char *doc, float *score_out) {
    if (!query || !doc || !score_out) return -1;
    int qt = count_tokens(query);
    float overlap = (qt > 0) ? (float)token_overlap(query, doc) / (float)qt : 0.0f;
    float bigr = char_bigram_jaccard(query, doc);
    *score_out = 0.6f * overlap + 0.4f * bigr;
    return 0;
}

int coa_embed_rerank(const char *query, const char **docs, size_t n, float *scores_out) {
    if (!query || !docs || !scores_out) return -1;
    for (size_t i = 0; i < n; i++)
        coa_embed_keyword_score(query, docs[i] ? docs[i] : "", &scores_out[i]);
    return 0;
}
