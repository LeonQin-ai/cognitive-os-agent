/* secret_detector.c — deterministic secret pattern detector + entropy
 * classifier (SECRET_SECURITY_DDD_V1.0 §7.2) and the redaction formatter
 * (§8.1). Rules are deterministic first; no LLM in the detection path. */
#include "security/secret.h"
#include "infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#if !defined(_WIN32)
#include <strings.h> /* strcasecmp (POSIX) */
#endif

/* --- helpers -------------------------------------------------------------- */

static int is_b64ish(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '=' ||
           c == '+' || c == '/';
}

static int is_tokish(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

/* Placeholder-looking values that must not be flagged (§7.3 false positives):
 * <...>, ${...}, %s/%d, "changeme", "xxx…", "example", "your-", "…", empty. */
static int is_placeholder(const char *s, size_t len) {
    static const char *const words[] = {
        "changeme", "change_me", "example", "placeholder", "dummy", "sample",
        "your-", "your_", "xxxx", "****", "....", "todo", "fixme", "redacted",
        "none", "null", "true", "false", "test", "default", "secret_here",
        "password_here", "<", "${", "%s", "%d", "{", "$", "*", "#", "?"
    };
    size_t i;
    if (len < 6)
        return 1; /* too short to be real material */
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        size_t wl = strlen(words[i]);
        if (len >= wl) {
            char head[32], tail[32];
            size_t cl = wl < sizeof head ? wl : sizeof head - 1;
            memcpy(head, s, cl); head[cl] = '\0';
            memcpy(tail, s + len - cl, cl); tail[cl] = '\0';
#ifdef _WIN32
            if (_stricmp(head, words[i]) == 0 || _stricmp(tail, words[i]) == 0)
                return 1;
#else
            if (strcasecmp(head, words[i]) == 0 || strcasecmp(tail, words[i]) == 0)
                return 1;
#endif
        }
    }
    return 0;
}

/* Shannon entropy over byte frequencies, bits/byte. */
static double shannon(const char *s, size_t len) {
    unsigned freq[256];
    double h = 0.0;
    size_t i;
    memset(freq, 0, sizeof freq);
    for (i = 0; i < len; i++)
        freq[(unsigned char)s[i]]++;
    for (i = 0; i < 256; i++) {
        if (freq[i]) {
            double p = (double)freq[i] / (double)len;
            h -= p * (log(p) / log(2.0));
        }
    }
    return h;
}

/* --- matches array ---------------------------------------------------------- */

typedef struct {
    secret_match *items;
    int count, cap;
} matchvec;

static void mv_push(matchvec *mv, size_t start, size_t end,
                    secret_severity_t sev, const char *kind) {
    if (mv->count == mv->cap) {
        int ncap = mv->cap ? mv->cap * 2 : 16;
        secret_match *ni = (secret_match *)realloc(mv->items,
                                                   (size_t)ncap * sizeof(secret_match));
        if (!ni)
            return;
        mv->items = ni;
        mv->cap = ncap;
    }
    mv->items[mv->count].start = start;
    mv->items[mv->count].end = end;
    mv->items[mv->count].severity = sev;
    snprintf(mv->items[mv->count].kind, sizeof mv->items[mv->count].kind, "%s", kind);
    mv->count++;
}

/* Push unless overlapping a previous HIGH match (first/highest wins). */
static void mv_push_nonoverlapping(matchvec *mv, size_t start, size_t end,
                                   secret_severity_t sev, const char *kind) {
    int i;
    for (i = 0; i < mv->count; i++) {
        secret_match *m = &mv->items[i];
        if (start < m->end && m->start < end) {
            if (m->severity >= sev)
                return;                 /* existing stronger match covers it */
            m->start = start;           /* weaker one is replaced */
            m->end = end;
            m->severity = sev;
            snprintf(m->kind, sizeof m->kind, "%s", kind);
            return;
        }
    }
    mv_push(mv, start, end, sev, kind);
}

static int cmp_match_start(const void *a, const void *b) {
    const secret_match *x = (const secret_match *)a;
    const secret_match *y = (const secret_match *)b;
    if (x->start != y->start)
        return x->start < y->start ? -1 : 1;
    return 0;
}

/* --- pattern detector ------------------------------------------------------- */

/* Case-insensitive find of needle in [p, end). */
static const char *find_ci(const char *p, const char *end, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || (size_t)(end - p) < nl)
        return NULL;
    for (; p <= end - nl; p++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (i == nl)
            return p;
    }
    return NULL;
}

