/* vector.c — in-memory vector store with cosine nearest-neighbor recall. */
#include "cognitive-os-agent/memory/vector.h"
#include "cognitive-os-agent/retrieval/embedding.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

typedef struct vec_entry {
    char *id;
    char *text;
    char *meta;
    float vec[COA_EMBED_DIM];
} vec_entry;

struct coa_vectorstore {
    coa_mutex mtx;
    vec_entry *items;
    size_t count;
    size_t cap;
};

coa_vectorstore *coa_vectorstore_new(void) {
    coa_vectorstore *v = (coa_vectorstore *)calloc(1, sizeof(*v));
    if (!v) return NULL;
    coa_mutex_init(&v->mtx);
    return v;
}

static void entry_clear(vec_entry *e) {
    free(e->id);
    free(e->text);
    free(e->meta);
    memset(e, 0, sizeof(*e));
}

void coa_vectorstore_free(coa_vectorstore *v) {
    if (!v) return;
    coa_mutex_lock(&v->mtx);
    for (size_t i = 0; i < v->count; i++) entry_clear(&v->items[i]);
    free(v->items);
    v->items = NULL;
    v->count = v->cap = 0;
    coa_mutex_unlock(&v->mtx);
    coa_mutex_destroy(&v->mtx);
    free(v);
}

int coa_vectorstore_add(coa_vectorstore *v, const char *id, const char *text, const char *meta) {
    if (!v || !id || !text) return -1;
    coa_mutex_lock(&v->mtx);
    /* update existing id */
    for (size_t i = 0; i < v->count; i++) {
        if (strcmp(v->items[i].id, id) == 0) {
            free(v->items[i].text);
            free(v->items[i].meta);
            v->items[i].text = coa_strdup(text);
            v->items[i].meta = coa_strdup(meta ? meta : "");
            coa_embed_text(text, v->items[i].vec);
            coa_mutex_unlock(&v->mtx);
            return 0;
        }
    }
    if (v->count == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 8;
        vec_entry *ni = (vec_entry *)realloc(v->items, cap * sizeof(vec_entry));
        if (!ni) { coa_mutex_unlock(&v->mtx); return -1; }
        v->items = ni;
        v->cap = cap;
    }
    vec_entry *e = &v->items[v->count++];
    memset(e, 0, sizeof(*e));
    e->id = coa_strdup(id);
    e->text = coa_strdup(text);
    e->meta = coa_strdup(meta ? meta : "");
    coa_embed_text(text, e->vec);
    coa_mutex_unlock(&v->mtx);
    return 0;
}

int coa_vectorstore_count(coa_vectorstore *v) {
    if (!v) return 0;
    coa_mutex_lock(&v->mtx);
    int n = (int)v->count;
    coa_mutex_unlock(&v->mtx);
    return n;
}

char *coa_vectorstore_nearest(coa_vectorstore *v, const char *query, int k) {
    if (!v) return coa_strdup("[]");
    float qvec[COA_EMBED_DIM];
    coa_embed_text(query, qvec);

    coa_mutex_lock(&v->mtx);
    /* collect top-k with a simple insertion into a small sorted list of indices */
    int *top_idx = (int *)malloc(k > 0 ? (size_t)k * sizeof(int) : sizeof(int));
    float *top_score = (float *)malloc(k > 0 ? (size_t)k * sizeof(float) : sizeof(float));
    int ntop = 0;
    if (!top_idx || !top_score) {
        free(top_idx);
        free(top_score);
        coa_mutex_unlock(&v->mtx);
        return coa_strdup("[]");
    }

    for (size_t i = 0; i < v->count; i++) {
        float s = coa_embed_cosine(qvec, v->items[i].vec, COA_EMBED_DIM);
        if (k > 0 && ntop >= k && s <= top_score[ntop - 1]) continue;
        int pos = ntop;
        while (pos > 0 && top_score[pos - 1] < s) { pos--; }
        if (k > 0 && ntop == k) {
            /* drop the lowest */
            for (int j = ntop - 1; j > pos; j--) { top_idx[j] = top_idx[j - 1]; top_score[j] = top_score[j - 1]; }
        } else {
            ntop++;
            for (int j = ntop - 1; j > pos; j--) { top_idx[j] = top_idx[j - 1]; top_score[j] = top_score[j - 1]; }
        }
        if (pos < ntop) { top_idx[pos] = (int)i; top_score[pos] = s; }
    }

    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (int i = 0; i < ntop; i++) {
            vec_entry *e = &v->items[top_idx[i]];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", e->id);
            cJSON_AddStringToObject(o, "text", e->text);
            cJSON_AddStringToObject(o, "meta", e->meta);
            cJSON_AddNumberToObject(o, "score", (double)top_score[i]);
            cJSON_AddItemToArray(arr, o);
        }
    }
    free(top_idx);
    free(top_score);
    coa_mutex_unlock(&v->mtx);

    char *s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr) cJSON_Delete(arr);
    return s ? s : coa_strdup("[]");
}

