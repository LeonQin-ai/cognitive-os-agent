#include "cognitive-os-agent/os/http.h"
#include "cognitive-os-agent/os/os_socket.h"

/* On Windows the HTTPS-capable backend lives in http_winhttp.c; this plaintext
 * implementation is only used on non-Windows platforms. HTTPS on POSIX goes
 * through a libcurl backend loaded lazily via dlopen (no build-time dep). */
#ifndef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <dlfcn.h>
#include <pthread.h>

#define HTTP_BUF 4096

typedef enum { CHUNK_SIZE = 0, CHUNK_DATA = 1, CHUNK_CRLF = 2, CHUNK_DONE = 3 } chunk_state;

struct http_stream {
    /* curl variant: the whole response is buffered at open time and reads
     * replay from it (SSE events still parse in order, just not incremental) */
    int via_curl;
    char *c_body;
    size_t c_len;
    size_t c_pos;
    /* plain-socket variant */
    sock *sock;
    char buf[HTTP_BUF];
    size_t pos;
    size_t len;
    int pb_has;
    char pb;
    int header_done;
    int chunked;
    int64_t chunk_remaining;
    chunk_state cstate;
    int64_t content_remaining; /* -1 = unknown */
    int status;
};

/* ---------- libcurl backend (dlopen'd; HTTPS on POSIX) ---------- */
typedef void CURL;
struct curl_slist;
typedef int CURLcode;

/* CURLOPT/CURLINFO values are stable ABI constants of libcurl. */
#define CURLOPT_WRITEDATA 10001L
#define CURLOPT_URL 10002L
#define CURLOPT_ERRORBUFFER 10010L
#define CURLOPT_HEADERDATA 10029L
#define CURLOPT_POSTFIELDS 10015L
#define CURLOPT_HTTPHEADER 10023L
#define CURLOPT_ACCEPT_ENCODING 10102L
#define CURLOPT_WRITEFUNCTION 20011L
#define CURLOPT_HEADERFUNCTION 20079L
#define CURLOPT_NOPROGRESS 43L
#define CURLOPT_FOLLOWLOCATION 52L
#define CURLOPT_POST 47L
#define CURLOPT_POSTFIELDSIZE 60L
#define CURLOPT_NOSIGNAL 99L
#define CURLOPT_TIMEOUT_MS 155L
#define CURLOPT_CONNECTTIMEOUT_MS 156L
#define CURLINFO_RESPONSE_CODE (0x200000L + 2L)

static struct {
    void *lib;
    CURL *(*easy_init)(void);
    void (*easy_cleanup)(CURL *);
    CURLcode (*easy_setopt)(CURL *, int option, ...);
    CURLcode (*easy_perform)(CURL *);
    CURLcode (*easy_getinfo)(CURL *, int info, ...);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
    CURLcode (*global_init)(long flags);
    int loaded; /* 1 = resolved, -1 = unavailable */
} cu;

static pthread_once_t cu_once = PTHREAD_ONCE_INIT;

static void cu_load(void) {
    const char *names[] = {"libcurl.so.4", "libcurl.so", NULL};
    for (int i = 0; names[i] && !cu.lib; i++)
        cu.lib = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
    if (!cu.lib) {
        cu.loaded = -1;
        return;
    }
    cu.easy_init = (CURL * (*)(void)) dlsym(cu.lib, "curl_easy_init");
    cu.easy_cleanup = (void (*)(CURL *))dlsym(cu.lib, "curl_easy_cleanup");
    cu.easy_setopt = (CURLcode (*)(CURL *, int, ...))dlsym(cu.lib, "curl_easy_setopt");
    cu.easy_perform = (CURLcode (*)(CURL *))dlsym(cu.lib, "curl_easy_perform");
    cu.easy_getinfo = (CURLcode (*)(CURL *, int, ...))dlsym(cu.lib, "curl_easy_getinfo");
    cu.slist_append = (struct curl_slist * (*)(struct curl_slist *, const char *)) dlsym(cu.lib, "curl_slist_append");
    cu.slist_free_all = (void (*)(struct curl_slist *))dlsym(cu.lib, "curl_slist_free_all");
    CURLcode (*global_init)(long) = (CURLcode (*)(long))dlsym(cu.lib, "curl_global_init");
    if (!cu.easy_init || !cu.easy_setopt || !cu.easy_perform || !cu.easy_getinfo || !cu.slist_append ||
        !cu.easy_cleanup) {
        cu.loaded = -1;
        return;
    }
    if (global_init)
        global_init(3L /* CURL_GLOBAL_ALL */);
    cu.loaded = 1;
}

