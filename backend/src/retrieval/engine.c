#include "cagent/retrieval/engine.h"
#include "cagent/infra/util.h"
#include "cagent/os/os_fs.h"

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

struct ca_index {
    term *terms;
    size_t count, cap;
};

ca_index *ca_index_new(void) { return calloc(1, sizeof(ca_index)); }

void ca_index_free(ca_index *idx) {
    if (!idx) return;
    for (size_t i = 0; i < idx->count; i++) {
        free(idx->terms[i].word);
        for (size_t j = 0; j < idx->terms[i].count; j++) free(idx->terms[i].occs[j].file);
        free(idx->terms[i].occs);
    }
    free(idx->terms);
    free(idx);
}

static term *find_term(ca_index *idx, const char *word, size_t wlen) {
    for (size_t i = 0; i < idx->count; i++) {
        if (strlen(idx->terms[i].word) == wlen && strncmp(idx->terms[i].word, word, wlen) == 0)
            return &idx->terms[i];
    }
    return NULL;
}

static term *get_or_add(ca_index *idx, const char *word, size_t wlen) {
    term *t = find_term(idx, word, wlen);
    if (t) return t;
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
    if (t->count > 0 && strcmp(t->occs[t->count - 1].file, file) == 0 &&
        t->occs[t->count - 1].line == line)
        return; /* dedupe */
    if (t->count == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 8;
        t->occs = realloc(t->occs, cap * sizeof(occ));
        t->cap = cap;
    }
    t->occs[t->count].file = ca_strdup(file);
    t->occs[t->count].line = line;
    t->count++;
}

static int is_word_char(int c) { return isalnum(c) || c == '_'; }

int ca_index_add_file(ca_index *idx, const char *path, const char *content) {
    const char *p = content;
    int line = 1;
    char word[128];
    while (*p) {
        if (*p == '\n') { line++; p++; continue; }
        if (!is_word_char((unsigned char)*p)) { p++; continue; }
        size_t n = 0;
        while (*p && is_word_char((unsigned char)*p) && n + 1 < sizeof(word)) word[n++] = *p++;
        word[n] = '\0';
        if (n >= 2) {
            term *t = get_or_add(idx, word, n);
            add_occ(t, path, line);
        }
    }
    return 0;
}

static int has_source_ext(const char *name) {
    static const char *exts[] = {".c", ".h", ".py", ".js", ".ts", ".json", ".md",
                                 ".sh", ".html", ".css", ".rs", ".go", ".java", ".c", ".cpp", ".hpp"};
    size_t len = strlen(name);
    for (size_t i = 0; i < sizeof(exts) / sizeof(char *); i++) {
        size_t el = strlen(exts[i]);
        if (len >= el && strcmp(name + len - el, exts[i]) == 0) return 1;
    }
    return 0;
}

static void scan_dir(ca_index *idx, const char *dir) {
    ca_dir_list dl;
    if (ca_fs_list_dir(dir, &dl) != 0) return;
    for (size_t i = 0; i < dl.count; i++) {
        char full[2048];
        ca_path_join(full, sizeof(full), dir, dl.items[i].name);
        if (dl.items[i].is_dir) {
            if (strcmp(dl.items[i].name, "state") == 0 || strcmp(dl.items[i].name, "build") == 0 ||
                strcmp(dl.items[i].name, "node_modules") == 0 || strcmp(dl.items[i].name, ".git") == 0)
                continue;
            scan_dir(idx, full);
        } else if (has_source_ext(dl.items[i].name)) {
            char *content = ca_fs_read_file(full);
            if (content) {
                ca_index_add_file(idx, full, content);
                free(content);
            }
        }
    }
    ca_fs_list_free(&dl);
}

int ca_index_build_dir(ca_index *idx, const char *dir) {
    if (!ca_fs_is_dir(dir)) return -1;
    scan_dir(idx, dir);
    return 0;
}

char *ca_index_search(ca_index *idx, const char *query, int limit) {
    /* tokenize query with the same rule as indexing (alnum + underscore) */
    const char *tokens[32];
    int ntok = 0;
    const char *p = query;
    while (*p && ntok < 32) {
        while (*p && !is_word_char((unsigned char)*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && is_word_char((unsigned char)*p)) p++;
        if ((size_t)(p - s) >= 2) tokens[ntok++] = s;
    }

    cJSON *arr = cJSON_CreateArray();
    for (int t = 0; t < ntok; t++) {
        size_t wlen = strlen(tokens[t]);
        term *tm = find_term(idx, tokens[t], wlen);
        if (!tm) continue;
        for (size_t i = 0; i < tm->count; i++) {
            if (limit > 0 && cJSON_GetArraySize(arr) >= limit) break;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "term", tm->word);
            cJSON_AddStringToObject(o, "file", tm->occs[i].file);
            cJSON_AddNumberToObject(o, "line", tm->occs[i].line);
            cJSON_AddItemToArray(arr, o);
        }
    }
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out ? out : ca_strdup("[]");
}
