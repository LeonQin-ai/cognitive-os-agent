/* metrics.h — counters, gauges, histograms rendered as Prometheus text format.
 * Thread-safe. Names are flat strings like "tasks.completed". */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_metrics coa_metrics;

coa_metrics *coa_metrics_new(void);
void coa_metrics_free(coa_metrics *m);

void coa_metrics_inc(coa_metrics *m, const char *name);              /* counter += 1 */
void coa_metrics_add(coa_metrics *m, const char *name, double v);    /* counter += v */
void coa_metrics_set(coa_metrics *m, const char *name, double v);    /* gauge = v */
void coa_metrics_observe(coa_metrics *m, const char *name, double v);/* histogram sample */

/* Render as Prometheus text. Caller frees returned string. */
char *coa_metrics_render(coa_metrics *m);

#ifdef __cplusplus
}
#endif
