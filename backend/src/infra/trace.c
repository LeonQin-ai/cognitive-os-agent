/* trace.c — bounded, thread-safe span ring. */
#include "cognitive-os-agent/infra/trace.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct trace {
    mutex_t mtx;
    trace_span *spans;
    size_t count, cap;
    size_t next; /* insertion slot (ring) */
    int64_t next_id;
};

trace *trace_new(size_t capacity) {
    trace *t;

    if (capacity == 0)
        capacity = 256;
    t = (trace *)calloc(1, sizeof(trace));
    if (!t)
        return NULL;
    t->spans = (trace_span *)calloc(capacity, sizeof(trace_span));
    if (!t->spans) {
        free(t);
        return NULL;
    }

    t->cap = capacity;
    t->next_id = 1;
    mutex_init(&t->mtx);
    return t;
}

void trace_free(trace *t) {
    if (!t)
        return;
    mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++)
        free(t->spans[i].name);
    free(t->spans);
    mutex_unlock(&t->mtx);
    mutex_destroy(&t->mtx);
    free(t);
}

int64_t trace_begin(trace *t, const char *name) {
    trace_span *s;
    int64_t id;

    if (!t || !name)
        return 0;
    mutex_lock(&t->mtx);
    s = &t->spans[t->next];
    if (t->count < t->cap)
        t->count++;
    free(s->name);
    s->name = xstrdup(name);
    s->id = t->next_id++;
    s->start_ms = time_now_ms();
    s->end_ms = 0;
    s->status = 0;
    t->next = (t->next + 1) % t->cap;
    id = s->id;
    mutex_unlock(&t->mtx);
    return id;
}

void trace_end(trace *t, int64_t id, int status) {
    if (!t)
        return;
    mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++) {
        trace_span *s = &t->spans[(t->next + t->cap - 1 - i) % t->cap];
        if (s->id == id && s->end_ms == 0) {
            s->end_ms = time_now_ms();
            s->status = status;
            break;
        }
    }

    mutex_unlock(&t->mtx);
}

int trace_count(trace *t) {
    int n;

    if (!t)
        return 0;
    mutex_lock(&t->mtx);
    n = (int)t->count;
    mutex_unlock(&t->mtx);
    return n;
}

void trace_clear(trace *t) {
    if (!t)
        return;
    mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++)
        free(t->spans[i].name);
    t->count = 0;
    t->next = 0;
    mutex_unlock(&t->mtx);
}

char *trace_json(trace *t) {
    cJSON *arr = cJSON_CreateArray();
    char *js;

    if (!t)
        return cJSON_PrintUnformatted(arr);
    mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++) {
        /* oldest first: slots wrap, so iterate from (next - count) forward */
        size_t idx = (t->next + t->cap - t->count + i) % t->cap;
        trace_span *s = &t->spans[idx];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", (double)s->id);
        cJSON_AddStringToObject(o, "name", s->name ? s->name : "");
        cJSON_AddNumberToObject(o, "start_ms", (double)s->start_ms);
        cJSON_AddNumberToObject(o, "end_ms", (double)s->end_ms);
        cJSON_AddNumberToObject(o, "duration_ms", (double)(s->end_ms ? s->end_ms - s->start_ms : 0));
        cJSON_AddNumberToObject(o, "status", s->status);
        cJSON_AddItemToArray(arr, o);
    }

    mutex_unlock(&t->mtx);
    js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return js;
}