static int cu_ok(void) {
    pthread_once(&cu_once, cu_load);
    return cu.loaded == 1;
}

typedef struct {
    strbuf body;
    strmap headers; /* only used by the full-response path */
    int want_headers;
} cu_sink;

static size_t cu_write_cb(const char *ptr, size_t size, size_t nmemb, void *ud) {
    cu_sink *s = (cu_sink *)ud;
    size_t n = size * nmemb;
    strbuf_append_n(&s->body, ptr, n);
    return n;
}

static size_t cu_header_cb(const char *ptr, size_t size, size_t nmemb, void *ud) {
    cu_sink *s = (cu_sink *)ud;
    if (!s->want_headers)
        return size * nmemb;
    size_t n = size * nmemb;
    char *line = (char *)malloc(n + 1);
    if (!line)
        return n;
    memcpy(line, ptr, n);
    line[n] = '\0';
    char *nl = strpbrk(line, "\r\n");
    if (nl)
        *nl = '\0';
    char *colon = strchr(line, ':');
    if (colon && colon != line) {
        *colon = '\0';
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        strmap_set(&s->headers, line, v);
    }
    free(line);
    return n;
}

/* Perform one full HTTPS request via libcurl. Returns a curl-backed stream
 * (whole body buffered) or NULL on failure. */
static http_stream *curl_open(const char *base_url, const char *method, const char *path, const char *body,
                                  const char *content_type, strmap *extra_headers, int timeout_ms,
                                  strmap *headers_out) {
    if (!cu_ok()) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "http: https:// requested but libcurl is not "
                            "available (install libcurl4)\n");
            warned = 1;
        }
        return NULL;
    }

    char url[2048];
    size_t bl = strlen(base_url);
    while (bl > 0 && base_url[bl - 1] == '/')
        bl--;
    if (path && path[0] == '/')
        snprintf(url, sizeof(url), "%.*s%s", (int)bl, base_url, path);
    else
        snprintf(url, sizeof(url), "%.*s/%s", (int)bl, base_url, path ? path : "");

    CURL *h = cu.easy_init();
    if (!h)
        return NULL;
    cu_sink sink;
    memset(&sink, 0, sizeof(sink));
    strbuf_init(&sink.body);
    sink.want_headers = headers_out != NULL;
    char errbuf[256] = {0};

    struct curl_slist *hdrs = NULL;
    if (content_type && *content_type) {
        char ct[256];
        snprintf(ct, sizeof(ct), "Content-Type: %s", content_type);
        hdrs = cu.slist_append(hdrs, ct);
    }
    if (extra_headers) {
        for (size_t i = 0; i < extra_headers->count; i++) {
            char hv[512];
            snprintf(hv, sizeof(hv), "%s: %s", extra_headers->items[i].key, extra_headers->items[i].val);
            hdrs = cu.slist_append(hdrs, hv);
        }
    }
    /* always accept compressed responses; curl decompresses transparently */

    int is_post = method && strcmp(method, "POST") == 0;
    cu.easy_setopt(h, (int)CURLOPT_URL, url);
    cu.easy_setopt(h, (int)CURLOPT_WRITEFUNCTION, &cu_write_cb);
    cu.easy_setopt(h, (int)CURLOPT_WRITEDATA, &sink);
    cu.easy_setopt(h, (int)CURLOPT_HEADERFUNCTION, &cu_header_cb);
    cu.easy_setopt(h, (int)CURLOPT_HEADERDATA, &sink);
    cu.easy_setopt(h, (int)CURLOPT_ERRORBUFFER, errbuf);
    cu.easy_setopt(h, (int)CURLOPT_NOPROGRESS, 1L);
    cu.easy_setopt(h, (int)CURLOPT_NOSIGNAL, 1L);
    cu.easy_setopt(h, (int)CURLOPT_FOLLOWLOCATION, 1L);
    cu.easy_setopt(h, (int)CURLOPT_ACCEPT_ENCODING, "");
    if (timeout_ms > 0) {
        cu.easy_setopt(h, (int)CURLOPT_TIMEOUT_MS, (long)timeout_ms);
        long ct = timeout_ms < 30000 ? timeout_ms : 30000;
        cu.easy_setopt(h, (int)CURLOPT_CONNECTTIMEOUT_MS, ct);
    }
    if (is_post) {
        cu.easy_setopt(h, (int)CURLOPT_POST, 1L);
        cu.easy_setopt(h, (int)CURLOPT_POSTFIELDS, body ? body : "");
        cu.easy_setopt(h, (int)CURLOPT_POSTFIELDSIZE, (long)(body ? strlen(body) : 0));
    }
    if (hdrs)
        cu.easy_setopt(h, (int)CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = cu.easy_perform(h);
    long status = 0;
    if (rc == 0)
        cu.easy_getinfo(h, (int)CURLINFO_RESPONSE_CODE, &status);
    if (hdrs)
        cu.slist_free_all(hdrs);
    cu.easy_cleanup(h);

    if (rc != 0) {
        strbuf_free(&sink.body);
        strmap_free(&sink.headers);
        return NULL;
    }

    http_stream *s = (http_stream *)calloc(1, sizeof(http_stream));
    if (!s) {
        strbuf_free(&sink.body);
        strmap_free(&sink.headers);
        return NULL;
    }
    s->via_curl = 1;
    s->status = (int)status;
    s->c_body = strbuf_detach(&sink.body);
    s->c_len = s->c_body ? strlen(s->c_body) : 0;
    s->c_pos = 0;
    if (headers_out) {
        *headers_out = sink.headers;
    } else {
        strmap_free(&sink.headers);
    }
    return s;
}

