/* secret_boundary.c — Secret Security Plane boundary guards: compatibility
 * mode (§5), audit/metrics sinks, LLM input/output guards (§13) and the
 * streaming egress filter with a rolling hold-back window.
 *
 * Streaming model: the guard holds back the last GUARD_TAIL-1 bytes of the
 * stream (they may complete a secret that continues in the next delta). Each
 * incoming delta is scanned together with the held bytes as one window; the
 * redacted window minus its trailing hold-back is then emitted. Stream end
 * (secret_stream_guard_free) flushes the held bytes. */
#include "security/secret.h"
#include "infra/audit.h"
#include "infra/metrics.h"
#include "infra/util.h"
#include "os/os_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#define GUARD_TAIL 6145 /* 1024-byte password, up to 6 JSON bytes per byte */

/* --- module state --------------------------------------------------------- */

static mutex_t g_mtx;
static _Atomic int g_mtx_init = 0;
static int g_mode_strict = 0;     /* 0 = passthrough (default), 1 = strict */
static audit *g_audit = NULL;     /* owned (secret_audit_open) */
static metrics *g_metrics = NULL; /* borrowed */

/* cumulative counters */
static long long g_scans, g_low, g_medium, g_high, g_redactions, g_blocked;

static void lock_init_once(void) {
    int state = atomic_load_explicit(&g_mtx_init, memory_order_acquire);
    if (state == 2) return;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_mtx_init, &expected, 1,
                                                memory_order_acq_rel, memory_order_acquire)) {
        mutex_init(&g_mtx);
        atomic_store_explicit(&g_mtx_init, 2, memory_order_release);
    } else {
        while (atomic_load_explicit(&g_mtx_init, memory_order_acquire) != 2) { }
    }
}

static void counter_add(int scans, int sev_low, int sev_medium, int sev_high,
                        int redacted, int blocked) {
    lock_init_once();
    mutex_lock(&g_mtx);
    g_scans += scans;
    g_low += sev_low;
    g_medium += sev_medium;
    g_high += sev_high;
    g_redactions += redacted;
    g_blocked += blocked;
    mutex_unlock(&g_mtx);
}

/* Emit one audit record per scan burst that found something. */
static void audit_scan(const char *boundary, const secret_match *ms, int n) {
    int medium = 0, high = 0, i;
    const char *kind = "entropy";
    char detail[256];

    if (!g_audit || n <= 0)
        return;
    for (i = 0; i < n; i++) {
        if (ms[i].severity == SECRET_SEV_MEDIUM)
            medium++;
        if (ms[i].severity == SECRET_SEV_HIGH) {
            high++;
            kind = ms[i].kind;
        }
    }
    snprintf(detail, sizeof detail, "{\"boundary\":\"%s\",\"high\":%d,\"medium\":%d}",
             boundary, high, medium);
    audit_log(g_audit, "secret.detect", kind, high ? "high" : "medium", detail);
    if (g_metrics)
        metrics_inc(g_metrics, high ? "secret_detect_high_total"
                                    : "secret_detect_medium_total");
}

/* Strongest severity in a match list (assumed non-empty). */
static secret_severity_t strongest(const secret_match *ms, int n) {
    secret_severity_t s = SECRET_SEV_NONE;
    int i;
    for (i = 0; i < n; i++)
        if (ms[i].severity > s)
            s = ms[i].severity;
    return s;
}

/* --- public: mode + sinks -------------------------------------------------- */

int secret_set_mode(const char *mode) {
    if (!mode || !*mode || strcmp(mode, "passthrough") == 0) {
        g_mode_strict = 0;
        return 0;
    }
    if (strcmp(mode, "strict") == 0) {
        g_mode_strict = 1;
        return 0;
    }
    return -1;
}

const char *secret_mode(void) {
    return g_mode_strict ? "strict" : "passthrough";
}

void secret_audit_open(const char *path) {
    secret_audit_close();
    if (path && *path)
        g_audit = audit_open(path);
}

void secret_audit_close(void) {
    if (g_audit) {
        audit_close(g_audit);
        g_audit = NULL;
    }
}

void secret_metrics_bind(void *m) {
    g_metrics = (metrics *)m;
}

/* --- guards ----------------------------------------------------------------- */

