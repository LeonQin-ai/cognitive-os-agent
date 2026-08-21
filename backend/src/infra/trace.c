/* trace.c — bounded, thread-safe span ring. */
#include "cagent/infra/trace.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/os_time.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct ca_trace {
    ca_mutex mtx;
    ca_trace_span *spans;
    size_t count, cap;
    size_t next;      /* insertion slot (ring) */
    int64_t next_id;
};

ca_trace *ca_trace_new(size_t capacity) {
    if (capacity == 0) capacity = 256;
    ca_trace *t = (ca_trace *)calloc(1, sizeof(ca_trace));
    if (!t) return NULL;
    t->spans = (ca_trace_span *)calloc(capacity, sizeof(ca_trace_span));
    if (!t->spans) { free(t); return NULL; }
    t->cap = capacity;
    t->next_id = 1;
    ca_mutex_init(&t->mtx);
    return t;
}

void ca_trace_free(ca_trace *t) {
    if (!t) return;
    ca_mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++) free(t->spans[i].name);
    free(t->spans);
    ca_mutex_unlock(&t->mtx);
    ca_mutex_destroy(&t->mtx);
    free(t);
}

int64_t ca_trace_begin(ca_trace *t, const char *name) {
    if (!t || !name) return 0;
    ca_mutex_lock(&t->mtx);
    ca_trace_span *s = &t->spans[t->next];
    if (t->count < t->cap) t->count++;
    free(s->name);
    s->name = ca_strdup(name);
    s->id = t->next_id++;
    s->start_ms = ca_time_now_ms();
    s->end_ms = 0;
    s->status = 0;
    t->next = (t->next + 1) % t->cap;
    int64_t id = s->id;
    ca_mutex_unlock(&t->mtx);
    return id;
}

void ca_trace_end(ca_trace *t, int64_t id, int status) {
    if (!t) return;
    ca_mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++) {
        ca_trace_span *s = &t->spans[(t->next + t->cap - 1 - i) % t->cap];
        if (s->id == id && s->end_ms == 0) {
            s->end_ms = ca_time_now_ms();
            s->status = status;
            break;
        }
    }
    ca_mutex_unlock(&t->mtx);
}

int ca_trace_count(ca_trace *t) {
    if (!t) return 0;
    ca_mutex_lock(&t->mtx);
    int n = (int)t->count;
    ca_mutex_unlock(&t->mtx);
    return n;
}

void ca_trace_clear(ca_trace *t) {
    if (!t) return;
    ca_mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++) free(t->spans[i].name);
    t->count = 0;
    t->next = 0;
    ca_mutex_unlock(&t->mtx);
}

char *ca_trace_json(ca_trace *t) {
    cJSON *arr = cJSON_CreateArray();
    if (!t) return cJSON_PrintUnformatted(arr);
    ca_mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++) {
        /* oldest first: slots wrap, so iterate from (next - count) forward */
        size_t idx = (t->next + t->cap - t->count + i) % t->cap;
        ca_trace_span *s = &t->spans[idx];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", (double)s->id);
        cJSON_AddStringToObject(o, "name", s->name ? s->name : "");
        cJSON_AddNumberToObject(o, "start_ms", (double)s->start_ms);
        cJSON_AddNumberToObject(o, "end_ms", (double)s->end_ms);
        cJSON_AddNumberToObject(o, "duration_ms", (double)(s->end_ms ? s->end_ms - s->start_ms : 0));
        cJSON_AddNumberToObject(o, "status", s->status);
        cJSON_AddItemToArray(arr, o);
    }
    ca_mutex_unlock(&t->mtx);
    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return js;
}
