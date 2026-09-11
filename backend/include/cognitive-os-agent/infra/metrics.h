/* metrics.h — counters, gauges, histograms rendered as Prometheus text format.
 * Thread-safe. Names are flat strings like "tasks.completed". */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct metrics metrics;

metrics *metrics_new(void);
void metrics_free(metrics *m);

void metrics_inc(metrics *m, const char *name);               /* counter += 1 */
void metrics_add(metrics *m, const char *name, double v);     /* counter += v */
void metrics_set(metrics *m, const char *name, double v);     /* gauge = v */

/* Render as Prometheus text. Caller frees returned string. */
char *metrics_render(metrics *m);

#ifdef __cplusplus
}
#endif
