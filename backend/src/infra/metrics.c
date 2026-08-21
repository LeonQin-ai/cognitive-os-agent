#include "cagent/infra/metrics.h"
#include "cagent/infra/util.h"
#include "cagent/os/os_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum { M_COUNTER, M_GAUGE, M_HIST } metric_kind;

typedef struct {
    char *name;
    metric_kind kind;
    double value;            /* counter/gauge; for hist: count */
    double sum;
    double buckets[6];       /* 0.001, 0.01, 0.1, 1, 10, 100 */
} metric;

struct ca_metrics {
    metric *items;
    size_t count, cap;
    ca_mutex mtx;
};

ca_metrics *ca_metrics_new(void) {
    ca_metrics *m = calloc(1, sizeof(ca_metrics));
    if (m) ca_mutex_init(&m->mtx);
    return m;
}

void ca_metrics_free(ca_metrics *m) {
    if (!m) return;
    ca_mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->count; i++) free(m->items[i].name);
    free(m->items);
    ca_mutex_unlock(&m->mtx);
    ca_mutex_destroy(&m->mtx);
    free(m);
}

static metric *find_or_add(ca_metrics *m, const char *name, metric_kind kind) {
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->items[i].name, name) == 0) return &m->items[i];
    }
    if (m->count == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 16;
        m->items = realloc(m->items, cap * sizeof(metric));
        if (!m->items) return NULL;
        m->cap = cap;
    }
    metric *mt = &m->items[m->count++];
    memset(mt, 0, sizeof(*mt));
    mt->name = ca_strdup(name);
    mt->kind = kind;
    return mt;
}

void ca_metrics_inc(ca_metrics *m, const char *name) { ca_metrics_add(m, name, 1.0); }

void ca_metrics_add(ca_metrics *m, const char *name, double v) {
    ca_mutex_lock(&m->mtx);
    metric *mt = find_or_add(m, name, M_COUNTER);
    if (mt) mt->value += v;
    ca_mutex_unlock(&m->mtx);
}

void ca_metrics_set(ca_metrics *m, const char *name, double v) {
    ca_mutex_lock(&m->mtx);
    metric *mt = find_or_add(m, name, M_GAUGE);
    if (mt) mt->value = v;
    ca_mutex_unlock(&m->mtx);
}

void ca_metrics_observe(ca_metrics *m, const char *name, double v) {
    static const double bounds[6] = {0.001, 0.01, 0.1, 1, 10, 100};
    ca_mutex_lock(&m->mtx);
    metric *mt = find_or_add(m, name, M_HIST);
    if (mt) {
        mt->value += 1.0; /* count */
        mt->sum += v;
        for (int i = 0; i < 6; i++) if (v <= bounds[i]) mt->buckets[i] += 1.0;
    }
    ca_mutex_unlock(&m->mtx);
}

static const char *kind_name(metric_kind k) {
    switch (k) {
        case M_COUNTER: return "counter";
        case M_GAUGE:   return "gauge";
        default:        return "histogram";
    }
}

char *ca_metrics_render(ca_metrics *m) {
    ca_strbuf sb;
    ca_strbuf_init(&sb);
    ca_mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->count; i++) {
        metric *mt = &m->items[i];
        ca_strbuf_appendf(&sb, "# TYPE cagent_%s %s\ncagent_%s %g\n",
                          mt->name, kind_name(mt->kind), mt->name, mt->value);
        if (mt->kind == M_HIST) {
            static const double bounds[6] = {0.001, 0.01, 0.1, 1, 10, 100};
            for (int b = 0; b < 6; b++)
                ca_strbuf_appendf(&sb, "cagent_%s_bucket{le=\"%g\"} %g\n",
                                  mt->name, bounds[b], mt->buckets[b]);
            ca_strbuf_appendf(&sb, "cagent_%s_bucket{le=\"+Inf\"} %g\n", mt->name, mt->value);
            ca_strbuf_appendf(&sb, "cagent_%s_sum %g\n", mt->name, mt->sum);
        }
    }
    ca_mutex_unlock(&m->mtx);
    return ca_strbuf_detach(&sb);
}
