/* capability.c — capability tokens. */
#include "cognitive-os-agent/plugin_runtime/capability.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct coa_capability {
    coa_mutex mtx;
    char **caps;
    size_t count, cap;
};

coa_capability *coa_capability_new(void) {
    coa_capability *c = (coa_capability *)calloc(1, sizeof(coa_capability));
    if (!c) return NULL;
    coa_mutex_init(&c->mtx);
    return c;
}

void coa_capability_free(coa_capability *c) {
    if (!c) return;
    coa_mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++) free(c->caps[i]);
    free(c->caps);
    c->caps = NULL;
    c->count = c->cap = 0;
    coa_mutex_unlock(&c->mtx);
    coa_mutex_destroy(&c->mtx);
    free(c);
}

static int find_cap(coa_capability *c, const char *cap) {
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->caps[i], cap) == 0) return (int)i;
    return -1;
}

int coa_capability_grant(coa_capability *c, const char *cap) {
    if (!c || !cap || !*cap) return -1;
    coa_mutex_lock(&c->mtx);
    if (find_cap(c, cap) >= 0) { coa_mutex_unlock(&c->mtx); return -1; }
    if (c->count == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 8;
        char **nc = (char **)realloc(c->caps, ncap * sizeof(char *));
        if (!nc) { coa_mutex_unlock(&c->mtx); return -1; }
        c->caps = nc;
        c->cap = ncap;
    }
    c->caps[c->count++] = coa_strdup(cap);
    coa_mutex_unlock(&c->mtx);
    return 0;
}

int coa_capability_revoke(coa_capability *c, const char *cap) {
    if (!c || !cap) return 0;
    coa_mutex_lock(&c->mtx);
    int i = find_cap(c, cap);
    if (i < 0) { coa_mutex_unlock(&c->mtx); return 0; }
    free(c->caps[i]);
    if (c->count - i - 1 > 0)
        memmove(&c->caps[i], &c->caps[i + 1], (c->count - i - 1) * sizeof(char *));
    c->count--;
    coa_mutex_unlock(&c->mtx);
    return 1;
}

int coa_capability_has(coa_capability *c, const char *cap) {
    if (!c || !cap) return 0;
    coa_mutex_lock(&c->mtx);
    int r = find_cap(c, cap) >= 0;
    coa_mutex_unlock(&c->mtx);
    return r;
}

int coa_capability_count(coa_capability *c) {
    if (!c) return 0;
    coa_mutex_lock(&c->mtx);
    int n = (int)c->count;
    coa_mutex_unlock(&c->mtx);
    return n;
}

/* prefix wildcard: "fs.*" matches "fs.read", "net.*" matches "net" and "net.http". */
static int wild_match(const char *pat, const char *s) {
    const char *star = strchr(pat, '*');
    if (!star) return strcmp(pat, s) == 0;
    size_t plen = (size_t)(star - pat);
    /* drop a trailing '.' so "net.*" also matches a bare "net" capability */
    size_t pfix = plen;
    if (pfix > 0 && pat[pfix - 1] == '.') pfix--;
    if (strlen(s) < pfix) return 0;
    return strncmp(pat, s, pfix) == 0;
}

int coa_capability_match(coa_capability *c, const char *pattern) {
    if (!c || !pattern) return 0;
    coa_mutex_lock(&c->mtx);
    int r = 0;
    for (size_t i = 0; i < c->count; i++)
        if (wild_match(pattern, c->caps[i])) { r = 1; break; }
    coa_mutex_unlock(&c->mtx);
    return r;
}

char *coa_capability_json(coa_capability *c) {
    if (!c) return coa_strdup("[]");
    coa_mutex_lock(&c->mtx);
    cJSON *arr = cJSON_CreateArray();
    if (arr)
        for (size_t i = 0; i < c->count; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(c->caps[i]));
    coa_mutex_unlock(&c->mtx);
    char *s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr) cJSON_Delete(arr);
    return s ? s : coa_strdup("[]");
}
