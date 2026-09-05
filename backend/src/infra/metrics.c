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
    double value;            /* counter/gauge; for hist: count */
    double sum;
    double buckets[6];       /* 0.001, 0.01, 0.1, 1, 10, 100 */
} metric;

struct coa_metrics {
    metric *items;
    size_t count, cap;
    coa_mutex mtx;
};

coa_metrics *coa_metrics_new(void) {
    coa_metrics *m = calloc(1, sizeof(coa_metrics));
    if (m) coa_mutex_init(&m->mtx);
    return m;
}

void coa_metrics_free(coa_metrics *m) {
    if (!m) return;
    coa_mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->count; i++) free(m->items[i].name);
    free(m->items);
    coa_mutex_unlock(&m->mtx);
    coa_mutex_destroy(&m->mtx);
    free(m);
}

static metric *find_or_add(coa_metrics *m, const char *name, metric_kind kind) {
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
    mt->name = coa_strdup(name);
    mt->kind = kind;
    return mt;
}

void coa_metrics_inc(coa_metrics *m, const char *name) { coa_metrics_add(m, name, 1.0); }

void coa_metrics_add(coa_metrics *m, const char *name, double v) {
    coa_mutex_lock(&m->mtx);
    metric *mt = find_or_add(m, name, M_COUNTER);
    if (mt) mt->value += v;
    coa_mutex_unlock(&m->mtx);
}

void coa_metrics_set(coa_metrics *m, const char *name, double v) {
    coa_mutex_lock(&m->mtx);
    metric *mt = find_or_add(m, name, M_GAUGE);
    if (mt) mt->value = v;
    coa_mutex_unlock(&m->mtx);
}

void coa_metrics_observe(coa_metrics *m, const char *name, double v) {
    static const double bounds[6] = {0.001, 0.01, 0.1, 1, 10, 100};
    coa_mutex_lock(&m->mtx);
    metric *mt = find_or_add(m, name, M_HIST);
    if (mt) {
        mt->value += 1.0; /* count */
        mt->sum += v;
        for (int i = 0; i < 6; i++) if (v <= bounds[i]) mt->buckets[i] += 1.0;
    }
    coa_mutex_unlock(&m->mtx);
}

static const char *kind_name(metric_kind k) {
    switch (k) {
        case M_COUNTER: return "counter";
        case M_GAUGE:   return "gauge";
        default:        return "histogram";
    }
}

char *coa_metrics_render(coa_metrics *m) {
    coa_strbuf sb;
    coa_strbuf_init(&sb);
    coa_mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->count; i++) {
        metric *mt = &m->items[i];
        coa_strbuf_appendf(&sb, "# TYPE coa_%s %s\ncagent_%s %g\n",
                          mt->name, kind_name(mt->kind), mt->name, mt->value);
        if (mt->kind == M_HIST) {
            static const double bounds[6] = {0.001, 0.01, 0.1, 1, 10, 100};
            for (int b = 0; b < 6; b++)
                coa_strbuf_appendf(&sb, "coa_%s_bucket{le=\"%g\"} %g\n",
                                  mt->name, bounds[b], mt->buckets[b]);
            coa_strbuf_appendf(&sb, "coa_%s_bucket{le=\"+Inf\"} %g\n", mt->name, mt->value);
            coa_strbuf_appendf(&sb, "coa_%s_sum %g\n", mt->name, mt->sum);
        }
    }
    coa_mutex_unlock(&m->mtx);
    return coa_strbuf_detach(&sb);
}
