/* vector.c — in-memory vector store with cosine nearest-neighbor recall. */
#include "cagent/memory/vector.h"
#include "cagent/retrieval/embedding.h"
#include "cagent/os/os_thread.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

typedef struct vec_entry {
    char *id;
    char *text;
    char *meta;
    float vec[CA_EMBED_DIM];
} vec_entry;

struct ca_vectorstore {
    ca_mutex mtx;
    vec_entry *items;
    size_t count;
    size_t cap;
};

ca_vectorstore *ca_vectorstore_new(void) {
    ca_vectorstore *v = (ca_vectorstore *)calloc(1, sizeof(*v));
    if (!v) return NULL;
    ca_mutex_init(&v->mtx);
    return v;
}

static void entry_clear(vec_entry *e) {
    free(e->id);
    free(e->text);
    free(e->meta);
    memset(e, 0, sizeof(*e));
}

void ca_vectorstore_free(ca_vectorstore *v) {
    if (!v) return;
    ca_mutex_lock(&v->mtx);
    for (size_t i = 0; i < v->count; i++) entry_clear(&v->items[i]);
    free(v->items);
    v->items = NULL;
    v->count = v->cap = 0;
    ca_mutex_unlock(&v->mtx);
    ca_mutex_destroy(&v->mtx);
    free(v);
}

int ca_vectorstore_add(ca_vectorstore *v, const char *id, const char *text, const char *meta) {
    if (!v || !id || !text) return -1;
    ca_mutex_lock(&v->mtx);
    /* update existing id */
    for (size_t i = 0; i < v->count; i++) {
        if (strcmp(v->items[i].id, id) == 0) {
            free(v->items[i].text);
            free(v->items[i].meta);
            v->items[i].text = ca_strdup(text);
            v->items[i].meta = ca_strdup(meta ? meta : "");
            ca_embed_text(text, v->items[i].vec);
            ca_mutex_unlock(&v->mtx);
            return 0;
        }
    }
    if (v->count == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 8;
        vec_entry *ni = (vec_entry *)realloc(v->items, cap * sizeof(vec_entry));
        if (!ni) { ca_mutex_unlock(&v->mtx); return -1; }
        v->items = ni;
        v->cap = cap;
    }
    vec_entry *e = &v->items[v->count++];
    memset(e, 0, sizeof(*e));
    e->id = ca_strdup(id);
    e->text = ca_strdup(text);
    e->meta = ca_strdup(meta ? meta : "");
    ca_embed_text(text, e->vec);
    ca_mutex_unlock(&v->mtx);
    return 0;
}

int ca_vectorstore_count(ca_vectorstore *v) {
    if (!v) return 0;
    ca_mutex_lock(&v->mtx);
    int n = (int)v->count;
    ca_mutex_unlock(&v->mtx);
    return n;
}

char *ca_vectorstore_nearest(ca_vectorstore *v, const char *query, int k) {
    if (!v) return ca_strdup("[]");
    float qvec[CA_EMBED_DIM];
    ca_embed_text(query, qvec);

    ca_mutex_lock(&v->mtx);
    /* collect top-k with a simple insertion into a small sorted list of indices */
    int *top_idx = (int *)malloc(k > 0 ? (size_t)k * sizeof(int) : sizeof(int));
    float *top_score = (float *)malloc(k > 0 ? (size_t)k * sizeof(float) : sizeof(float));
    int ntop = 0;
    if (!top_idx || !top_score) {
        free(top_idx);
        free(top_score);
        ca_mutex_unlock(&v->mtx);
        return ca_strdup("[]");
    }

    for (size_t i = 0; i < v->count; i++) {
        float s = ca_embed_cosine(qvec, v->items[i].vec, CA_EMBED_DIM);
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
    ca_mutex_unlock(&v->mtx);

    char *s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr) cJSON_Delete(arr);
    return s ? s : ca_strdup("[]");
}
