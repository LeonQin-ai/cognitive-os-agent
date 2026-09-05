/* trace.h — lightweight span-based tracing / observability.
 * A bounded, thread-safe ring of spans. Spans are opened with coa_trace_begin
 * and closed with coa_trace_end; the whole buffer renders as a JSON array for
 * the console / Monitor UI. Spans carry a monotonic id so callers can close
 * them by id without holding a pointer. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_trace coa_trace;

typedef struct coa_trace_span {
    int64_t id;
    char *name;
    int64_t start_ms;
    int64_t end_ms;    /* 0 = still open */
    int status;        /* 0 = running, 1 = ok, -1 = error */
} coa_trace_span;

coa_trace *coa_trace_new(size_t capacity);
void coa_trace_free(coa_trace *t);

/* Open a span; returns a positive id (0 on failure). */
int64_t coa_trace_begin(coa_trace *t, const char *name);
/* Close a span by id. status: 1 ok, -1 error. Unknown ids are ignored. */
void coa_trace_end(coa_trace *t, int64_t id, int status);

int coa_trace_count(coa_trace *t);
/* JSON array of spans {id,name,start_ms,end_ms,duration_ms,status} (caller frees). */
char *coa_trace_json(coa_trace *t);
/* Clear all spans. */
void coa_trace_clear(coa_trace *t);

#ifdef __cplusplus
}
#endif
