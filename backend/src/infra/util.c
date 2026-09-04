#include "cagent/infra/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---------- dynamic string buffer ---------- */
void ca_strbuf_init(ca_strbuf *sb) {
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void ca_strbuf_free(ca_strbuf *sb) {
    free(sb->buf);
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static void strbuf_grow(ca_strbuf *sb, size_t need) {
    if (sb->cap >= need) return;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < need) cap *= 2;
    char *nb = realloc(sb->buf, cap);
    if (!nb) { fprintf(stderr, "ca_strbuf: out of memory\n"); exit(1); }
    sb->buf = nb;
    sb->cap = cap;
}

void ca_strbuf_append_n(ca_strbuf *sb, const char *s, size_t n) {
    strbuf_grow(sb, sb->len + n + 1);
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

void ca_strbuf_append(ca_strbuf *sb, const char *s) {
    ca_strbuf_append_n(sb, s, strlen(s));
}

void ca_strbuf_appendf(ca_strbuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    strbuf_grow(sb, sb->len + (size_t)n + 1);
    vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

char *ca_strbuf_detach(ca_strbuf *sb) {
    char *r = sb->buf ? sb->buf : ca_strdup("");
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
    return r;
}

/* ---------- string map ---------- */
void ca_strmap_set(ca_strmap *m, const char *key, const char *val) {
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->items[i].key, key) == 0) {
            free(m->items[i].val);
            m->items[i].val = val ? ca_strdup(val) : NULL;
            return;
        }
    }
    if (m->count == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 8;
        m->items = realloc(m->items, cap * sizeof(ca_kv));
        if (!m->items) { fprintf(stderr, "ca_strmap: oom\n"); exit(1); }
        m->cap = cap;
    }
    m->items[m->count].key = ca_strdup(key);
    m->items[m->count].val = val ? ca_strdup(val) : NULL;
    m->count++;
}

const char *ca_strmap_get(const ca_strmap *m, const char *key) {
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->items[i].key, key) == 0) return m->items[i].val;
    }
    return NULL;
}

void ca_strmap_free(ca_strmap *m) {
    for (size_t i = 0; i < m->count; i++) {
        free(m->items[i].key);
        free(m->items[i].val);
    }
    free(m->items);
    memset(m, 0, sizeof(*m));
}

/* ---------- misc ---------- */
char *ca_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (!r) return NULL;
    memcpy(r, s, n);
    return r;
}

void ca_path_join(char *out, size_t n, const char *a, const char *b) {
    /* Build in a temp buffer first so `out` may safely alias `a` (snprintf
     * with overlapping src/dst is undefined behavior). */
    char tmp[2048];
    const char sep =
#if defined(_WIN32)
        '\\';
#else
        '/';
#endif
    if (!a || !*a) { snprintf(tmp, sizeof(tmp), "%s", b ? b : ""); }
    else {
        size_t la = strlen(a);
        if (la > 0 && a[la - 1] != '/' && a[la - 1] != '\\')
            snprintf(tmp, sizeof(tmp), "%s%c%s", a, sep, b ? b : "");
        else
            snprintf(tmp, sizeof(tmp), "%s%s", a, b ? b : "");
    }
    snprintf(out, n, "%s", tmp);
}

void ca_path_resolve(char *out, size_t n, const char *workspace, const char *path) {
    if (!path || !*path) { snprintf(out, n, "%s", workspace ? workspace : ""); return; }
    if (path[0] == '/' || path[0] == '\\') { snprintf(out, n, "%s", path); return; }
#if defined(_WIN32)
    if (path[1] == ':' && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) {
        snprintf(out, n, "%s", path);
        return;
    }
#endif
    if (workspace && *workspace) ca_path_join(out, n, workspace, path);
    else snprintf(out, n, "%s", path);
}

/* ---------- hashing ---------- */
uint64_t ca_hash64(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

void ca_hash_hex(char out[17], uint64_t h) {
    static const char hexc[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        out[i] = hexc[h & 0xF];
        h >>= 4;
    }
    out[16] = '\0';
}

/* ---------- UTF-8 validation / sanitization ---------- */
/* Returns the number of bytes in the UTF-8 sequence starting at byte b,
 * or 0 if b cannot start a valid sequence of the expected length. */
static int utf8_seq_len(unsigned char b) {
    if (b < 0x80) return 1;          /* ASCII */
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 0;                        /* continuation byte or invalid lead */
}

int ca_str_utf8_valid_n(const char *s, long long n) {
    if (!s) return 1;
    size_t len = (n < 0) ? strlen(s) : (size_t)n;
    size_t i = 0;
    while (i < len) {
        int l = utf8_seq_len((unsigned char)s[i]);
        if (l == 0 || i + (size_t)l > len) return 0;
        for (int k = 1; k < l; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
        /* reject overlong / surrogate / > U+10FFFF encodings */
        unsigned int cp = 0;
        if (l == 1) cp = (unsigned char)s[i];
        else if (l == 2) cp = ((unsigned char)s[i] & 0x1F) << 6 | ((unsigned char)s[i+1] & 0x3F);
        else if (l == 3) cp = ((unsigned char)s[i] & 0x0F) << 12 | ((unsigned char)s[i+1] & 0x3F) << 6 | ((unsigned char)s[i+2] & 0x3F);
        else cp = ((unsigned char)s[i] & 0x07) << 18 | ((unsigned char)s[i+1] & 0x3F) << 12 | ((unsigned char)s[i+2] & 0x3F) << 6 | ((unsigned char)s[i+3] & 0x3F);
        if ((l == 2 && cp < 0x80) || (l == 3 && cp < 0x800) || (l == 4 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return 0;
        i += (size_t)l;
    }
    return 1;
}

char *ca_str_utf8_sanitize(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    while (i < len) {
        int l = utf8_seq_len((unsigned char)s[i]);
        int ok = (l > 0 && i + (size_t)l <= len);
        if (ok) {
            for (int k = 1; k < l; k++)
                if (((unsigned char)s[i + k] & 0xC0) != 0x80) { ok = 0; break; }
        }
        if (ok) {
            unsigned int cp = 0;
            if (l == 1) cp = (unsigned char)s[i];
            else if (l == 2) cp = ((unsigned char)s[i] & 0x1F) << 6 | ((unsigned char)s[i+1] & 0x3F);
            else if (l == 3) cp = ((unsigned char)s[i] & 0x0F) << 12 | ((unsigned char)s[i+1] & 0x3F) << 6 | ((unsigned char)s[i+2] & 0x3F);
            else cp = ((unsigned char)s[i] & 0x07) << 18 | ((unsigned char)s[i+1] & 0x3F) << 12 | ((unsigned char)s[i+2] & 0x3F) << 6 | ((unsigned char)s[i+3] & 0x3F);
            if ((l == 2 && cp < 0x80) || (l == 3 && cp < 0x800) || (l == 4 && cp < 0x10000) ||
                (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) ok = 0;
        }
        if (ok) {
            memcpy(out + o, s + i, (size_t)l);
            o += (size_t)l;
            i += (size_t)l;
        } else {
            out[o++] = '?';
            i += (l > 0) ? 1 : 1;   /* skip the bad lead byte; continuations get re-checked */
        }
    }
    out[o] = '\0';
    return out;
}