int secret_guard_llm_input(const char *const *contents, size_t n) {
    size_t i;
    secret_severity_t worst = SECRET_SEV_NONE;

    for (i = 0; i < n; i++) {
        secret_match *ms = NULL;
        int cnt;
        const char *c = contents && contents[i] ? contents[i] : NULL;
        if (!c || !*c)
            continue;
        cnt = secret_scan_ssh_passwords(c, strlen(c), &ms);
        if (cnt > 0) {
            secret_severity_t s = strongest(ms, cnt);
            audit_scan("llm_input", ms, cnt);
            if (s > worst)
                worst = s;
            counter_add(1, s >= SECRET_SEV_LOW, s >= SECRET_SEV_MEDIUM,
                        s >= SECRET_SEV_HIGH, 0, 0);
        }
        secret_matches_free(ms);
    }
    /* strict mode blocks HIGH-confidence ingress (§5.3); passthrough never blocks */
    if (g_mode_strict && worst >= SECRET_SEV_HIGH) {
        counter_add(0, 0, 0, 0, 0, 1);
        if (g_metrics)
            metrics_inc(g_metrics, "secret_blocked_total");
        return -1;
    }
    return 0;
}

static char *redact_untrusted_matches(const char *text, const secret_match *ms, int cnt,
                                      const char *const *trusted, size_t trusted_n) {
    strbuf out;
    size_t pos = 0;
    int changed = 0;
    (void)trusted; (void)trusted_n;

    strbuf_init(&out);
    for (int i = 0; i < cnt; i++) {
        if (ms[i].end <= ms[i].start || ms[i].start < pos)
            continue;
        strbuf_append_n(&out, text + pos, ms[i].start - pos);
        if (ms[i].severity >= SECRET_SEV_MEDIUM) {
            strbuf_append(&out, "[REDACTED:ssh_password]");
            changed = 1;
        } else {
            strbuf_append_n(&out, text + ms[i].start, ms[i].end - ms[i].start);
        }
        pos = ms[i].end;
    }
    strbuf_append(&out, text + pos);
    if (!changed) {
        strbuf_free(&out);
        return NULL;
    }
    return strbuf_detach(&out);
}

void secret_guard_llm_output_trusted(char **content, const char *const *trusted, size_t trusted_n) {
    secret_match *ms = NULL;
    int cnt;
    char *clean;

    if (!content || !*content || !**content)
        return;
    cnt = secret_scan_ssh_passwords(*content, strlen(*content), &ms);
    if (cnt < 0) { free(*content); *content=xstrdup("[SSH password filtering failed]"); return; }
    if (cnt > 0) {
        secret_severity_t s = strongest(ms, cnt);
        audit_scan("llm_output", ms, cnt);
        counter_add(1, s >= SECRET_SEV_LOW, s >= SECRET_SEV_MEDIUM,
                    s >= SECRET_SEV_HIGH, 0, 0);
        clean = redact_untrusted_matches(*content, ms, cnt, trusted, trusted_n);
        if (clean) {
            free(*content);
            *content = clean;
            counter_add(0, 0, 0, 0, 1, 0);
            if (g_metrics)
                metrics_inc(g_metrics, "secret_redactions_total");
        }
    }
    secret_matches_free(ms);
}

void secret_guard_llm_output(char **content) {
    secret_guard_llm_output_trusted(content, NULL, 0);
}

/* --- streaming egress filter -------------------------------------------------- */

typedef struct stream_guard {
    secret_stream_cb inner;
    void *ud;
    char hold[GUARD_TAIL]; /* withheld tail, N-terminated; may complete a
                            * secret with the next delta */
    size_t hold_len;
} stream_guard;

void *secret_stream_guard_new(secret_stream_cb inner, void *ud) {
    stream_guard *g = (stream_guard *)calloc(1, sizeof(stream_guard));
    if (!g)
        return NULL;
    g->inner = inner;
    g->ud = ud;
    return g;
}

/* Scan+emit one window: hold + delta. Emits the redacted window minus the
 * trailing GUARD_TAIL-1 bytes (new hold); clears the hold after a redaction
 * (the match consumed the boundary region). */
