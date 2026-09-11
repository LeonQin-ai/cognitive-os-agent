/* http.h — minimal HTTP/1.1 client over TCP.
 * Supports GET/POST, chunked transfer decoding, and streaming reads (for SSE). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "cognitive-os-agent/infra/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct http_response {
    int status; /* HTTP status code, e.g. 200 */
    char *body; /* decoded body (malloc'd, NUL-terminated) */
    size_t body_len;
    strmap headers;
} http_response;

typedef struct http_stream http_stream;

/* Full non-streaming POST/GET. NULL on connection/parse failure. Caller frees. */
http_response *http_post(const char *base_url, const char *path, const char *body, const char *content_type,
                                 strmap *extra_headers, int timeout_ms);
http_response *http_get(const char *base_url, const char *path, strmap *extra_headers, int timeout_ms);
void http_response_free(http_response *r);

/* Streaming request: sends the request and parses the response head, leaving the
 * connection open so the caller can read the (de-chunked) body line by line. */
http_stream *http_stream_open(const char *base_url, const char *method, const char *path, const char *body,
                                      const char *content_type, strmap *extra_headers, int timeout_ms);
int http_stream_status(http_stream *h);
/* Read one decoded line (up to \n inclusive). Returns bytes read, 0 at end-of-body, -1 on error. */
int http_stream_read_line(http_stream *h, char *out, size_t cap);
/* Read up to cap raw decoded bytes into out. Returns bytes, 0 at end, -1 on error. */
int http_stream_read(http_stream *h, char *out, size_t cap);
void http_stream_close(http_stream *h);

#ifdef __cplusplus
}
#endif
