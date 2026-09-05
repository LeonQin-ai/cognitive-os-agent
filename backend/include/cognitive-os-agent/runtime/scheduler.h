/* scheduler.h — priority task scheduler with a worker pool.
 * Lower priority value = higher precedence. Supports cooperative cancellation
 * and timeout via coa_task_should_abort (checked between actions by the runner). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum coa_task_status {
    COA_TS_QUEUED = 0,
    COA_TS_RUNNING = 1,
    COA_TS_DONE = 2,
    COA_TS_FAILED = 3,
    COA_TS_CANCELLED = 4,
    COA_TS_TIMEOUT = 5,
} coa_task_status;

typedef struct coa_task {
    int64_t id;
    int priority;              /* lower = higher priority */
    int64_t timeout_ms;        /* 0 = none */
    int64_t created_ms;
    int64_t started_ms;
    int64_t finished_ms;
    volatile int cancel_flag;  /* set by cancel() */
    int timed_out;             /* set when deadline passed */
    coa_task_status status;
    char *input;               /* task description / prompt */
    char *output;              /* set by runner */
    void *userdata;
    /* internal (managed by scheduler.c): coroutine handle + owning scheduler */
    void *coro;                /* coa_coro* running this task, or NULL */
    void *sched;               /* coa_scheduler* back-pointer for the trampoline */
} coa_task;

typedef struct coa_scheduler coa_scheduler;

/* Runs a task; t->output / t->status may be set by the runner. */
typedef void (*coa_task_runner)(coa_task *t, coa_scheduler *s, void *worker_ud);
/* Called (outside the scheduler lock) when a task finishes. */
typedef void (*coa_task_completion)(coa_task *t, void *ud);

coa_scheduler *coa_scheduler_new(int workers, coa_task_runner runner, void *worker_ud);
void coa_scheduler_free(coa_scheduler *s);

/* Enqueue a task. Returns its id, or -1 on failure. */
int64_t coa_scheduler_submit(coa_scheduler *s, int priority, const char *input,
                            void *userdata, int64_t timeout_ms);
/* Request cancellation. Returns 1 if the task was found, 0 otherwise. */
int coa_scheduler_cancel(coa_scheduler *s, int64_t id);

/* Look up a task by id (borrowed pointer, valid until scheduler_free). */
coa_task *coa_scheduler_get(coa_scheduler *s, int64_t id);

int coa_scheduler_active(coa_scheduler *s);               /* queued + running */
int coa_scheduler_total(coa_scheduler *s);

void coa_scheduler_set_completion_cb(coa_scheduler *s, coa_task_completion cb, void *ud);

/* Wait until no tasks are queued or running. Returns 0 ok, -1 timeout. */
int coa_scheduler_wait_idle(coa_scheduler *s, int timeout_ms);
/* Stop accepting work and join worker threads. Returns 0 ok, -1 timeout. */
int coa_scheduler_shutdown(coa_scheduler *s, int timeout_ms);

/* Cooperative abort check for runners: true if cancelled or past deadline. */
int coa_task_should_abort(const coa_task *t);

/* Cooperative yield for runners: voluntarily give up the worker thread so the
 * scheduler can run another task. No-op when not inside a scheduler coroutine. */
void coa_scheduler_yield(void);

#ifdef __cplusplus
}
#endif
