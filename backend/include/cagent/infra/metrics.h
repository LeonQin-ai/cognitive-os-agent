/* metrics.h — counters, gauges, histograms rendered as Prometheus text format.
 * Thread-safe. Names are flat strings like "tasks.completed". */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_metrics ca_metrics;

ca_metrics *ca_metrics_new(void);
void ca_metrics_free(ca_metrics *m);

void ca_metrics_inc(ca_metrics *m, const char *name);              /* counter += 1 */
void ca_metrics_add(ca_metrics *m, const char *name, double v);    /* counter += v */
void ca_metrics_set(ca_metrics *m, const char *name, double v);    /* gauge = v */
void ca_metrics_observe(ca_metrics *m, const char *name, double v);/* histogram sample */

/* Render as Prometheus text. Caller frees returned string. */
char *ca_metrics_render(ca_metrics *m);

#ifdef __cplusplus
}
#endif
