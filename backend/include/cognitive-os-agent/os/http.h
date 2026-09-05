/* http.h — minimal HTTP/1.1 client over TCP.
 * Supports GET/POST, chunked transfer decoding, and streaming reads (for SSE). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "cognitive-os-agent/infra/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_http_response {
    int status;            /* HTTP status code, e.g. 200 */
    char *body;            /* decoded body (malloc'd, NUL-terminated) */
    size_t body_len;
    coa_strmap headers;
} coa_http_response;

typedef struct coa_http_stream coa_http_stream;

/* Full non-streaming POST/GET. NULL on connection/parse failure. Caller frees. */
coa_http_response *coa_http_post(const char *base_url, const char *path, const char *body,
                               const char *content_type, coa_strmap *extra_headers,
                               int timeout_ms);
coa_http_response *coa_http_get(const char *base_url, const char *path, coa_strmap *extra_headers,
                              int timeout_ms);
void coa_http_response_free(coa_http_response *r);

/* Streaming request: sends the request and parses the response head, leaving the
 * connection open so the caller can read the (de-chunked) body line by line. */
coa_http_stream *coa_http_stream_open(const char *base_url, const char *method, const char *path,
                                    const char *body, const char *content_type,
                                    coa_strmap *extra_headers, int timeout_ms);
int coa_http_stream_status(coa_http_stream *h);
/* Read one decoded line (up to \n inclusive). Returns bytes read, 0 at end-of-body, -1 on error. */
int coa_http_stream_read_line(coa_http_stream *h, char *out, size_t cap);
/* Read up to cap raw decoded bytes into out. Returns bytes, 0 at end, -1 on error. */
int coa_http_stream_read(coa_http_stream *h, char *out, size_t cap);
void coa_http_stream_close(coa_http_stream *h);

#ifdef __cplusplus
}
#endif