/* ---------- hybrid + multi-query retrieval (shared helpers) ---------- */

/* Caller holds v->mtx. score = w_vec*cosine + (1-w_vec)*keyword for every
 * entry. w_vec >= 0.999 skips the keyword pass (pure vector). */
static void score_all(coa_vectorstore *v, const char *query, float w_vec,
                      float *out) {
    float qvec[COA_EMBED_DIM];
    coa_embed_text(query, qvec);
    int do_kw = w_vec < 0.999f;
    for (size_t i = 0; i < v->count; i++) {
        float cos = coa_embed_cosine(qvec, v->items[i].vec, COA_EMBED_DIM);
        if (cos < 0) cos = 0;
        float s = w_vec * cos;
        if (do_kw) {
            float kw = 0;
            coa_embed_keyword_score(query, v->items[i].text, &kw);
            s += (1.0f - w_vec) * kw;
        }
        out[i] = s;
    }
}

/* Caller holds v->mtx. Top-k by the given per-entry scores -> JSON array. */
static char *topk_json(coa_vectorstore *v, const float *scores, int k) {
    if (k <= 0) return coa_strdup("[]");
    int *top_idx = (int *)malloc((size_t)k * sizeof(int));
    float *top_score = (float *)malloc((size_t)k * sizeof(float));
    if (!top_idx || !top_score) { free(top_idx); free(top_score); return coa_strdup("[]"); }
    int ntop = 0;
    for (size_t i = 0; i < v->count; i++) {
        float s = scores[i];
        if (ntop >= k && s <= top_score[ntop - 1]) continue;
        int pos = ntop < k ? ntop : k - 1;
        while (pos > 0 && top_score[pos - 1] < s) pos--;
        if (ntop < k) ntop++;
        for (int j = ntop - 1; j > pos; j--) {
            top_idx[j] = top_idx[j - 1];
            top_score[j] = top_score[j - 1];
        }
        top_idx[pos] = (int)i;
        top_score[pos] = s;
    }
    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (int i = 0; i < ntop; i++) {
            vec_entry *e = &v->items[top_idx[i]];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", e->id);
            cJSON_AddStringToObject(o, "text", e->text);
            cJSON_AddStringToObject(o, "meta", e->meta);
            cJSON_AddNumberToObject(o, "score", (double)top_score[i]);
            cJSON_AddItemToArray(arr, o);
        }
    }
    free(top_idx);
    free(top_score);
    char *s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr) cJSON_Delete(arr);
    return s ? s : coa_strdup("[]");
}

char *coa_vectorstore_nearest_hybrid(coa_vectorstore *v, const char *query,
                                    int k, float w_vec) {
    if (!v || !query || k <= 0) return coa_strdup("[]");
    if (w_vec < 0) w_vec = 0;
    if (w_vec > 1) w_vec = 1;
    coa_mutex_lock(&v->mtx);
    if (v->count == 0) { coa_mutex_unlock(&v->mtx); return coa_strdup("[]"); }
    float *scores = (float *)malloc(v->count * sizeof(float));
    if (!scores) { coa_mutex_unlock(&v->mtx); return coa_strdup("[]"); }
    score_all(v, query, w_vec, scores);
    char *out = topk_json(v, scores, k);
    free(scores);
    coa_mutex_unlock(&v->mtx);
    return out;
}

char *coa_vectorstore_nearest_multi(coa_vectorstore *v, const char *const *queries,
                                   int nq, int k) {
    if (!v || !queries || nq <= 0 || k <= 0) return coa_strdup("[]");
    coa_mutex_lock(&v->mtx);
    if (v->count == 0) { coa_mutex_unlock(&v->mtx); return coa_strdup("[]"); }
    float *best = (float *)calloc(v->count, sizeof(float));
    if (!best) { coa_mutex_unlock(&v->mtx); return coa_strdup("[]"); }
    for (int q = 0; q < nq; q++) {
        if (!queries[q] || !*queries[q]) continue;
        float *scores = (float *)malloc(v->count * sizeof(float));
        if (!scores) continue;
        score_all(v, queries[q], 1.0f, scores);
        for (size_t i = 0; i < v->count; i++)
            if (scores[i] > best[i]) best[i] = scores[i];
        free(scores);
    }
    char *out = topk_json(v, best, k);
    free(best);
    coa_mutex_unlock(&v->mtx);
    return out;
}
