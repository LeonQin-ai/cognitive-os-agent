#include "cognitive-os-agent/os/http.h"
#include "cognitive-os-agent/os/os_socket.h"

/* On Windows the HTTPS-capable backend lives in http_winhttp.c; this plaintext
 * implementation is only used on non-Windows platforms. */
#ifndef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define HTTP_BUF 4096

typedef enum { CHUNK_SIZE = 0, CHUNK_DATA = 1, CHUNK_CRLF = 2, CHUNK_DONE = 3 } chunk_state;

struct coa_http_stream {
    coa_socket *sock;
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

/* ---------- URL parsing ---------- */
static int parse_base_url(const char *base, char *host, size_t hostsz, uint16_t *port) {
    const char *p = base;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) return -1; /* TLS via libcurl backend only */
    *port = 80;
    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i + 1 < hostsz) host[i++] = *p++;
    host[i] = '\0';
    if (i == 0) return -1;
    if (*p == ':') {
        p++;
        long prt = strtol(p, NULL, 10);
        if (prt > 0 && prt < 65536) *port = (uint16_t)prt;
    }
    return 0;
}

/* ---------- raw buffered reads ---------- */
static int http_fill(coa_http_stream *h) {
    if (h->pos < h->len) return (int)(h->len - h->pos);
    h->pos = 0;
    h->len = 0;
    int n = coa_sock_recv(h->sock, h->buf, HTTP_BUF);
    if (n <= 0) return -1;
    h->len = (size_t)n;
    return (int)h->len;
}

static int http_getc(coa_http_stream *h) {
    if (http_fill(h) <= 0) return -1;
    return (unsigned char)h->buf[h->pos++];
}

static int http_raw_line(coa_http_stream *h, char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        int c = http_getc(h);
        if (c < 0) return -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n + 1 < cap) out[n++] = (char)c;
    }
    out[n] = '\0';
    return (int)n;
}

static int64_t parse_chunk_size(const char *line) {
    int64_t v = 0;
    for (const char *p = line; *p; p++) {
        int c = tolower((unsigned char)*p);
        if (c == ';') break;
        if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else break;
    }
    return v;
}