/* ---------- raw buffered reads ---------- */
static int http_fill(http_stream *h) {
    if (h->pos < h->len)
        return (int)(h->len - h->pos);
    h->pos = 0;
    h->len = 0;
    int n = sock_recv(h->sock, h->buf, HTTP_BUF);
    if (n <= 0)
        return -1;
    h->len = (size_t)n;
    return (int)h->len;
}

static int http_getc(http_stream *h) {
    if (http_fill(h) <= 0)
        return -1;
    return (unsigned char)h->buf[h->pos++];
}

static int http_raw_line(http_stream *h, char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        int c = http_getc(h);
        if (c < 0)
            return -1;
        if (c == '\n')
            break;
        if (c == '\r')
            continue;
        if (n + 1 < cap)
            out[n++] = (char)c;
    }
    out[n] = '\0';
    return (int)n;
}

static int64_t parse_chunk_size(const char *line) {
    int64_t v = 0;
    for (const char *p = line; *p; p++) {
        int c = tolower((unsigned char)*p);
        if (c == ';')
            break;
        if (c >= '0' && c <= '9')
            v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f')
            v = v * 16 + (c - 'a' + 10);
        else
            break;
    }
    return v;
}

/* ---------- decoded (transfer-decoded) byte reads ---------- */
static int decode_getc(http_stream *h) {
    if (h->pb_has) {
        h->pb_has = 0;
        return (unsigned char)h->pb;
    }
    if (h->chunked) {
        for (;;) {
            if (h->cstate == CHUNK_DONE)
                return -1;
            if (h->cstate == CHUNK_DATA) {
                if (h->chunk_remaining == 0) {
                    h->cstate = CHUNK_CRLF;
                    continue;
                }
                int c = http_getc(h);
                if (c < 0)
                    return -1;
                h->chunk_remaining--;
                return c;
            }
            if (h->cstate == CHUNK_CRLF) {
                int c = http_getc(h);
                if (c < 0)
                    return -1;
                if (c == '\r')
                    continue;
                if (c == '\n') {
                    h->cstate = CHUNK_SIZE;
                    continue;
                }
                continue; /* tolerate stray bytes */
            }
            /* CHUNK_SIZE: read a hex-size line */
            char line[128];
            size_t n = 0;
            for (;;) {
                int c = http_getc(h);
                if (c < 0)
                    return -1;
                if (c == '\n')
                    break;
                if (c == '\r')
                    continue;
                if (n + 1 < sizeof(line))
                    line[n++] = (char)c;
            }
            line[n] = '\0';
            int64_t sz = parse_chunk_size(line);
            if (sz <= 0) {
                /* 0-size chunk ends the body */
                h->cstate = CHUNK_DONE;
                return -1;
            }
            h->chunk_remaining = sz;
            h->cstate = CHUNK_DATA;
        }
    }
    if (h->content_remaining >= 0) {
        if (h->content_remaining == 0)
            return -1;
        int c = http_getc(h);
        if (c < 0)
            return -1;
        h->content_remaining--;
        return c;
    }
    return http_getc(h);
}

