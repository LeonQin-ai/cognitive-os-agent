/* memory.c — cognitive memory facade.
 * Composes fine-grained sub-stores:
 *  - working memory:  short-term ring buffer of recent items (inline)
 *  - long-term facts: ca_kvstore (memory/kv.h)
 *  - episodes:        ca_episodic (memory/episode.h)
 *  - vector-lite:     ca_vectorstore (memory/vector.h), mirroring working + episodes
 * Long-term facts are persisted as JSON under the state root. */
#include "cagent/memory/memory.h"
#include "cagent/memory/kv.h"
#include "cagent/memory/episode.h"
#include "cagent/memory/vector.h"
#include "cagent/infra/util.h"
#include "cagent/infra/persist.h"
#include "cagent/os/os_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "cJSON.h"

#define WORKING_CAP 64

typedef struct {
    char **items;      /* newest first */
    size_t count;
} working_mem;

struct ca_memory {
    char root[512];
    working_mem working;     /* guarded by mtx */
    ca_kvstore *facts;       /* own mutex */
    ca_episodic *episodes;   /* own mutex */
    ca_vectorstore *vectors; /* own mutex */
    size_t seq;              /* monotonic id for vector mirroring (guarded by mtx) */
    ca_mutex mtx;
};

ca_memory *ca_memory_new(const char *state_root) {
    ca_memory *m = (ca_memory *)calloc(1, sizeof(ca_memory));
    if (!m) return NULL;
    snprintf(m->root, sizeof(m->root), "%s", state_root);
    ca_mutex_init(&m->mtx);
    m->facts = ca_kvstore_new();
    m->episodes = ca_episodic_new();
    m->vectors = ca_vectorstore_new();
    if (!m->facts || !m->episodes || !m->vectors) {
        if (m->facts) ca_kvstore_free(m->facts);
        if (m->episodes) ca_episodic_free(m->episodes);
        if (m->vectors) ca_vectorstore_free(m->vectors);
        ca_mutex_destroy(&m->mtx);
        free(m);
        return NULL;
    }

    /* load persisted facts */
    char *facts_json = ca_persist_read("memory", "facts.json");
    if (facts_json) {
        cJSON *root = cJSON_Parse(facts_json);
        if (root && cJSON_IsObject(root)) {
            cJSON *it;
            cJSON_ArrayForEach(it, root)
                ca_kvstore_set(m->facts, it->string, it->valuestring ? it->valuestring : "");
        }
        if (root) cJSON_Delete(root);
        free(facts_json);
    }
    return m;
}

void ca_memory_free(ca_memory *m) {
    if (!m) return;
    for (size_t i = 0; i < m->working.count; i++) free(m->working.items[i]);
    free(m->working.items);
    ca_kvstore_free(m->facts);
    ca_episodic_free(m->episodes);
    ca_vectorstore_free(m->vectors);
    ca_mutex_destroy(&m->mtx);
    free(m);
}

void ca_memory_working_push(ca_memory *m, const char *text) {
    if (!m || !text) return;
    char id[32];
    ca_mutex_lock(&m->mtx);
    if (m->working.count == WORKING_CAP) {
        free(m->working.items[WORKING_CAP - 1]);
        m->working.count--;
    }
    char **ni = (char **)realloc(m->working.items, (m->working.count + 1) * sizeof(char *));
    if (!ni) { ca_mutex_unlock(&m->mtx); return; }
    m->working.items = ni;
    memmove(m->working.items + 1, m->working.items, m->working.count * sizeof(char *));
    m->working.items[0] = ca_strdup(text);
    m->working.count++;
    snprintf(id, sizeof(id), "w:%zu", m->seq++);
    ca_mutex_unlock(&m->mtx);

    ca_vectorstore_add(m->vectors, id, text, "working");
}

int ca_memory_working_count(ca_memory *m) {
    if (!m) return 0;
    ca_mutex_lock(&m->mtx);
    int n = (int)m->working.count;
    ca_mutex_unlock(&m->mtx);
    return n;
}

const char *ca_memory_working_at(ca_memory *m, int i) {
    if (!m || i < 0) return NULL;
    ca_mutex_lock(&m->mtx);
    const char *v = ((size_t)i < m->working.count) ? m->working.items[i] : NULL;
    ca_mutex_unlock(&m->mtx);
    return v;
}

