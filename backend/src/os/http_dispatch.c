/* http_dispatch.c — public http_* API dispatcher over registered platform
 * hooks (see os/platform.h). The per-OS backend lives in a platform
 * directory and binds here through os_http_platform_hooks(); the first
 * call to any http_* function registers it. */
#include "os/platform.h"

#include <string.h>

static const os_http_hooks *g_http = NULL;

void os_http_register(const os_http_hooks *hooks) {
    if (hooks)
        g_http = hooks;
}

const os_http_hooks *os_http_active(void) {
    if (!g_http)
        g_http = os_http_platform_hooks();
    return g_http;
}

const char *os_http_backend_name(void) {
    const os_http_hooks *h = os_http_active();
    return h && h->name ? h->name : "";
}

http_response *http_post(const char *base_url, const char *path, const char *body,
                         const char *content_type, strmap *extra_headers, int timeout_ms) {
    const os_http_hooks *h = os_http_active();
    return h && h->post ? h->post(base_url, path, body, content_type,
                                  extra_headers, timeout_ms)
                        : NULL;
}

http_response *http_get(const char *base_url, const char *path,
                        strmap *extra_headers, int timeout_ms) {
    const os_http_hooks *h = os_http_active();
    return h && h->get ? h->get(base_url, path, extra_headers, timeout_ms) : NULL;
}

void http_response_free(http_response *r) {
    const os_http_hooks *h = os_http_active();
    if (h && h->response_free)
        h->response_free(r);
}

http_stream *http_stream_open(const char *base_url, const char *method, const char *path,
                              const char *body, const char *content_type,
                              strmap *extra_headers, int timeout_ms) {
    const os_http_hooks *h = os_http_active();
    return h && h->stream_open ? h->stream_open(base_url, method, path, body,
                                                content_type, extra_headers,
                                                timeout_ms)
                               : NULL;
}

int http_stream_status(http_stream *s) {
    const os_http_hooks *h = os_http_active();
    return h && h->stream_status ? h->stream_status(s) : -1;
}

int http_stream_read_line(http_stream *s, char *out, size_t cap) {
    const os_http_hooks *h = os_http_active();
    return h && h->stream_read_line ? h->stream_read_line(s, out, cap) : -1;
}

int http_stream_read(http_stream *s, char *out, size_t cap) {
    const os_http_hooks *h = os_http_active();
    return h && h->stream_read ? h->stream_read(s, out, cap) : -1;
}

void http_stream_close(http_stream *s) {
    const os_http_hooks *h = os_http_active();
    if (h && h->stream_close)
        h->stream_close(s);
}
