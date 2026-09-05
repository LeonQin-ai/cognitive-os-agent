/* task.h — standalone task lifecycle helpers.
 * A coa_task is the scheduler's unit of work (defined in scheduler.h); this
 * module provides lifecycle utilities (create / free / transition / JSON) so
 * task management can be reasoned about independently of the worker pool. */
#pragma once
#include <stdint.h>
#include "cognitive-os-agent/runtime/scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate and initialize a task in QUEUED state (input copied). */
coa_task *coa_task_new(int64_t id, int priority, const char *input, int64_t timeout_ms);
void coa_task_free(coa_task *t);

/* Move a task to `st`, recording started_ms / finished_ms as appropriate.
 * now_ms == 0 uses the current time. */
void coa_task_transition(coa_task *t, coa_task_status st, int64_t now_ms);

/* Human-readable status name. */
const char *coa_task_status_name(coa_task_status st);

/* Serialize a task as a JSON object {id,priority,status,input,output,timeout_ms}
 * (malloc'd; caller frees). */
char *coa_task_to_json(const coa_task *t);

#ifdef __cplusplus
}
#endif
