/* http_winhttp.c — Windows HTTPS backend for the ca_http_* API.
 * The default os/http.c only speaks plaintext http:// (and refuses https://).
 * Real LLM providers (OpenAI/DeepSeek/Anthropic) are HTTPS-only, so on Windows
 * we implement the same API over WinHTTP, which handles TLS via the system
 * crypto stack. This file is compiled only on Windows (_WIN32); on other
 * platforms os/http.c provides these symbols instead. */
#include "cagent/os/http.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

struct ca_http_stream {
    char *body;
    size_t len;
    size_t pos;
    int status;
};

static wchar_t *to_wide(const char *s) {
    if (!s) return NULL;
    int wn = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (wn <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wn);
    return w;
}

/* Returns 1 for https, 0 for http, -1 on parse error. Fills host/port/basepath. */
static int parse_url(const char *url, char *host, size_t hostsz, int *port,
                     char *basepath, size_t bpsz) {
    const char *p = url;
    int https = 0;
    if (strncmp(p, "https://", 8) == 0) { https = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else return -1;

    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i + 1 < hostsz) host[i++] = *p++;
    host[i] = '\0';
    if (*p == ':') {
        p++;
        long prt = strtol(p, NULL, 10);
        if (prt > 0 && prt < 65536) *port = (int)prt;
        while (*p && *p != '/') p++;
    } else {
        *port = https ? 443 : 80;
    }

    size_t j = 0;
    if (*p == '/') {
        while (*p && j + 1 < bpsz) basepath[j++] = *p++;
    }
    if (j == 0) { basepath[0] = '/'; basepath[1] = '\0'; }
    else basepath[j] = '\0';
    return https;
}

static ca_http_response *do_request(const char *method, const char *base_url, const char *path,
                                    const char *body, const char *content_type,
                                    ca_strmap *extra_headers, int timeout_ms) {
    char host[256], basepath[1024];
    int port;
    int https = parse_url(base_url, host, sizeof(host), &port, basepath, sizeof(basepath));
    if (https < 0) return NULL;

    /* final request path = base_url path + caller path (avoid // and /v1/v1) */
    char fpath[1536];
    if (strcmp(basepath, "/") == 0) {
        snprintf(fpath, sizeof(fpath), "%s", path ? path : "/");
    } else {
        size_t bl = strlen(basepath);
        if (bl > 1 && basepath[bl - 1] == '/') basepath[bl - 1] = '\0';
        snprintf(fpath, sizeof(fpath), "%s%s", basepath, path ? path : "/");
    }

    wchar_t *whost = to_wide(host);
    wchar_t *wpath = to_wide(fpath);
    wchar_t *wmethod = to_wide(method);
    if (!whost || !wpath || !wmethod) {
        free(whost); free(wpath); free(wmethod);
        return NULL;
    }

    HINTERNET hSess = WinHttpOpen(L"c-agent/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { free(whost); free(wpath); free(wmethod); return NULL; }

    /* Honor the caller's timeout for resolve/connect/send/receive. Without this,
       WinHTTP's defaults (~30s+) mean a slow or dead peer blocks the caller —
       fatal inside the single-threaded HTTP server where every request queues. */
    DWORD to = (DWORD)(timeout_ms > 0 ? timeout_ms : 30000);
    WinHttpSetTimeouts(hSess, to, to, to, to);

    HINTERNET hConn = WinHttpConnect(hSess, whost, (INTERNET_PORT)port, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); free(whost); free(wpath); free(wmethod); return NULL; }

    DWORD dwFlags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, wmethod, wpath, NULL,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, dwFlags);
    if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); free(whost); free(wpath); free(wmethod); return NULL; }

    /* build headers (Content-Type + any extra) */
    ca_strbuf hdr;
    ca_strbuf_init(&hdr);
    if (content_type && body && *body)
        ca_strbuf_appendf(&hdr, "Content-Type: %s\r\n", content_type);
    if (extra_headers) {
        for (size_t i = 0; i < extra_headers->count; i++)
            ca_strbuf_appendf(&hdr, "%s: %s\r\n", extra_headers->items[i].key, extra_headers->items[i].val);
    }
    wchar_t *whdr = to_wide(hdr.buf && hdr.len ? hdr.buf : "");
    if (!whdr) { WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); ca_strbuf_free(&hdr); free(whost); free(wpath); free(wmethod); return NULL; }

    BOOL ok = WinHttpSendRequest(hReq, hdr.len ? whdr : WINHTTP_NO_ADDITIONAL_HEADERS,
                                 hdr.len ? (DWORD)-1L : 0,
                                 body ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA,
                                 body ? (DWORD)strlen(body) : 0,
                                 body ? (DWORD)strlen(body) : 0, 0);
    free(whdr);
    ca_strbuf_free(&hdr);
    if (!ok) { WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); free(whost); free(wpath); free(wmethod); return NULL; }

    if (WinHttpReceiveResponse(hReq, NULL) == FALSE) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        free(whost); free(wpath); free(wmethod); return NULL;
    }

    DWORD dwStatus = 0, dwSize = sizeof(dwStatus);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &dwStatus, &dwSize, NULL);

    ca_strbuf body_buf;
    ca_strbuf_init(&body_buf);
    DWORD available = 0;
    do {
        if (WinHttpQueryDataAvailable(hReq, &available) == FALSE) break;
        if (available == 0) break;
        char *tmp = (char *)malloc((size_t)available + 1);
        if (!tmp) break;
        DWORD downloaded = 0;
        if (WinHttpReadData(hReq, tmp, available, &downloaded) && downloaded > 0)
            ca_strbuf_append_n(&body_buf, tmp, (size_t)downloaded);
        free(tmp);
    } while (available > 0);

    ca_http_response *r = (ca_http_response *)calloc(1, sizeof(*r));
    r->status = (int)dwStatus;
    r->body = ca_strbuf_detach(&body_buf);
    r->body_len = r->body ? strlen(r->body) : 0;

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    free(whost); free(wpath); free(wmethod);
    return r;
}