static void scan_patterns(const char *text, size_t len, matchvec *mv) {
    const char *p = text, *end = text + len;

    /* 1. PEM private key blocks — the header line alone is decisive (§7.3). */
    {
        static const char *const headers[] = {
            "-----begin openssh private key-----",
            "-----begin rsa private key-----",
            "-----begin dsa private key-----",
            "-----begin ec private key-----",
            "-----begin pgp private key block-----",
            "-----begin private key-----"
        };
        size_t h;
        for (h = 0; h < sizeof(headers) / sizeof(headers[0]); h++) {
            const char *hit = find_ci(p, end, headers[h]);
            while (hit) {
                const char *e = hit + strlen(headers[h]);
                mv_push_nonoverlapping(mv, (size_t)(hit - text), (size_t)(e - text),
                                       SECRET_SEV_HIGH, "pem_key");
                hit = find_ci(e, end, headers[h]);
            }
        }
    }

    /* 2. Bearer / Basic authorization values. */
    {
        static const char *const schemes[] = { "bearer ", "basic " };
        size_t s;
        for (s = 0; s < sizeof(schemes) / sizeof(schemes[0]); s++) {
            const char *hit = find_ci(p, end, schemes[s]);
            while (hit) {
                const char *v = hit + strlen(schemes[s]);
                const char *e = v;
                while (e < end && (is_b64ish(*e) || *e == '.') &&
                       (size_t)(e - v) < 512)
                    e++;
                if (e - v >= 16 && !is_placeholder(v, (size_t)(e - v)))
                    mv_push_nonoverlapping(mv, (size_t)(v - text), (size_t)(e - text),
                                           SECRET_SEV_HIGH,
                                           s == 0 ? "bearer" : "basic_auth");
                hit = find_ci(e, end, schemes[s]);
            }
        }
    }

    /* 3. Fixed-prefix tokens: JWT, AWS, common api-key shapes. */
    {
        const char *c = p;
        while (c < end) {
            size_t off = (size_t)(c - text);
            size_t rem = len - off;
            if (rem > 8 && strncmp(c, "eyJ", 3) == 0) {
                /* JWT: eyJ…(.eyJ…)?.signature */
                const char *e = c;
                int dots = 0;
                while (e < end && (size_t)(e - c) < 2048) {
                    if (*e == '.') {
                        dots++;
                        if (dots > 2)
                            break;
                    } else if (!is_b64ish(*e)) {
                        break;
                    }
                    e++;
                }
                if (dots >= 2 && (size_t)(e - c) >= 32)
                    mv_push_nonoverlapping(mv, off, (size_t)(e - text),
                                           SECRET_SEV_HIGH, "jwt");
                c = e;
                continue;
            }
            if (rem >= 20 && strncmp(c, "AKIA", 4) == 0 &&
                isalnum((unsigned char)c[4]) && isupper((unsigned char)c[4])) {
                const char *e = c + 4;
                /* access key ids are 16 uppercase alphanumerics after AKIA */
                while (e < end && e - c < 20 &&
                       (isupper((unsigned char)*e) || isdigit((unsigned char)*e)))
                    e++;
                if (e - c == 20)
                    mv_push_nonoverlapping(mv, off, (size_t)(e - text),
                                           SECRET_SEV_HIGH, "aws_key");
                c = e;
                continue;
            }
            if (rem > 23 && strncmp(c, "sk-", 3) == 0) {
                const char *e = c + 3;
                while (e < end && is_tokish(*e))
                    e++;
                if (e - c >= 23)
                    mv_push_nonoverlapping(mv, off, (size_t)(e - text),
                                           SECRET_SEV_HIGH, "api_key");
                c = e;
                continue;
            }
            if (rem > 36 && strncmp(c, "ghp_", 4) == 0) {
                const char *e = c + 4;
                while (e < end && is_tokish(*e))
                    e++;
                if (e - c >= 36)
                    mv_push_nonoverlapping(mv, off, (size_t)(e - text),
                                           SECRET_SEV_HIGH, "api_key");
                c = e;
                continue;
            }
            if (rem > 20 && (strncmp(c, "xoxb-", 5) == 0 || strncmp(c, "xoxp-", 5) == 0 ||
                             strncmp(c, "xoxa-", 5) == 0 || strncmp(c, "xoxr-", 5) == 0)) {
                const char *e = c + 5;
                while (e < end && is_tokish(*e))
                    e++;
                if (e - c >= 20)
                    mv_push_nonoverlapping(mv, off, (size_t)(e - text),
                                           SECRET_SEV_HIGH, "api_key");
                c = e;
                continue;
            }
            if (rem > 39 && strncmp(c, "AIza", 4) == 0) {
                const char *e = c + 4;
                while (e < end && (is_tokish(*e) || *e == '_') && e - c < 39)
                    e++;
                if (e - c == 39)
                    mv_push_nonoverlapping(mv, off, (size_t)(e - text),
                                           SECRET_SEV_HIGH, "api_key");
                c = e;
                continue;
            }
            c++;
        }
    }

    /* 4. key = value credential assignments (§7.3 "password=...", "api_key=..."). */
    {
        static const char *const keys[] = {
            "password", "passwd", "pwd", "secret", "token", "api_key",
            "apikey", "api-key", "access_key", "private_key", "client_secret",
            "auth"
        };
        size_t k;
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            const char *hit = find_ci(p, end, keys[k]);
            while (hit) {
                const char *c = hit + strlen(keys[k]);
                /* separator: = or : (optionally around spaces) */
                while (c < end && (*c == ' ' || *c == '\t'))
                    c++;
                if (c < end && (*c == '=' || *c == ':')) {
                    const char *v;
                    const char *e;
                    char quote = 0;
                    c++;
                    while (c < end && (*c == ' ' || *c == '\t'))
                        c++;
                    if (c < end && (*c == '"' || *c == '\'')) {
                        quote = *c;
                        c++;
                    }
                    v = c;
                    while (c < end && *c != '\n' && *c != '\r' &&
                           (quote ? *c != quote : *c != '"' && *c != '\'' &&
                                                      *c != ' ' && *c != '\t' &&
                                                      *c != ',' && *c != ';' &&
                                                      *c != ')')) {
                        c++;
                    }
                    e = c;
                    /* trim trailing quote already excluded; check the value */
                    if (e > v && (e - v) >= 6 &&
                        !is_placeholder(v, (size_t)(e - v)) &&
                        shannon(v, (size_t)(e - v)) >= 2.5)
                        mv_push_nonoverlapping(mv, (size_t)(v - text), (size_t)(e - text),
                                               SECRET_SEV_HIGH, "kv_credential");
                    hit = (c < end) ? find_ci(c, end, keys[k]) : NULL;
                    continue;
                }
                hit = (c < end) ? find_ci(c, end, keys[k]) : NULL;
            }
        }
    }
}

