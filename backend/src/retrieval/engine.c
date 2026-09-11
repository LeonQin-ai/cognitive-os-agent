#include "cognitive-os-agent/retrieval/engine.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "cJSON.h"

typedef struct occ {
    char *file;
    int line;
} occ;

typedef struct term {
    char *word;
    occ *occs;
    size_t count, cap;
} term;

struct ret_index {
    term *terms;
    size_t count, cap;
};

ret_index *index_new(void) {
    return calloc(1, sizeof(ret_index));
}

void index_free(ret_index *idx) {
    if (!idx)
        return;
    for (size_t i = 0; i < idx->count; i++) {
        free(idx->terms[i].word);
        for (size_t j = 0; j < idx->terms[i].count; j++)
            free(idx->terms[i].occs[j].file);
        free(idx->terms[i].occs);
    }
    free(idx->terms);
    free(idx);
}

static term *find_term(ret_index *idx, const char *word, size_t wlen) {
    for (size_t i = 0; i < idx->count; i++) {
        if (strlen(idx->terms[i].word) == wlen && strncmp(idx->terms[i].word, word, wlen) == 0)
            return &idx->terms[i];
    }
    return NULL;
}

static term *get_or_add(ret_index *idx, const char *word, size_t wlen) {
    term *t = find_term(idx, word, wlen);
    if (t)
        return t;
    if (idx->count == idx->cap) {
        size_t cap = idx->cap ? idx->cap * 2 : 256;
        idx->terms = realloc(idx->terms, cap * sizeof(term));
        idx->cap = cap;
    }
    t = &idx->terms[idx->count++];
    memset(t, 0, sizeof(*t));
    t->word = malloc(wlen + 1);
    memcpy(t->word, word, wlen);
    t->word[wlen] = '\0';
    return t;
}

static void add_occ(term *t, const char *file, int line) {
    if (t->count > 0 && strcmp(t->occs[t->count - 1].file, file) == 0 && t->occs[t->count - 1].line == line)
        return; /* dedupe */
    if (t->count == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 8;
        t->occs = realloc(t->occs, cap * sizeof(occ));
        t->cap = cap;
    }
    t->occs[t->count].file = xstrdup(file);
    t->occs[t->count].line = line;
    t->count++;
}

static int is_word_char(int c) {
    return isalnum(c) || c == '_';
}

int index_add_file(ret_index *idx, const char *path, const char *content) {
    const char *p = content;
    int line = 1;
    char word[128];
    while (*p) {
        if (*p == '\n') {
            line++;
            p++;
            continue;
        }
        if (!is_word_char((unsigned char)*p)) {
            p++;
            continue;
        }
        size_t n = 0;
        while (*p && is_word_char((unsigned char)*p) && n + 1 < sizeof(word))
            word[n++] = *p++;
        word[n] = '\0';
        if (n >= 2) {
            term *t = get_or_add(idx, word, n);
            add_occ(t, path, line);
        }
    }
    return 0;
}

char *index_search(ret_index *idx, const char *query, int limit) {
    /* tokenize query with the same rule as indexing (alnum + underscore) */
    const char *tokens[32];
    int ntok = 0;
    const char *p = query;
    while (*p && ntok < 32) {
        while (*p && !is_word_char((unsigned char)*p))
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && is_word_char((unsigned char)*p))
            p++;
        if ((size_t)(p - s) >= 2)
            tokens[ntok++] = s;
    }

    cJSON *arr = cJSON_CreateArray();
    for (int t = 0; t < ntok; t++) {
        size_t wlen = strlen(tokens[t]);
        term *tm = find_term(idx, tokens[t], wlen);
        if (!tm)
            continue;
        for (size_t i = 0; i < tm->count; i++) {
            if (limit > 0 && cJSON_GetArraySize(arr) >= limit)
                break;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "term", tm->word);
            cJSON_AddStringToObject(o, "file", tm->occs[i].file);
            cJSON_AddNumberToObject(o, "line", tm->occs[i].line);
            cJSON_AddItemToArray(arr, o);
        }
    }
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out ? out : xstrdup("[]");
}
