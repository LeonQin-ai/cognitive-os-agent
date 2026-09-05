/* sse.h — Server-Sent-Events reader over the HTTP stream client. */
#pragma once
#include <stddef.h>
#include "cognitive-os-agent/infra/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_sse coa_sse;

/* Send the request and begin reading the SSE stream. NULL on connection/head failure. */
coa_sse *coa_sse_start(const char *base_url, const char *path, const char *body,
                     const char *content_type, coa_strmap *extra_headers, int timeout_ms);
int coa_sse_status(const coa_sse *s);

/* Read the next "data:" payload into out (NUL-terminated).
 * Returns 1 = data, 0 = stream end, -1 = error. */
int coa_sse_next(coa_sse *s, char *out, size_t cap);
void coa_sse_close(coa_sse *s);

#ifdef __cplusplus
}
#endif