static void guard_emit_window(stream_guard *g, const char *window, size_t wlen,
                              size_t delta_len) {
    secret_match *ms = NULL;
    int cnt = secret_scan_ssh_passwords(window, wlen, &ms);
    size_t carry=secret_ssh_password_max_length();
    if (carry && carry<32) carry=32; /* short-password label context */
    if (carry) carry--;
    size_t cut = wlen > carry ? wlen-carry : 0;
    (void)delta_len;
    if (cnt < 0) { if (g->inner) g->inner("[SSH password filtering failed]",g->ud); g->hold_len=0; return; }
    /* A complete match crossing the carry boundary can be emitted masked now,
     * so no partial password is emitted and no already-masked tail is replayed. */
    for (int i=0; i<cnt; i++) if (ms[i].start<cut && ms[i].end>cut) cut=ms[i].end;
    if (cnt>0) { audit_scan("llm_stream",ms,cnt); counter_add(1,1,1,1,1,0); }
    secret_matches_free(ms);
    if (cut && g->inner) {
        char *prefix=malloc(cut+1);
        if (prefix) { memcpy(prefix,window,cut); prefix[cut]=0; }
        char *out=prefix ? secret_redact_ssh_passwords(prefix,cut,NULL):NULL;
        g->inner(out ? out:"[SSH password filtering failed]",g->ud);
        free(out); free(prefix);
    }
    g->hold_len=wlen-cut;
    memcpy(g->hold,window+cut,g->hold_len); g->hold[g->hold_len]=0;
}

void secret_stream_guard_cb(const char *delta, void *vg) {
    stream_guard *g = (stream_guard *)vg;
    size_t dlen;
    char *window;
    size_t wlen;

    if (!g || !delta)
        return;
    if (!g->inner)
        return;
    dlen = strlen(delta);
    if (dlen == 0)
        return;

    wlen = g->hold_len + dlen;
    window = (char *)malloc(wlen + 1);
    if (!window) {
        g->inner("[SSH password filtering failed]", g->ud); g->hold_len=0;
        return;
    }
    memcpy(window, g->hold, g->hold_len);
    memcpy(window + g->hold_len, delta, dlen);
    window[wlen] = '\0';
    guard_emit_window(g, window, wlen, dlen);
    free(window);
}

void secret_stream_guard_free(void *vg) {
    stream_guard *g = (stream_guard *)vg;
    if (!g)
        return;
    /* flush the held tail: scan and emit it */
    if (g->hold_len && g->inner) {
        secret_match *ms = NULL;
        int cnt = secret_scan_ssh_passwords(g->hold, g->hold_len, &ms);
        if (cnt > 0) {
            char *out = secret_redact_ssh_passwords(g->hold, g->hold_len, NULL);
            audit_scan("llm_stream", ms, cnt);
            counter_add(1, strongest(ms, cnt) >= SECRET_SEV_LOW,
                        strongest(ms, cnt) >= SECRET_SEV_MEDIUM,
                        strongest(ms, cnt) >= SECRET_SEV_HIGH, 0, 0);
            g->inner(out ? out : "[SSH password filtering failed]", g->ud);
            free(out);
        } else if (cnt < 0) {
            g->inner("[SSH password filtering failed]", g->ud);
        } else {
            g->inner(g->hold, g->ud);
        }
        secret_matches_free(ms);
    }
    free(g);
}

/* --- stats --------------------------------------------------------------------- */

char *secret_stats_json(void) {
    long long scans, low, medium, high, redactions, blocked;
    char *s;
    lock_init_once();
    mutex_lock(&g_mtx);
    scans = g_scans;
    low = g_low;
    medium = g_medium;
    high = g_high;
    redactions = g_redactions;
    blocked = g_blocked;
    mutex_unlock(&g_mtx);
    s = (char *)malloc(256);
    if (!s)
        return NULL;
    snprintf(s, 256,
             "{\"mode\":\"%s\",\"scans\":%lld,\"low\":%lld,\"medium\":%lld,"
             "\"high\":%lld,\"redactions\":%lld,\"blocked\":%lld}",
             secret_mode(), scans, low, medium, high, redactions, blocked);
    return s;
}

void secret_stats_reset(void) {
    lock_init_once();
    mutex_lock(&g_mtx);
    g_scans = g_low = g_medium = g_high = g_redactions = g_blocked = 0;
    mutex_unlock(&g_mtx);
}
