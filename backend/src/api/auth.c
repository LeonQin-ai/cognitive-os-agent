/* auth.c — API key / bearer-token authentication (constant-time compare). */
#include "cagent/api/auth.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

struct ca_auth {
    char **keys;
    size_t count;
    size_t cap;
};

ca_auth *ca_auth_new(void) {
    return (ca_auth *)calloc(1, sizeof(ca_auth));
}

void ca_auth_free(ca_auth *a) {
    if (!a) return;
    for (size_t i = 0; i < a->count; i++) free(a->keys[i]);
    free(a->keys);
    free(a);
}

void ca_auth_add_key(ca_auth *a, const char *key) {
    if (!a || !key || !*key) return;
    if (a->count == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 4;
        char **nk = (char **)realloc(a->keys, cap * sizeof(char *));
        if (!nk) return;
        a->keys = nk;
        a->cap = cap;
    }
    a->keys[a->count++] = ca_strdup(key);
}

int ca_auth_count(ca_auth *a) {
    return a ? (int)a->count : 0;
}

/* Constant-time string equality: scans the full length of both inputs and
 * accumulates differences so the runtime does not depend on matching length. */
static int ct_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    int diff = (int)(la ^ lb);
    size_t n = la > lb ? la : lb;
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (int)(ca ^ cb);
    }
    return diff == 0;
}

int ca_auth_check(ca_auth *a, const char *token) {
    if (!a || !token) return 0;
    for (size_t i = 0; i < a->count; i++)
        if (ct_equal(a->keys[i], token)) return 1;
    return 0;
}

/* Portable ASCII case-insensitive equality for a short prefix. */
static int prefix_ieq(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

int ca_auth_check_header(ca_auth *a, const char *authorization) {
    if (!a || !authorization) return 0;
    const char *tok = authorization;
    if (prefix_ieq(authorization, "bearer ")) tok = authorization + 7;
    while (*tok == ' ' || *tok == '\t') tok++;
    if (!*tok) return 0;
    return ca_auth_check(a, tok);
}

/* Small xorshift64 PRNG seeded once per process from wall clock + stack
 * address. Not cryptographically strong; adequate for demo session tokens. */
static unsigned long long xorshift64(unsigned long long *s) {
    unsigned long long x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

void ca_auth_generate_token(char *out, size_t bytes) {
    if (!out || bytes == 0) return;
    static unsigned long long state;
    static int seeded = 0;
    if (!seeded) {
        unsigned long long a = (unsigned long long)time(NULL);
        unsigned long long b = (unsigned long long)(uintptr_t)&state;
        state = (a << 32) ^ b ^ 0x9E3779B97F4A7C15ULL;
        if (state == 0) state = 1;
        seeded = 1;
    }
    static const char hexc[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; i++) {
        unsigned long long r = xorshift64(&state);
        out[i * 2]     = hexc[(r >> 4) & 0xF];
        out[i * 2 + 1] = hexc[r & 0xF];
    }
    out[bytes * 2] = '\0';
}