/* --- entropy detector (fallback) -------------------------------------------- */

static void scan_entropy(const char *text, size_t len, matchvec *mv) {
    size_t run_start = 0, i = 0;
    while (i <= len) {
        int cls = (i < len && (is_b64ish(text[i]) || text[i] == '.'));
        if (!cls) {
            size_t rl = i - run_start;
            if (rl >= 28) {
                double h = shannon(text + run_start, rl);
                if (h >= 4.5)
                    mv_push_nonoverlapping(mv, run_start, i,
                                           SECRET_SEV_MEDIUM, "entropy");
            }
            run_start = i + 1;
        }
        i++;
    }
}

/* --- public API -------------------------------------------------------------- */

int secret_scan_text(const char *text, size_t len, secret_match **out) {
    matchvec mv;
    int n;

    if (out)
        *out = NULL;
    if (!text || len == 0 || !out)
        return 0;
    mv.items = NULL;
    mv.count = mv.cap = 0;

    scan_patterns(text, len, &mv);
    scan_entropy(text, len, &mv);
    if (mv.count > 1)
        qsort(mv.items, (size_t)mv.count, sizeof(secret_match), cmp_match_start);
    n = mv.count;
    *out = mv.items;
    return n;
}

void secret_matches_free(secret_match *m) {
    free(m);
}

char *secret_redact_text(const char *text, size_t len, const char *replacement) {
    const char *rep = replacement ? replacement : "[REDACTED:secret]";
    secret_match *ms = NULL;
    int n, i;
    strbuf sb;
    size_t pos = 0;

    if (!text)
        return NULL;
    n = secret_scan_text(text, len, &ms);
    if (n <= 0) {
        free(ms);
        return xstrdup(text);
    }
    strbuf_init(&sb);
    for (i = 0; i < n; i++) {
        const secret_match *m = &ms[i];
        if (m->severity < SECRET_SEV_MEDIUM)
            continue;                      /* LOW passes through */
        if (m->start > pos)
            strbuf_append_n(&sb, text + pos, m->start - pos);
        strbuf_append(&sb, rep);
        pos = m->end;
    }
    if (pos < len)
        strbuf_append_n(&sb, text + pos, len - pos);
    free(ms);
    return strbuf_detach(&sb);
}
