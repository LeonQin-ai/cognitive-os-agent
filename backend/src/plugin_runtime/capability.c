/* capability.c — capability tokens. */
#include "cognitive-os-agent/plugin_runtime/capability.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct capability {
    mutex_t mtx;
    char **caps;
    size_t count, cap;
};

capability *capability_new(void) {
    capability *c = (capability *)calloc(1, sizeof(capability));
    if (!c)
        return NULL;
    mutex_init(&c->mtx);
    return c;
}

void capability_free(capability *c) {
    if (!c)
        return;
    mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++)
        free(c->caps[i]);
    free(c->caps);
    c->caps = NULL;
    c->count = c->cap = 0;
    mutex_unlock(&c->mtx);
    mutex_destroy(&c->mtx);
    free(c);
}

static int find_cap(capability *c, const char *cap) {
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->caps[i], cap) == 0)
            return (int)i;
    return -1;
}

int capability_grant(capability *c, const char *cap) {
    if (!c || !cap || !*cap)
        return -1;
    mutex_lock(&c->mtx);
    if (find_cap(c, cap) >= 0) {
        mutex_unlock(&c->mtx);
        return -1;
    }

    if (c->count == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 8;
        char **nc = (char **)realloc(c->caps, ncap * sizeof(char *));
        if (!nc) {
            mutex_unlock(&c->mtx);
            return -1;
        }
        c->caps = nc;
        c->cap = ncap;
    }

    c->caps[c->count++] = xstrdup(cap);
    mutex_unlock(&c->mtx);
    return 0;
}

int capability_revoke(capability *c, const char *cap) {
    int i;

    if (!c || !cap)
        return 0;
    mutex_lock(&c->mtx);
    i = find_cap(c, cap);
    if (i < 0) {
        mutex_unlock(&c->mtx);
        return 0;
    }

    free(c->caps[i]);
    if (c->count - i - 1 > 0)
        memmove(&c->caps[i], &c->caps[i + 1], (c->count - i - 1) * sizeof(char *));
    c->count--;
    mutex_unlock(&c->mtx);
    return 1;
}

int capability_has(capability *c, const char *cap) {
    int r;

    if (!c || !cap)
        return 0;
    mutex_lock(&c->mtx);
    r = find_cap(c, cap) >= 0;
    mutex_unlock(&c->mtx);
    return r;
}

int capability_count(capability *c) {
    int n;

    if (!c)
        return 0;
    mutex_lock(&c->mtx);
    n = (int)c->count;
    mutex_unlock(&c->mtx);
    return n;
}

/* prefix wildcard: "fs.*" matches "fs.read", "net.*" matches "net" and "net.http". */
static int wild_match(const char *pat, const char *s) {
    const char *star = strchr(pat, '*');
    size_t plen;
    /* drop a trailing '.' so "net.*" also matches a bare "net" capability */
    size_t pfix;

    if (!star)
        return strcmp(pat, s) == 0;
    plen = (size_t)(star - pat);
    /* drop a trailing '.' so "net.*" also matches a bare "net" capability */
    pfix = plen;
    if (pfix > 0 && pat[pfix - 1] == '.')
        pfix--;
    if (strlen(s) < pfix)
        return 0;
    return strncmp(pat, s, pfix) == 0;
}

int capability_match(capability *c, const char *pattern) {
    int r = 0;

    if (!c || !pattern)
        return 0;
    mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->count; i++)
        if (wild_match(pattern, c->caps[i])) {
            r = 1;
            break;
        }

    mutex_unlock(&c->mtx);
    return r;
}

char *capability_json(capability *c) {
    cJSON *arr;
    char *s;

    if (!c)
        return xstrdup("[]");
    mutex_lock(&c->mtx);
    arr = cJSON_CreateArray();
    if (arr)
        for (size_t i = 0; i < c->count; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(c->caps[i]));
    mutex_unlock(&c->mtx);
    s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr)
        cJSON_Delete(arr);
    return s ? s : xstrdup("[]");
}