int http_stream_read_line(http_stream *h, char *out, size_t cap) {
    size_t n = 0;
    if (h && h->via_curl) {
        /* replay from the buffered curl response, dropping \r like the
         * plain path does */
        while (h->c_pos < h->c_len && n + 1 < cap) {
            char c = h->c_body[h->c_pos++];
            if (c == '\n')
                break;
            if (c == '\r')
                continue;
            out[n++] = c;
        }
        out[n] = '\0';
        if (n == 0 && h->c_pos >= h->c_len)
            return -1;
        return (int)n;
    }
    for (;;) {
        int c = decode_getc(h);
        if (c < 0) {
            if (n > 0)
                break;
            return -1;
        }
        if (c == '\n')
            break;
        if (c == '\r')
            continue;
        if (n + 1 < cap)
            out[n++] = (char)c;
        else
            break;
    }
    out[n] = '\0';
    return (int)n;
}

int http_stream_read(http_stream *h, char *out, size_t cap) {
    if (h && h->via_curl) {
        size_t n = h->c_len - h->c_pos;
        if (n > cap)
            n = cap;
        memcpy(out, h->c_body + h->c_pos, n);
        h->c_pos += n;
        return (int)n;
    }
    size_t n = 0;
    while (n < cap) {
        int c = decode_getc(h);
        if (c < 0)
            break;
        out[n++] = (char)c;
    }
    return (int)n;
}

int http_stream_status(http_stream *h) {
    return h ? h->status : 0;
}

/* ---------- response head ---------- */
static int parse_response_head(http_stream *h) {
    char line[1024];
    int n = http_raw_line(h, line, sizeof(line));
    if (n < 0)
        return -1;
    if (strncmp(line, "HTTP/1.", 7) != 0)
        return -1;
    const char *p = line + 7;
    /* skip the minor version digit(s) and spaces, e.g. "HTTP/1.1 200 OK" */
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    int status = 0;
    while (*p >= '0' && *p <= '9') {
        status = status * 10 + (*p - '0');
        p++;
    }
    h->status = status;

    int chunked = 0;
    int64_t content_len = -1;
    for (;;) {
        if (http_raw_line(h, line, sizeof(line)) < 0)
            return -1;
        if (line[0] == '\0')
            break;
        char lname[256];
        size_t i;
        for (i = 0; i < strlen(line) && i < 250; i++)
            lname[i] = (char)tolower((unsigned char)line[i]);
        lname[i] = '\0';
        if (strncmp(lname, "transfer-encoding:", 18) == 0 && strstr(line, "chunked"))
            chunked = 1;
        else if (strncmp(lname, "content-length:", 15) == 0) {
            const char *v = strchr(line, ':');
            if (v)
                content_len = strtoll(v + 1, NULL, 10);
        }
    }
    h->chunked = chunked;
    h->content_remaining = content_len;
    h->cstate = chunked ? CHUNK_SIZE : CHUNK_DATA;
    h->header_done = 1;
    return 0;
}

/* ---------- URL parsing (plain-http path; https dispatches to libcurl
 * before this is ever reached) ---------- */
static int parse_base_url(const char *base, char *host, size_t hostsz, uint16_t *port) {
    const char *p = base;
    if (strncmp(p, "http://", 7) == 0)
        p += 7;
    else if (strncmp(p, "https://", 8) == 0)
        return -1;
    *port = 80;
    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i + 1 < hostsz)
        host[i++] = *p++;
    host[i] = '\0';
    if (i == 0)
        return -1;
    if (*p == ':') {
        p++;
        long prt = strtol(p, NULL, 10);
        if (prt > 0 && prt < 65536)
            *port = (uint16_t)prt;
    }
    return 0;
}

