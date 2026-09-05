/* trace.h — lightweight span-based tracing / observability.
 * A bounded, thread-safe ring of spans. Spans are opened with ca_trace_begin
 * and closed with ca_trace_end; the whole buffer renders as a JSON array for
 * the console / Monitor UI. Spans carry a monotonic id so callers can close
 * them by id without holding a pointer. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_trace ca_trace;

typedef struct ca_trace_span {
    int64_t id;
    char *name;
    int64_t start_ms;
    int64_t end_ms;    /* 0 = still open */
    int status;        /* 0 = running, 1 = ok, -1 = error */
} ca_trace_span;

ca_trace *ca_trace_new(size_t capacity);
void ca_trace_free(ca_trace *t);

/* Open a span; returns a positive id (0 on failure). */
int64_t ca_trace_begin(ca_trace *t, const char *name);
/* Close a span by id. status: 1 ok, -1 error. Unknown ids are ignored. */
void ca_trace_end(ca_trace *t, int64_t id, int status);

int ca_trace_count(ca_trace *t);
/* JSON array of spans {id,name,start_ms,end_ms,duration_ms,status} (caller frees). */
char *ca_trace_json(ca_trace *t);
/* Clear all spans. */
void ca_trace_clear(ca_trace *t);

#ifdef __cplusplus
}
#endif