void ca_memory_remember(ca_memory *m, const char *key, const char *value) {
    if (!m || !key || !*key) return;
    ca_kvstore_set(m->facts, key, value);
}

const char *ca_memory_recall(ca_memory *m, const char *key) {
    if (!m || !key) return NULL;
    return ca_kvstore_get(m->facts, key);
}

void ca_memory_record_experience(ca_memory *m, const char *task, const char *result) {
    if (!m || !task) return;
    char id[32];
    ca_mutex_lock(&m->mtx);
    snprintf(id, sizeof(id), "e:%zu", m->seq++);
    ca_mutex_unlock(&m->mtx);

    ca_episodic_add(m->episodes, task, result);
    ca_vectorstore_add(m->vectors, id, task, result ? result : "");
}

static void tokenize(const char *s, const char *tokens[64], int *ntok) {
    int n = 0;
    const char *p = s;
    while (*p && n < 64) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);
        if (len >= 3) tokens[n++] = start; /* store pointer; compare via ci_eq_n */
    }
    *ntok = n;
}

/* Portable case-insensitive compare of the first n bytes. */
static int ci_eq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return 1;
}

static int token_match(const char *text, const char *tok, size_t tlen) {
    const char *p = text;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        if ((size_t)(p - s) == tlen && ci_eq_n(s, tok, tlen)) return 1;
    }
    return 0;
}

char *ca_memory_search(ca_memory *m, const char *query, int limit) {
    const char *tokens[64];
    int ntok = 0;
    tokenize(query, tokens, &ntok);
    if (ntok == 0) return ca_strdup("[]");

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return ca_strdup("[]");

    /* score working memory */
    ca_mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->working.count; i++) {
        int hits = 0;
        for (int t = 0; t < ntok; t++) if (token_match(m->working.items[i], tokens[t], strlen(tokens[t]))) hits++;
        if (hits > 0) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "kind", "working");
            cJSON_AddStringToObject(o, "text", m->working.items[i]);
            cJSON_AddNumberToObject(o, "score", hits);
            cJSON_AddItemToArray(arr, o);
            if (limit > 0 && cJSON_GetArraySize(arr) >= limit) break;
        }
    }
    ca_mutex_unlock(&m->mtx);

    /* score experiences */
    if (limit <= 0 || cJSON_GetArraySize(arr) < limit) {
        int n = ca_episodic_count(m->episodes);
        for (int i = 0; i < n; i++) {
            const char *task = ca_episodic_task(m->episodes, i);
            const char *result = ca_episodic_result(m->episodes, i);
            int hits = 0;
            for (int t = 0; t < ntok; t++) if (task && token_match(task, tokens[t], strlen(tokens[t]))) hits++;
            if (hits > 0) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "kind", "experience");
                cJSON_AddStringToObject(o, "text", task ? task : "");
                cJSON_AddStringToObject(o, "result", result ? result : "");
                cJSON_AddNumberToObject(o, "score", hits);
                cJSON_AddItemToArray(arr, o);
                if (limit > 0 && cJSON_GetArraySize(arr) >= limit) break;
            }
        }
    }

    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : ca_strdup("[]");
}

char *ca_memory_retrieve(ca_memory *m, const char *query, int k) {
    if (!m || !query) return ca_strdup("[]");
    return ca_vectorstore_nearest(m->vectors, query, k);
}

void ca_memory_flush(ca_memory *m) {
    if (!m) return;
    char *s = ca_kvstore_snapshot_json(m->facts);
    if (s) {
        ca_persist_write("memory", "facts.json", s);
        free(s);
    }
}

char *ca_memory_working_json(ca_memory *m) {
    if (!m) return ca_strdup("[]");
    ca_mutex_lock(&m->mtx);
    cJSON *arr = cJSON_CreateArray();
    if (arr)
        for (size_t i = 0; i < m->working.count; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(m->working.items[i]));
    ca_mutex_unlock(&m->mtx);
    char *s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr) cJSON_Delete(arr);
    return s ? s : ca_strdup("[]");
}

char *ca_memory_longterm_json(ca_memory *m) {
    if (!m) return ca_strdup("{}");
    return ca_kvstore_snapshot_json(m->facts);
}

char *ca_memory_episodes_json(ca_memory *m) {
    if (!m) return ca_strdup("[]");
    return ca_episodic_json(m->episodes);
}