/* ---------- decoded (transfer-decoded) byte reads ---------- */
static int decode_getc(coa_http_stream *h) {
    if (h->pb_has) { h->pb_has = 0; return (unsigned char)h->pb; }
    if (h->chunked) {
        for (;;) {
            if (h->cstate == CHUNK_DONE) return -1;
            if (h->cstate == CHUNK_DATA) {
                if (h->chunk_remaining == 0) { h->cstate = CHUNK_CRLF; continue; }
                int c = http_getc(h);
                if (c < 0) return -1;
                h->chunk_remaining--;
                return c;
            }
            if (h->cstate == CHUNK_CRLF) {
                int c = http_getc(h);
                if (c < 0) return -1;
                if (c == '\r') continue;
                if (c == '\n') { h->cstate = CHUNK_SIZE; continue; }
                continue; /* tolerate stray bytes */
            }
            /* CHUNK_SIZE: read a hex-size line */
            char line[128];
            size_t n = 0;
            for (;;) {
                int c = http_getc(h);
                if (c < 0) return -1;
                if (c == '\n') break;
                if (c == '\r') continue;
                if (n + 1 < sizeof(line)) line[n++] = (char)c;
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
        if (h->content_remaining == 0) return -1;
        int c = http_getc(h);
        if (c < 0) return -1;
        h->content_remaining--;
        return c;
    }
    return http_getc(h);
}

int coa_http_stream_read_line(coa_http_stream *h, char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        int c = decode_getc(h);
        if (c < 0) {
            if (n > 0) break;
            return -1;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n + 1 < cap) out[n++] = (char)c;
        else break;
    }
    out[n] = '\0';
    return (int)n;
}

int coa_http_stream_read(coa_http_stream *h, char *out, size_t cap) {
    size_t n = 0;
    while (n < cap) {
        int c = decode_getc(h);
        if (c < 0) break;
        out[n++] = (char)c;
    }
    return (int)n;
}

int coa_http_stream_status(coa_http_stream *h) { return h ? h->status : 0; }

/* ---------- response head ---------- */
static int parse_response_head(coa_http_stream *h) {
    char line[1024];
    int n = http_raw_line(h, line, sizeof(line));
    if (n < 0) return -1;
    if (strncmp(line, "HTTP/1.", 7) != 0) return -1;
    const char *p = line + 7;
    /* skip the minor version digit(s) and spaces, e.g. "HTTP/1.1 200 OK" */
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    int status = 0;
    while (*p >= '0' && *p <= '9') { status = status * 10 + (*p - '0'); p++; }
    h->status = status;

    int chunked = 0;
    int64_t content_len = -1;
    for (;;) {
        if (http_raw_line(h, line, sizeof(line)) < 0) return -1;
        if (line[0] == '\0') break;
        char lname[256];
        size_t i;
        for (i = 0; i < strlen(line) && i < 250; i++) lname[i] = (char)tolower((unsigned char)line[i]);
        lname[i] = '\0';
        if (strncmp(lname, "transfer-encoding:", 18) == 0 && strstr(line, "chunked"))
            chunked = 1;
        else if (strncmp(lname, "content-length:", 15) == 0) {
            const char *v = strchr(line, ':');
            if (v) content_len = strtoll(v + 1, NULL, 10);
        }
    }
    h->chunked = chunked;
    h->content_remaining = content_len;
    h->cstate = chunked ? CHUNK_SIZE : CHUNK_DATA;
    h->header_done = 1;
    return 0;
}

/* ---------- open connection ---------- */
static coa_http_stream *http_open(const char *base_url, const char *method, const char *path,
                                 const char *body, const char *content_type,
                                 coa_strmap *extra_headers, int timeout_ms) {
    char host[256];
    uint16_t port;
    if (parse_base_url(base_url, host, sizeof(host), &port) != 0) return NULL;

    coa_socket *sock = coa_sock_connect(host, port, timeout_ms > 0 ? timeout_ms : 10000);
    if (!sock) return NULL;

    coa_strbuf sb;
    coa_strbuf_init(&sb);
    size_t blen = body ? strlen(body) : 0;
    coa_strbuf_appendf(&sb, "%s %s HTTP/1.1\r\n", method, path);
    coa_strbuf_appendf(&sb, "Host: %s:%u\r\n", host, (unsigned)port);
    coa_strbuf_append(&sb, "User-Agent: cognitive-os-agent/0.1\r\n");
    if (content_type && blen) coa_strbuf_appendf(&sb, "Content-Type: %s\r\n", content_type);
    if (blen) coa_strbuf_appendf(&sb, "Content-Length: %zu\r\n", blen);
    coa_strbuf_append(&sb, "Connection: keep-alive\r\n");
    if (extra_headers) {
        for (size_t i = 0; i < extra_headers->count; i++)
            coa_strbuf_appendf(&sb, "%s: %s\r\n", extra_headers->items[i].key, extra_headers->items[i].val);
    }
    coa_strbuf_append(&sb, "\r\n");
    if (blen) coa_strbuf_append_n(&sb, body, blen);

    int sent = coa_sock_send(sock, sb.buf, sb.len);
    int ok = (sent == (int)sb.len);
    coa_strbuf_free(&sb);
    if (!ok) {
        coa_sock_close(sock);
        return NULL;
    }

    coa_http_stream *h = calloc(1, sizeof(coa_http_stream));
    if (!h) { coa_sock_close(sock); return NULL; }
    h->sock = sock;
    h->content_remaining = -1;
    if (parse_response_head(h) != 0) {
        coa_http_stream_close(h);
        return NULL;
    }
    return h;
}

void coa_http_stream_close(coa_http_stream *h) {
    if (!h) return;
    if (h->sock) coa_sock_close(h->sock);
    free(h);
}

/* ---------- full responses ---------- */
static coa_http_response *http_full(const char *base_url, const char *method, const char *path,
                                   const char *body, const char *content_type,
                                   coa_strmap *extra_headers, int timeout_ms) {
    coa_http_stream *h = http_open(base_url, method, path, body, content_type, extra_headers, timeout_ms);
    if (!h) return NULL;

    coa_http_response *r = calloc(1, sizeof(coa_http_response));
    if (!r) { coa_http_stream_close(h); return NULL; }
    r->status = h->status;

    coa_strbuf sb;
    coa_strbuf_init(&sb);
    char tmp[8192];
    int n;
    while ((n = coa_http_stream_read(h, tmp, sizeof(tmp))) > 0) {
        coa_strbuf_append_n(&sb, tmp, (size_t)n);
        if (sb.len > 64u * 1024u * 1024u) break;
    }
    r->body = coa_strbuf_detach(&sb);
    r->body_len = strlen(r->body);
    coa_http_stream_close(h);
    return r;
}

coa_http_response *coa_http_post(const char *base_url, const char *path, const char *body,
                               const char *content_type, coa_strmap *extra_headers, int timeout_ms) {
    return http_full(base_url, "POST", path, body, content_type, extra_headers, timeout_ms);
}

coa_http_response *coa_http_get(const char *base_url, const char *path, coa_strmap *extra_headers, int timeout_ms) {
    return http_full(base_url, "GET", path, NULL, NULL, extra_headers, timeout_ms);
}

coa_http_stream *coa_http_stream_open(const char *base_url, const char *method, const char *path,
                                    const char *body, const char *content_type,
                                    coa_strmap *extra_headers, int timeout_ms) {
    return http_open(base_url, method, path, body, content_type, extra_headers, timeout_ms);
}

void coa_http_response_free(coa_http_response *r) {
    if (!r) return;
    free(r->body);
    coa_strmap_free(&r->headers);
    free(r);
}

#endif /* _WIN32 */
