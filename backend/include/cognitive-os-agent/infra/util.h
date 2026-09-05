/* util.h — shared small utilities: dynamic strings, string map, hashing */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- dynamic string buffer ---------- */
typedef struct coa_strbuf {
    char *buf;
    size_t len;
    size_t cap;
} coa_strbuf;

void coa_strbuf_init(coa_strbuf *sb);
void coa_strbuf_free(coa_strbuf *sb);
void coa_strbuf_append(coa_strbuf *sb, const char *s);
void coa_strbuf_append_n(coa_strbuf *sb, const char *s, size_t n);
void coa_strbuf_appendf(coa_strbuf *sb, const char *fmt, ...);
/* Release the buffer to the caller (caller must free()); resets sb. */
char *coa_strbuf_detach(coa_strbuf *sb);

/* ---------- string map (owned char* -> char*) ---------- */
typedef struct coa_kv {
    char *key;
    char *val;
} coa_kv;

typedef struct coa_strmap {
    coa_kv *items;
    size_t count;
    size_t cap;
} coa_strmap;

void coa_strmap_set(coa_strmap *m, const char *key, const char *val);
/* Returns NULL if absent. Do not free the returned pointer. */
const char *coa_strmap_get(const coa_strmap *m, const char *key);
void coa_strmap_free(coa_strmap *m);

/* ---------- misc ---------- */
char *coa_strdup(const char *s);
/* Returns 1 when `s` (up to n bytes, or NUL-terminated when n<0) is valid UTF-8. */
int coa_str_utf8_valid_n(const char *s, long long n);
/* Returns a malloc'd copy of `s` guaranteed to be valid UTF-8: any invalid
 * byte sequence is replaced with '?'. Returns NULL only on OOM. Text coming
 * from external processes (shell output on Chinese Windows is GBK) or from
 * untrusted clients must pass through this before entering LLM prompts —
 * providers reject invalid UTF-8 with a hard 400 that poisons every later
 * turn in the session. */
char *coa_str_utf8_sanitize(const char *s);
/* Join two path segments with the platform separator into out[0..n). */
void coa_path_join(char *out, size_t n, const char *a, const char *b);
/* Resolve a (possibly relative) path against a workspace base dir into out[0..n).
 * Absolute paths and Windows drive paths pass through unchanged. */
void coa_path_resolve(char *out, size_t n, const char *workspace, const char *path);

/* FNV-1a 64-bit hash (non-cryptographic; used for content addressing & indexes). */
uint64_t coa_hash64(const void *data, size_t len);
/* Hex string (16 chars) of a 64-bit hash; writes into out[0..16], no terminator needed. */
void coa_hash_hex(char out[17], uint64_t h);

#ifdef __cplusplus
}
#endif
