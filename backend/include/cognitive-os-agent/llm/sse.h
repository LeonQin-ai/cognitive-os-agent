/* sse.h — Server-Sent-Events reader over the HTTP stream client. */
#pragma once
#include <stddef.h>
#include "cagent/infra/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_sse ca_sse;

/* Send the request and begin reading the SSE stream. NULL on connection/head failure. */
ca_sse *ca_sse_start(const char *base_url, const char *path, const char *body,
                     const char *content_type, ca_strmap *extra_headers, int timeout_ms);
int ca_sse_status(const ca_sse *s);

/* Read the next "data:" payload into out (NUL-terminated).
 * Returns 1 = data, 0 = stream end, -1 = error. */
int ca_sse_next(ca_sse *s, char *out, size_t cap);
void ca_sse_close(ca_sse *s);

#ifdef __cplusplus
}
#endif
