/* cron.h — scheduled tasks (定时任务): periodically submit a stored prompt to
 * the scheduler as a session-tagged chat task.
 *
 * Two schedule kinds:
 *   interval: every_sec > 0 — run every N seconds
 *   daily:    at = "HH:MM"  — run once per day at that local time
 *
 * Jobs persist to <state_root>/cron.json and survive restarts. A background
 * tick thread (cron_start) wakes every 15s, submits due jobs and schedules
 * the next run. Submissions go through the normal chat path, so results show
 * up in the named session's history. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "cognitive-os-agent.h"

typedef struct cron_mgr cron_mgr;

/* Create the manager (loads <state_root>/cron.json when present). */
cron_mgr *cron_new(scheduler *sched, const char *state_root);
void cron_free(cron_mgr *c);

/* Start the background tick thread. 0 on success. */
int cron_start(cron_mgr *c);

/* Add a job. Exactly one of every_sec (> 0) / at ("HH:MM") selects the
 * schedule kind. Returns the job id, or -1 on invalid arguments. */
long long cron_add(cron_mgr *c, const char *name, const char *prompt,
                   const char *session, int every_sec, const char *at);

/* Remove a job (0 on success, -1 unknown id). */
int cron_remove(cron_mgr *c, long long id);

/* Enable/disable without deleting (0 on success, -1 unknown id). */
int cron_set_enabled(cron_mgr *c, long long id, int enabled);

/* JSON array of jobs (caller frees): [{id,name,prompt,session,every_sec,at,
 * enabled,next_run_ms,last_run_ms}...] */
char *cron_json(const cron_mgr *c);

/* Force a due check + submission right now (used by tests; the tick thread
 * does this on its own schedule). Returns the number of jobs submitted. */
int cron_tick(cron_mgr *c);

#ifdef __cplusplus
}
#endif
