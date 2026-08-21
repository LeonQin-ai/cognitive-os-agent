/* episode.h — episodic memory: a history of (task -> result) episodes. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_episodic ca_episodic;

ca_episodic *ca_episodic_new(void);
void ca_episodic_free(ca_episodic *e);

/* Record a completed episode (both copied). */
void ca_episodic_add(ca_episodic *e, const char *task, const char *result);
int ca_episodic_count(ca_episodic *e);

/* Borrowed task/result of the i-th episode (0 = oldest). Do not free. */
const char *ca_episodic_task(ca_episodic *e, int i);
const char *ca_episodic_result(ca_episodic *e, int i);

/* All episodes as a JSON array of {task,result} (malloc'd; caller frees). */
char *ca_episodic_json(ca_episodic *e);

#ifdef __cplusplus
}
#endif