ca_http_response *ca_http_post(const char *base_url, const char *path, const char *body,
                               const char *content_type, ca_strmap *extra_headers, int timeout_ms) {
    return do_request("POST", base_url, path, body, content_type, extra_headers, timeout_ms);
}

void ca_http_response_free(ca_http_response *r) {
    if (!r) return;
    free(r->body);
    ca_strmap_free(&r->headers);
    free(r);
}

ca_http_response *ca_http_get(const char *base_url, const char *path, ca_strmap *extra_headers,
                              int timeout_ms) {
    return do_request("GET", base_url, path, NULL, NULL, extra_headers, timeout_ms);
}

ca_http_stream *ca_http_stream_open(const char *base_url, const char *method, const char *path,
                                    const char *body, const char *content_type,
                                    ca_strmap *extra_headers, int timeout_ms) {
    ca_http_response *r = do_request(method, base_url, path, body, content_type, extra_headers, timeout_ms);
    if (!r) return NULL;
    ca_http_stream *h = (ca_http_stream *)calloc(1, sizeof(*h));
    h->body = r->body;   /* transfer ownership */
    h->len = r->body_len;
    h->pos = 0;
    h->status = r->status;
    free(r);             /* free the wrapper, keep the body buffer */
    return h;
}

int ca_http_stream_status(ca_http_stream *h) { return h ? h->status : 0; }

int ca_http_stream_read(ca_http_stream *h, char *out, size_t cap) {
    if (!h) return -1;
    size_t avail = h->len - h->pos;
    if (avail == 0) return 0;
    size_t n = avail < cap ? avail : cap;
    memcpy(out, h->body + h->pos, n);
    h->pos += n;
    return (int)n;
}

int ca_http_stream_read_line(ca_http_stream *h, char *out, size_t cap) {
    if (!h) return -1;
    size_t n = 0;
    while (h->pos < h->len && n + 1 < cap) {
        char c = h->body[h->pos++];
        if (c == '\n') break;
        if (c == '\r') continue;
        out[n++] = c;
    }
    out[n] = '\0';
    return (int)n;
}

void ca_http_stream_close(ca_http_stream *h) {
    if (!h) return;
    free(h->body);
    free(h);
}

#endif /* _WIN32 */
