/* http.h — minimal HTTP/1.1 client over TCP.
 * Supports GET/POST, chunked transfer decoding, and streaming reads (for SSE). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "cagent/infra/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_http_response {
    int status;            /* HTTP status code, e.g. 200 */
    char *body;            /* decoded body (malloc'd, NUL-terminated) */
    size_t body_len;
    ca_strmap headers;
} ca_http_response;

typedef struct ca_http_stream ca_http_stream;

/* Full non-streaming POST/GET. NULL on connection/parse failure. Caller frees. */
ca_http_response *ca_http_post(const char *base_url, const char *path, const char *body,
                               const char *content_type, ca_strmap *extra_headers,
                               int timeout_ms);
ca_http_response *ca_http_get(const char *base_url, const char *path, ca_strmap *extra_headers,
                              int timeout_ms);
void ca_http_response_free(ca_http_response *r);

/* Streaming request: sends the request and parses the response head, leaving the
 * connection open so the caller can read the (de-chunked) body line by line. */
ca_http_stream *ca_http_stream_open(const char *base_url, const char *method, const char *path,
                                    const char *body, const char *content_type,
                                    ca_strmap *extra_headers, int timeout_ms);
int ca_http_stream_status(ca_http_stream *h);
/* Read one decoded line (up to \n inclusive). Returns bytes read, 0 at end-of-body, -1 on error. */
int ca_http_stream_read_line(ca_http_stream *h, char *out, size_t cap);
/* Read up to cap raw decoded bytes into out. Returns bytes, 0 at end, -1 on error. */
int ca_http_stream_read(ca_http_stream *h, char *out, size_t cap);
void ca_http_stream_close(ca_http_stream *h);

#ifdef __cplusplus
}
#endif