/* ---------- open connection ---------- */
static http_stream *http_open(const char *base_url, const char *method, const char *path, const char *body,
                                  const char *content_type, strmap *extra_headers, int timeout_ms) {
    char host[256];
    uint16_t port;
    if (parse_base_url(base_url, host, sizeof(host), &port) != 0)
        return NULL;

    sock *sock = sock_connect(host, port, timeout_ms > 0 ? timeout_ms : 10000);
    if (!sock)
        return NULL;

    strbuf sb;
    strbuf_init(&sb);
    size_t blen = body ? strlen(body) : 0;
    strbuf_appendf(&sb, "%s %s HTTP/1.1\r\n", method, path);
    strbuf_appendf(&sb, "Host: %s:%u\r\n", host, (unsigned)port);
    strbuf_append(&sb, "User-Agent: cognitive-os-agent/0.1\r\n");
    if (content_type && blen)
        strbuf_appendf(&sb, "Content-Type: %s\r\n", content_type);
    if (blen)
        strbuf_appendf(&sb, "Content-Length: %zu\r\n", blen);
    strbuf_append(&sb, "Connection: keep-alive\r\n");
    if (extra_headers) {
        for (size_t i = 0; i < extra_headers->count; i++)
            strbuf_appendf(&sb, "%s: %s\r\n", extra_headers->items[i].key, extra_headers->items[i].val);
    }
    strbuf_append(&sb, "\r\n");
    if (blen)
        strbuf_append_n(&sb, body, blen);

    int sent = sock_send(sock, sb.buf, sb.len);
    int ok = (sent == (int)sb.len);
    strbuf_free(&sb);
    if (!ok) {
        sock_close(sock);
        return NULL;
    }

    http_stream *h = calloc(1, sizeof(http_stream));
    if (!h) {
        sock_close(sock);
        return NULL;
    }
    h->sock = sock;
    h->content_remaining = -1;
    if (parse_response_head(h) != 0) {
        http_stream_close(h);
        return NULL;
    }
    return h;
}

void http_stream_close(http_stream *h) {
    if (!h)
        return;
    if (h->via_curl) {
        free(h->c_body);
        free(h);
        return;
    }
    if (h->sock)
        sock_close(h->sock);
    free(h);
}

/* ---------- full responses ---------- */
static http_response *http_full(const char *base_url, const char *method, const char *path, const char *body,
                                    const char *content_type, strmap *extra_headers, int timeout_ms) {
    if (strncmp(base_url, "https://", 8) == 0) {
        /* TLS path: libcurl buffers the whole response, then we expose it
         * through the same response shape as the plain backend */
        strmap headers;
        memset(&headers, 0, sizeof(headers));
        http_stream *h = curl_open(base_url, method, path, body, content_type, extra_headers, timeout_ms, &headers);
        if (!h)
            return NULL;
        http_response *r = (http_response *)calloc(1, sizeof(http_response));
        if (!r) {
            strmap_free(&headers);
            http_stream_close(h);
            return NULL;
        }
        r->status = h->status;
        r->body = h->c_body;
        r->body_len = h->c_len;
        h->c_body = NULL;
        r->headers = headers;
        http_stream_close(h);
        return r;
    }
    http_stream *h = http_open(base_url, method, path, body, content_type, extra_headers, timeout_ms);
    if (!h)
        return NULL;

    http_response *r = calloc(1, sizeof(http_response));
    if (!r) {
        http_stream_close(h);
        return NULL;
    }
    r->status = h->status;

    strbuf sb;
    strbuf_init(&sb);
    char tmp[8192];
    int n;
    while ((n = http_stream_read(h, tmp, sizeof(tmp))) > 0) {
        strbuf_append_n(&sb, tmp, (size_t)n);
        if (sb.len > 64u * 1024u * 1024u)
            break;
    }
    r->body = strbuf_detach(&sb);
    r->body_len = strlen(r->body);
    http_stream_close(h);
    return r;
}

http_response *http_post(const char *base_url, const char *path, const char *body, const char *content_type,
                                 strmap *extra_headers, int timeout_ms) {
    return http_full(base_url, "POST", path, body, content_type, extra_headers, timeout_ms);
}

http_response *http_get(const char *base_url, const char *path, strmap *extra_headers, int timeout_ms) {
    return http_full(base_url, "GET", path, NULL, NULL, extra_headers, timeout_ms);
}

http_stream *http_stream_open(const char *base_url, const char *method, const char *path, const char *body,
                                      const char *content_type, strmap *extra_headers, int timeout_ms) {
    if (strncmp(base_url, "https://", 8) == 0)
        return curl_open(base_url, method, path, body, content_type, extra_headers, timeout_ms, NULL);
    return http_open(base_url, method, path, body, content_type, extra_headers, timeout_ms);
}

void http_response_free(http_response *r) {
    if (!r)
        return;
    free(r->body);
    strmap_free(&r->headers);
    free(r);
}

#endif /* _WIN32 */
