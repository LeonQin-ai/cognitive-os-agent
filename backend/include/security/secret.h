/* secret.h — Secret Security Plane (SECRET_SECURITY_DDD_V1.0), core slice.
 *
 * Deterministic detection + redaction at the LLM boundary (§7, §8, §13) with
 * audit and compatibility modes (§5). Phase 0 semantics: never blocks in the
 * default "passthrough" mode — detection records audit events and redacts
 * egress; "strict" additionally rejects LLM input carrying high-confidence
 * secrets. Detection is cheap and never invokes the LLM (§1.5). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- detection ----------------------------------------------------------- */

typedef enum {
    SECRET_SEV_NONE = 0,
    SECRET_SEV_LOW,    /* weak/ambiguous signal — allowed, not redacted */
    SECRET_SEV_MEDIUM, /* likely secret — redact + audit */
    SECRET_SEV_HIGH    /* high-confidence credential — redact + audit;
                          blocks LLM input in strict mode */
} secret_severity_t;

typedef struct secret_match {
    size_t start, end; /* byte offsets [start, end) into the scanned text */
    secret_severity_t severity;
    char kind[24];     /* "pem_key","bearer","jwt","aws_key","api_key",
                          "kv_credential","basic_auth","entropy" */
} secret_match;

/* Scan text for secret-like material (pattern detector first, then entropy —
 * §7.2). Returns the match count (>= 0); *out receives a malloc'd array the
 * caller frees with secret_matches_free (NULL when count == 0). */
int secret_scan_text(const char *text, size_t len, secret_match **out);
void secret_matches_free(secret_match *m);

/* Redact every MEDIUM/HIGH match, replacing it with `replacement`
 * (NULL = "[REDACTED:secret]"). LOW matches pass through. Returns a malloc'd
 * copy of the text (caller frees); NULL on allocation failure. */
char *secret_redact_text(const char *text, size_t len, const char *replacement);

/* --- compatibility mode (§5) --------------------------------------------- */

/* "passthrough" (default): audit + redact egress, never block.
 * "strict": additionally reject LLM input with HIGH-confidence secrets.
 * Returns 0 ok, -1 unknown mode. */
int secret_set_mode(const char *mode);
const char *secret_mode(void);

/* --- sinks (owned by the module once bound) -------------------------------- */

/* Open (and own) the security audit sink at `path` — one JSONL line per
 * detection burst. Passing NULL closes any bound sink. */
void secret_audit_open(const char *path);
/* Bind a `metrics *` from infra/metrics.h (borrowed; caller owns it). */
void secret_metrics_bind(void *metrics);
/* Release the owned audit sink (called from runtime shutdown). */
void secret_audit_close(void);

/* --- LLM boundary guard (§13) --------------------------------------------- */

/* Scan request message contents before they leave the trust boundary.
 * Returns 0 ok, -1 blocked (strict mode + HIGH-confidence secret; the caller
 * must not send the request). Audit fires on every detection. */
int secret_guard_llm_input(const char *const *contents, size_t n);

/* Egress filter for a full LLM response: redacts matches in place (the buffer
 * is freed and replaced when anything matched). */
void secret_guard_llm_output(char **content);

/* Streaming egress filter. Wrap the consumer callback:
 *   - create with secret_stream_guard_new(inner_cb, inner_ud);
 *   - pass secret_stream_guard_cb / the guard to the provider;
 *   - free with secret_stream_guard_free (flushes the carry tail).
 * A rolling tail carry catches matches that span delta boundaries. */
typedef void (*secret_stream_cb)(const char *delta, void *ud);
void *secret_stream_guard_new(secret_stream_cb inner, void *ud);
void secret_stream_guard_cb(const char *delta, void *guard);
void secret_stream_guard_free(void *guard);

/* --- observability (§22) --------------------------------------------------- */

/* JSON object of cumulative counters: scans, matches by severity, redactions,
 * blocked inputs. Caller frees. */
char *secret_stats_json(void);

/* Test-only: zero all counters. */
void secret_stats_reset(void);

#ifdef __cplusplus
}
#endif
