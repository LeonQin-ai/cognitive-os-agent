#include "cognitive-os-agent/infra/metrics.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum { M_COUNTER, M_GAUGE, M_HIST } metric_kind;

typedef struct {
    char *name;
    metric_kind kind;
    double value; /* counter/gauge; for hist: count */
    double sum;
    double buckets[6]; /* 0.001, 0.01, 0.1, 1, 10, 100 */
} metric;

struct metrics {
    metric *items;
    size_t count, cap;
    mutex_t mtx;
};

metrics *metrics_new(void) {
    metrics *m = calloc(1, sizeof(metrics));
    if (m)
        mutex_init(&m->mtx);
    return m;
}

void metrics_free(metrics *m) {
    if (!m)
        return;
    mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->count; i++)
        free(m->items[i].name);
    free(m->items);
    mutex_unlock(&m->mtx);
    mutex_destroy(&m->mtx);
    free(m);
}

static metric *find_or_add(metrics *m, const char *name, metric_kind kind) {
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->items[i].name, name) == 0)
            return &m->items[i];
    }

    if (m->count == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 16;
        m->items = realloc(m->items, cap * sizeof(metric));
        if (!m->items)
            return NULL;
        m->cap = cap;
    }

    metric *mt = &m->items[m->count++];
    memset(mt, 0, sizeof(*mt));
    mt->name = xstrdup(name);
    mt->kind = kind;
    return mt;
}

void metrics_inc(metrics *m, const char *name) {
    metrics_add(m, name, 1.0);
}

void metrics_add(metrics *m, const char *name, double v) {
    mutex_lock(&m->mtx);
    metric *mt = find_or_add(m, name, M_COUNTER);
    if (mt)
        mt->value += v;
    mutex_unlock(&m->mtx);
}

void metrics_set(metrics *m, const char *name, double v) {
    mutex_lock(&m->mtx);
    metric *mt = find_or_add(m, name, M_GAUGE);
    if (mt)
        mt->value = v;
    mutex_unlock(&m->mtx);
}

static const char *kind_name(metric_kind k) {
    switch (k) {
    case M_COUNTER:
        return "counter";
    case M_GAUGE:
        return "gauge";
    default:
        return "histogram";
    }
}

char *metrics_render(metrics *m) {
    strbuf sb;
    strbuf_init(&sb);
    mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->count; i++) {
        metric *mt = &m->items[i];
        strbuf_appendf(&sb, "# TYPE coa_%s %s\ncagent_%s %g\n", mt->name, kind_name(mt->kind), mt->name, mt->value);
        if (mt->kind == M_HIST) {
            static const double bounds[6] = {0.001, 0.01, 0.1, 1, 10, 100};
            for (int b = 0; b < 6; b++)
                strbuf_appendf(&sb, "coa_%s_bucket{le=\"%g\"} %g\n", mt->name, bounds[b], mt->buckets[b]);
            strbuf_appendf(&sb, "coa_%s_bucket{le=\"+Inf\"} %g\n", mt->name, mt->value);
            strbuf_appendf(&sb, "coa_%s_sum %g\n", mt->name, mt->sum);
        }
    }

    mutex_unlock(&m->mtx);
    return strbuf_detach(&sb);
}
