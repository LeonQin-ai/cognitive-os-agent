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

#define AUTH_TOKEN_BYTES 512

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
    if (!a || !key || !*key || strlen(key) >= AUTH_TOKEN_BYTES)
        return;
    if (a->count == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 4;
        char **nk = (char **)realloc(a->keys, cap * sizeof(char *));
        if (!nk)
            return;
        a->keys = nk;
        a->cap = cap;
    }

    char *padded = (char *)calloc(AUTH_TOKEN_BYTES, 1);
    if (!padded) return;
    memcpy(padded, key, strlen(key));
    a->keys[a->count++] = padded;
}

int auth_count(auth *a) {
    return a ? (int)a->count : 0;
}

/* Fixed-width padded comparison: the secret length never controls a loop. */
int auth_check(auth *a, const char *token) {
    if (!a || !token) return 0;
    unsigned char candidate[AUTH_TOKEN_BYTES] = {0};
    size_t n=0;
    while (n < sizeof(candidate) && token[n]) { candidate[n]=(unsigned char)token[n]; n++; }
    if (n == sizeof(candidate)) return 0;
    int matched=0;
    for (size_t k=0; k<a->count; k++) {
        volatile unsigned diff=0;
        for (size_t i=0; i<sizeof(candidate); i++) diff |= candidate[i] ^ (unsigned char)a->keys[k][i];
        matched |= diff==0;
    }
    return matched;
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
