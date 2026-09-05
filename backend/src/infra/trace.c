/* trace.c — bounded, thread-safe span ring. */
#include "cognitive-os-agent/infra/trace.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct coa_trace {
    coa_mutex mtx;
    coa_trace_span *spans;
    size_t count, cap;
    size_t next;      /* insertion slot (ring) */
    int64_t next_id;
};

coa_trace *coa_trace_new(size_t capacity) {
    if (capacity == 0) capacity = 256;
    coa_trace *t = (coa_trace *)calloc(1, sizeof(coa_trace));
    if (!t) return NULL;
    t->spans = (coa_trace_span *)calloc(capacity, sizeof(coa_trace_span));
    if (!t->spans) { free(t); return NULL; }
    t->cap = capacity;
    t->next_id = 1;
    coa_mutex_init(&t->mtx);
    return t;
}

void coa_trace_free(coa_trace *t) {
    if (!t) return;
    coa_mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++) free(t->spans[i].name);
    free(t->spans);
    coa_mutex_unlock(&t->mtx);
    coa_mutex_destroy(&t->mtx);
    free(t);
}

int64_t coa_trace_begin(coa_trace *t, const char *name) {
    if (!t || !name) return 0;
    coa_mutex_lock(&t->mtx);
    coa_trace_span *s = &t->spans[t->next];
    if (t->count < t->cap) t->count++;
    free(s->name);
    s->name = coa_strdup(name);
    s->id = t->next_id++;
    s->start_ms = coa_time_now_ms();
    s->end_ms = 0;
    s->status = 0;
    t->next = (t->next + 1) % t->cap;
    int64_t id = s->id;
    coa_mutex_unlock(&t->mtx);
    return id;
}

void coa_trace_end(coa_trace *t, int64_t id, int status) {
    if (!t) return;
    coa_mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++) {
        coa_trace_span *s = &t->spans[(t->next + t->cap - 1 - i) % t->cap];
        if (s->id == id && s->end_ms == 0) {
            s->end_ms = coa_time_now_ms();
            s->status = status;
            break;
        }
    }
    coa_mutex_unlock(&t->mtx);
}

int coa_trace_count(coa_trace *t) {
    if (!t) return 0;
    coa_mutex_lock(&t->mtx);
    int n = (int)t->count;
    coa_mutex_unlock(&t->mtx);
    return n;
}

void coa_trace_clear(coa_trace *t) {
    if (!t) return;
    coa_mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++) free(t->spans[i].name);
    t->count = 0;
    t->next = 0;
    coa_mutex_unlock(&t->mtx);
}

char *coa_trace_json(coa_trace *t) {
    cJSON *arr = cJSON_CreateArray();
    if (!t) return cJSON_PrintUnformatted(arr);
    coa_mutex_lock(&t->mtx);
    for (size_t i = 0; i < t->count; i++) {
        /* oldest first: slots wrap, so iterate from (next - count) forward */
        size_t idx = (t->next + t->cap - t->count + i) % t->cap;
        coa_trace_span *s = &t->spans[idx];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", (double)s->id);
        cJSON_AddStringToObject(o, "name", s->name ? s->name : "");
        cJSON_AddNumberToObject(o, "start_ms", (double)s->start_ms);
        cJSON_AddNumberToObject(o, "end_ms", (double)s->end_ms);
        cJSON_AddNumberToObject(o, "duration_ms", (double)(s->end_ms ? s->end_ms - s->start_ms : 0));
        cJSON_AddNumberToObject(o, "status", s->status);
        cJSON_AddItemToArray(arr, o);
    }
    coa_mutex_unlock(&t->mtx);
    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return js;
}
