/* tasklog.h — durable task journal (任务保存/恢复).
 *
 * Appends one JSONL record per terminal task transition to
 * <state_root>/tasks.jsonl: {id,status,session,input,output,ts}. The journal
 * is the checkpoint store: unlike the in-memory scheduler table it survives
 * restarts, so finished/cancelled tasks can be listed and re-submitted
 * ("resume") later. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "cognitive-os-agent.h"

typedef struct tasklog tasklog;

tasklog *tasklog_new(const char *state_root);
void tasklog_free(tasklog *tl);

/* Append a terminal record (output and trace are capped). `trace_json` is a
 * sanitized in-memory execution timeline, serialized only at completion. */
void tasklog_record(tasklog *tl, int64_t id, const char *status, const char *session, const char *input,
                    const char *output, const char *trace_json);

/* Last `limit` records as a JSON array (caller frees). NULL tl → "[]". */
char *tasklog_json(tasklog *tl, int limit);

/* Find the newest record for `id`; sets *status, *session, *input, *output to
 * malloc'd strings when found (any may be NULL). Returns 1 on hit. */
int tasklog_find(tasklog *tl, int64_t id, char **status, char **session, char **input, char **output);

#ifdef __cplusplus
}
#endif
