/* util.h — shared small utilities: dynamic strings, string map, hashing */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- dynamic string buffer ---------- */
typedef struct ca_strbuf {
    char *buf;
    size_t len;
    size_t cap;
} ca_strbuf;

void ca_strbuf_init(ca_strbuf *sb);
void ca_strbuf_free(ca_strbuf *sb);
void ca_strbuf_append(ca_strbuf *sb, const char *s);
void ca_strbuf_append_n(ca_strbuf *sb, const char *s, size_t n);
void ca_strbuf_appendf(ca_strbuf *sb, const char *fmt, ...);
/* Release the buffer to the caller (caller must free()); resets sb. */
char *ca_strbuf_detach(ca_strbuf *sb);

/* ---------- string map (owned char* -> char*) ---------- */
typedef struct ca_kv {
    char *key;
    char *val;
} ca_kv;

typedef struct ca_strmap {
    ca_kv *items;
    size_t count;
    size_t cap;
} ca_strmap;

void ca_strmap_set(ca_strmap *m, const char *key, const char *val);
/* Returns NULL if absent. Do not free the returned pointer. */
const char *ca_strmap_get(const ca_strmap *m, const char *key);
void ca_strmap_free(ca_strmap *m);

/* ---------- misc ---------- */
char *ca_strdup(const char *s);
/* Join two path segments with the platform separator into out[0..n). */
void ca_path_join(char *out, size_t n, const char *a, const char *b);
/* Resolve a (possibly relative) path against a workspace base dir into out[0..n).
 * Absolute paths and Windows drive paths pass through unchanged. */
void ca_path_resolve(char *out, size_t n, const char *workspace, const char *path);

/* FNV-1a 64-bit hash (non-cryptographic; used for content addressing & indexes). */
uint64_t ca_hash64(const void *data, size_t len);
/* Hex string (16 chars) of a 64-bit hash; writes into out[0..16], no terminator needed. */
void ca_hash_hex(char out[17], uint64_t h);

#ifdef __cplusplus
}
#endif
