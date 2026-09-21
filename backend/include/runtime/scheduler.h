/* scheduler.h — priority task scheduler with a worker pool.
 * Lower priority value = higher precedence. Supports cooperative cancellation
 * and timeout via task_should_abort (checked between actions by the runner). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "os/os_thread.h"
#ifndef __cplusplus
#include <stdatomic.h>
#define TASK_ATOMIC(T) _Atomic(T)
#else
#define TASK_ATOMIC(T) T
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum task_status {
    TS_QUEUED = 0,
    TS_RUNNING = 1,
    TS_DONE = 2,
    TS_FAILED = 3,
    TS_CANCELLED = 4,
    TS_TIMEOUT = 5,
} task_status;

typedef struct task {
    int64_t id;
    int priority;       /* lower = higher priority */
    int64_t timeout_ms; /* 0 = none */
    int64_t created_ms;
    int64_t started_ms;
    int64_t finished_ms;
    TASK_ATOMIC(int) cancel_flag; /* set by cancel() */
    int timed_out;            /* set when deadline passed */
    TASK_ATOMIC(task_status) status;
    char *input;  /* task description / prompt */
    char *output; /* set by runner */
    mutex_t progress_mtx;
    char *progress_json; /* immutable snapshot, protected by progress_mtx */
    /* Sanitized execution timeline kept in memory while the task runs. It is
     * written to the durable journal only after the terminal transition. */
    char *trace_json;
    char *pending_input; /* steering messages, protected by progress_mtx */
    int updates_closed;
    unsigned update_count;
    char *tag;    /* optional routing tag (e.g. chat session id) */
    int thinking_mode; /* user-selected deep-thinking preference for this chat task */
    void *userdata;
    /* internal (managed by scheduler.c): coroutine handle + owning scheduler */
    void *coro;  /* coro* running this task, or NULL */
    void *sched; /* scheduler* back-pointer for the trampoline */
} task;
#undef TASK_ATOMIC

typedef struct scheduler scheduler;

/* Runs a task; t->output / t->status may be set by the runner. */
typedef void (*task_runner)(task *t, scheduler *s, void *worker_ud);
/* Called (outside the scheduler lock) when a task finishes. */
typedef void (*task_completion)(task *t, void *ud);

scheduler *scheduler_new(int workers, task_runner runner, void *worker_ud);
void scheduler_free(scheduler *s);

/* Enqueue a task. Returns its id, or -1 on failure. */
int64_t scheduler_submit(scheduler *s, int priority, const char *input, void *userdata, int64_t timeout_ms);
/* Same, with an owned routing tag (NULL = none). The tag is freed with the
 * task; runners read it as t->tag. */
int64_t scheduler_submit_tag(scheduler *s, int priority, const char *input, void *userdata, int64_t timeout_ms,
                                 const char *tag);
/* Tagged submission with a task-local reasoning preference. Existing task
 * submitters keep the default (normal) mode through scheduler_submit_tag. */
int64_t scheduler_submit_tag_mode(scheduler *s, int priority, const char *input, void *userdata, int64_t timeout_ms,
                                  const char *tag, int thinking_mode);

/* Look up a task by id (borrowed pointer, valid until scheduler_free). */
task *scheduler_get(scheduler *s, int64_t id);

int scheduler_total(scheduler *s);
void task_set_progress(task *t, const char *json);
char *task_progress_copy(task *t);
/* Append one safe, user-observable progress event to the in-memory timeline.
 * Raw model plans, prompts, tool arguments, and tool output are deliberately
 * excluded; callers own `json` and may free it after this call. */
void task_trace_add(task *t, const char *json);
char *task_trace_copy(task *t);
int task_add_message(task *t, const char *message); /* >0 revision, -1 closed, -2 limit */
char *task_take_messages(task *t, unsigned *revision);
int task_has_messages(task *t);
int task_close_messages(task *t); /* false if accepted messages still need processing */

void scheduler_set_completion_cb(scheduler *s, task_completion cb, void *ud);

/* Wait until no tasks are queued or running. Returns 0 ok, -1 timeout. */
int scheduler_wait_idle(scheduler *s, int timeout_ms);
/* Stop accepting work and join worker threads. Returns 0 ok, -1 timeout. */
int scheduler_shutdown(scheduler *s, int timeout_ms);

/* Cooperative abort check for runners: true if cancelled or past deadline. */
int task_should_abort(const task *t);

/* Cooperative yield for runners: voluntarily give up the worker thread so the
 * scheduler can run another task. No-op when not inside a scheduler coroutine. */
void scheduler_yield(void);

#ifdef __cplusplus
}
#endif
