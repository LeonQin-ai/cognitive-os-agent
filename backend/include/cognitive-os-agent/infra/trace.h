/* trace.h — lightweight span-based tracing / observability.
 * A bounded, thread-safe ring of spans. Spans are opened with trace_begin
 * and closed with trace_end; the whole buffer renders as a JSON array for
 * the console / Monitor UI. Spans carry a monotonic id so callers can close
 * them by id without holding a pointer. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct trace trace;

typedef struct trace_span {
    int64_t id;
    char *name;
    int64_t start_ms;
    int64_t end_ms; /* 0 = still open */
    int status;     /* 0 = running, 1 = ok, -1 = error */
} trace_span;

trace *trace_new(size_t capacity);
void trace_free(trace *t);

/* Open a span; returns a positive id (0 on failure). */
int64_t trace_begin(trace *t, const char *name);
/* Close a span by id. status: 1 ok, -1 error. Unknown ids are ignored. */
void trace_end(trace *t, int64_t id, int status);

int trace_count(trace *t);
/* JSON array of spans {id,name,start_ms,end_ms,duration_ms,status} (caller frees). */
char *trace_json(trace *t);
/* Clear all spans. */
void trace_clear(trace *t);

#ifdef __cplusplus
}
#endif
