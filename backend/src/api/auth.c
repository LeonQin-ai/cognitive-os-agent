/* auth.c — API key / bearer-token authentication (constant-time compare). */
#include "api/auth.h"
#include "infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#endif

struct auth {
    char **keys;
    size_t count;
    size_t cap;
};

auth *auth_new(void) {
    return (auth *)calloc(1, sizeof(auth));
}

void auth_free(auth *a) {
    if (!a)
        return;
    for (size_t i = 0; i < a->count; i++)
        free(a->keys[i]);
    free(a->keys);
    free(a);
}

void auth_add_key(auth *a, const char *key) {
    if (!a || !key || !*key)
        return;
    if (a->count == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 4;
        char **nk = (char **)realloc(a->keys, cap * sizeof(char *));
        if (!nk)
            return;
        a->keys = nk;
        a->cap = cap;
    }

    a->keys[a->count++] = xstrdup(key);
}

int auth_count(auth *a) {
    return a ? (int)a->count : 0;
}

/* Constant-time string equality: scans the full length of both inputs and
 * accumulates differences so the runtime does not depend on matching length. */
static int ct_equal(const char *a, const char *b) {
    int diff;
    size_t n;

    size_t la = strlen(a), lb = strlen(b);
    diff = (int)(la ^ lb);
    n = la > lb ? la : lb;
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (int)(ca ^ cb);
    }

    return diff == 0;
}

int auth_check(auth *a, const char *token) {
    if (!a || !token)
        return 0;
    for (size_t i = 0; i < a->count; i++)
        if (ct_equal(a->keys[i], token))
            return 1;
    return 0;
}

/* Portable ASCII case-insensitive equality for a short prefix. */
static int prefix_ieq(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }

    return 1;
}

int auth_check_header(auth *a, const char *authorization) {
    const char *tok = authorization;

    if (!a || !authorization)
        return 0;
    if (prefix_ieq(authorization, "bearer "))
        tok = authorization + 7;
    while (*tok == ' ' || *tok == '\t')
        tok++;
    if (!*tok)
        return 0;
    return auth_check(a, tok);
}

void auth_generate_token(char *out, size_t bytes) {
    static const char hexc[] = "0123456789abcdef";
    unsigned char *random;
    int ok = 0;

    if (!out || bytes == 0)
        return;
    random = malloc(bytes);
    if (!random) { out[0] = '\0'; return; }
#if defined(_WIN32)
    ok = BCryptGenRandom(NULL, random, (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { ok = fread(random, 1, bytes, f) == bytes; fclose(f); }
#endif
    if (!ok) { free(random); out[0] = '\0'; return; }

    for (size_t i = 0; i < bytes; i++) {
        out[i * 2] = hexc[random[i] >> 4];
        out[i * 2 + 1] = hexc[random[i] & 0xF];
    }
    free(random);
    out[bytes * 2] = '\0';
}
