/* scheduler.h — priority task scheduler with a worker pool.
 * Lower priority value = higher precedence. Supports cooperative cancellation
 * and timeout via ca_task_should_abort (checked between actions by the runner). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ca_task_status {
    CA_TS_QUEUED = 0,
    CA_TS_RUNNING = 1,
    CA_TS_DONE = 2,
    CA_TS_FAILED = 3,
    CA_TS_CANCELLED = 4,
    CA_TS_TIMEOUT = 5,
} ca_task_status;

typedef struct ca_task {
    int64_t id;
    int priority;              /* lower = higher priority */
    int64_t timeout_ms;        /* 0 = none */
    int64_t created_ms;
    int64_t started_ms;
    int64_t finished_ms;
    volatile int cancel_flag;  /* set by cancel() */
    int timed_out;             /* set when deadline passed */
    ca_task_status status;
    char *input;               /* task description / prompt */
    char *output;              /* set by runner */
    void *userdata;
    /* internal (managed by scheduler.c): coroutine handle + owning scheduler */
    void *coro;                /* ca_coro* running this task, or NULL */
    void *sched;               /* ca_scheduler* back-pointer for the trampoline */
} ca_task;

typedef struct ca_scheduler ca_scheduler;

/* Runs a task; t->output / t->status may be set by the runner. */
typedef void (*ca_task_runner)(ca_task *t, ca_scheduler *s, void *worker_ud);
/* Called (outside the scheduler lock) when a task finishes. */
typedef void (*ca_task_completion)(ca_task *t, void *ud);

ca_scheduler *ca_scheduler_new(int workers, ca_task_runner runner, void *worker_ud);
void ca_scheduler_free(ca_scheduler *s);

/* Enqueue a task. Returns its id, or -1 on failure. */
int64_t ca_scheduler_submit(ca_scheduler *s, int priority, const char *input,
                            void *userdata, int64_t timeout_ms);
/* Request cancellation. Returns 1 if the task was found, 0 otherwise. */
int ca_scheduler_cancel(ca_scheduler *s, int64_t id);

/* Look up a task by id (borrowed pointer, valid until scheduler_free). */
ca_task *ca_scheduler_get(ca_scheduler *s, int64_t id);

int ca_scheduler_active(ca_scheduler *s);               /* queued + running */
int ca_scheduler_total(ca_scheduler *s);

void ca_scheduler_set_completion_cb(ca_scheduler *s, ca_task_completion cb, void *ud);

/* Wait until no tasks are queued or running. Returns 0 ok, -1 timeout. */
int ca_scheduler_wait_idle(ca_scheduler *s, int timeout_ms);
/* Stop accepting work and join worker threads. Returns 0 ok, -1 timeout. */
int ca_scheduler_shutdown(ca_scheduler *s, int timeout_ms);

/* Cooperative abort check for runners: true if cancelled or past deadline. */
int ca_task_should_abort(const ca_task *t);

/* Cooperative yield for runners: voluntarily give up the worker thread so the
 * scheduler can run another task. No-op when not inside a scheduler coroutine. */
void ca_scheduler_yield(void);

#ifdef __cplusplus
}
#endif
