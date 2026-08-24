/* embedding.c — embeddings with a pluggable provider.
 * Local provider: deterministic "hashing trick" bag-of-words into a fixed
 * CA_EMBED_DIM vector, L2-normalized (works fully offline).
 * Remote provider: OpenAI-compatible POST {base}/embeddings.
 * A simple token-overlap rerank scorer is provided for context reordering. */
#include "cagent/retrieval/embedding.h"
#include "cagent/os/http.h"
#include "cagent/infra/util.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include "cJSON.h"

/* ---------- local provider ---------- */

static void local_embed(const char *text, float *out) {
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
            int bucket = (int)(h % (uint64_t)CA_EMBED_DIM);
            out[bucket] += 1.0f;
        }
    }
    float norm = 0.0f;
    for (int i = 0; i < CA_EMBED_DIM; i++) norm += out[i] * out[i];
    norm = sqrtf(norm);
    if (norm > 1e-9f) {
        for (int i = 0; i < CA_EMBED_DIM; i++) out[i] /= norm;
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
    memset(out, 0, CA_EMBED_DIM * sizeof(float));
    if (!g_base[0]) return -1;

    cJSON *body = cJSON_CreateObject();
    if (g_model[0]) cJSON_AddStringToObject(body, "model", g_model);
    cJSON *input = cJSON_AddArrayToObject(body, "input");
    cJSON_AddItemToArray(input, cJSON_CreateString(text ? text : ""));
    char *js = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!js) return -1;

    ca_strmap headers;
    memset(&headers, 0, sizeof(headers));
    if (g_key[0]) ca_strmap_set(&headers, "Authorization", g_key); /* caller prefixes "Bearer " */

    ca_http_response *r = ca_http_post(g_base, "/embeddings", js, "application/json",
                                       &headers, 15000);
    free(js);
    ca_strmap_free(&headers);
    if (!r || r->status != 200 || !r->body) {
        if (r) ca_http_response_free(r);
        return -1;
    }

    cJSON *root = cJSON_Parse(r->body);
    ca_http_response_free(r);
    if (!root) return -1;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *first = data && cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : NULL;
    cJSON *emb = first ? cJSON_GetObjectItemCaseSensitive(first, "embedding") : NULL;
    int rc = -1;
    if (emb && cJSON_IsArray(emb)) {
        int n = cJSON_GetArraySize(emb);
        if (n > CA_EMBED_DIM) n = CA_EMBED_DIM;
        for (int i = 0; i < n; i++) {
            cJSON *v = cJSON_GetArrayItem(emb, i);
            if (v && cJSON_IsNumber(v)) out[i] = (float)v->valuedouble;
        }
        l2norm(out, CA_EMBED_DIM);
        rc = 0;
    }
    cJSON_Delete(root);
    return rc;
}

/* ---------- public API ---------- */

int ca_embed_dim(void) { return CA_EMBED_DIM; }

void ca_embed_text(const char *text, float *out) {
    if (!out) return;
    if (g_remote && remote_embed(text, out) == 0) return;
    local_embed(text, out);
}

float ca_embed_cosine(const float *a, const float *b, int dim) {
    if (!a || !b || dim <= 0) return 0.0f;
    double dot = 0.0;
    for (int i = 0; i < dim; i++) dot += (double)a[i] * (double)b[i];
    return (float)dot; /* unit-normalized: dot == cosine */
}

void ca_embedding_use_local(void) {
    g_remote = 0;
    g_base[0] = g_key[0] = g_model[0] = '\0';
}

int ca_embedding_use_remote(const char *base_url, const char *api_key, const char *model) {
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

const char *ca_embedding_provider_name(void) {
    return g_remote ? "remote" : "local";
}

/* ---------- rerank (token overlap) ---------- */

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

int ca_embed_rerank(const char *query, const char **docs, size_t n, float *scores_out) {
    if (!query || !docs || !scores_out) return -1;
    int qt = count_tokens(query);
    for (size_t i = 0; i < n; i++) {
        float overlap = (float)token_overlap(query, docs[i]);
        scores_out[i] = (qt > 0) ? overlap / (float)qt : 0.0f;
    }
    return 0;
}
