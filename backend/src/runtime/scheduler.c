#include "cognitive-os-agent/runtime/scheduler.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/os/os_coro.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>

struct scheduler {
    int workers;
    task_runner runner;
    void *worker_ud;

    task **queue; /* sorted by (priority asc, id asc) */
    size_t qlen, qcap;

    task **all; /* all tasks ever, for lookup */
    size_t alen, acap;

    mutex_t mtx;
    cond not_empty;
    int shutdown_flag;
    int active; /* queued + running */

    task_completion on_complete;
    void *complete_ud;

    int64_t next_id;
    thread_t **threads;
};

int task_should_abort(const task *t) {
    if (!t)
        return 1;
    if (t->cancel_flag)
        return 1;
    if (t->timeout_ms > 0 && (time_now_ms() - t->started_ms) > t->timeout_ms)
        return 1;
    return 0;
}

static int task_less(const task *a, const task *b) {
    if (a->priority != b->priority)
        return a->priority < b->priority;
    return a->id < b->id;
}

/* insert into sorted queue */
static void queue_insert(scheduler *s, task *t) {
    if (s->qlen == s->qcap) {
        size_t cap = s->qcap ? s->qcap * 2 : 16;
        s->queue = realloc(s->queue, cap * sizeof(task *));
        s->qcap = cap;
    }
    size_t i = s->qlen;
    while (i > 0 && task_less(t, s->queue[i - 1])) {
        s->queue[i] = s->queue[i - 1];
        i--;
    }
    s->queue[i] = t;
    s->qlen++;
}

static task *queue_pop(scheduler *s) {
    if (s->qlen == 0)
        return NULL;
    task *t = s->queue[0];
    memmove(s->queue, s->queue + 1, (s->qlen - 1) * sizeof(task *));
    s->qlen--;
    return t;
}

static void add_all(scheduler *s, task *t) {
    if (s->alen == s->acap) {
        size_t cap = s->acap ? s->acap * 2 : 16;
        s->all = realloc(s->all, cap * sizeof(task *));
        s->acap = cap;
    }
    s->all[s->alen++] = t;
}

/* Coroutine body: run the task's runner to completion (or yield). */
static void task_coro_entry(void *arg) {
    task *t = (task *)arg;
    scheduler *s = (scheduler *)t->sched;
    if (s && s->runner)
        s->runner(t, s, s->worker_ud);
}

/* Set the terminal status and decrement the active counter. Called under lock. */
static void finalize_task(scheduler *s, task *t) {
    t->finished_ms = time_now_ms();
    if (t->timed_out)
        t->status = TS_TIMEOUT;
    else if (t->cancel_flag)
        t->status = TS_CANCELLED;
    else if (t->status == TS_RUNNING)
        t->status = TS_DONE;
    s->active--;
}

void scheduler_yield(void) {
    coro_yield();
}

static void worker_main(void *arg) {
    scheduler *s = (scheduler *)arg;
    for (;;) {
        task *t = NULL;
        mutex_lock(&s->mtx);
        while (!s->shutdown_flag && s->qlen == 0)
            cond_wait(&s->not_empty, &s->mtx);
        if (s->shutdown_flag && s->qlen == 0) {
            mutex_unlock(&s->mtx);
            break;
        }
        t = queue_pop(s);
        if (!t->coro) {
            /* first run: create the coroutine (lazily, avoids eager 256KB stacks) */
            t->started_ms = time_now_ms();
            t->coro = coro_new(task_coro_entry, t, 0);
            t->status = t->coro ? TS_RUNNING : TS_FAILED;
        }
        mutex_unlock(&s->mtx);

        if (t->coro)
            coro_resume(t->coro); /* runs until yield or finish */

        mutex_lock(&s->mtx);
        int done = !t->coro || coro_done((coro *)t->coro);
        if (done) {
            finalize_task(s, t);
            if (t->coro) {
                coro_free((coro *)t->coro);
                t->coro = NULL;
            }
        } else {
            queue_insert(s, t); /* yielded: re-enter the ready queue */
        }
        task_completion cb = done ? s->on_complete : NULL;
        void *cud = s->complete_ud;
        mutex_unlock(&s->mtx);

        if (cb)
            cb(t, cud);
        mutex_lock(&s->mtx);
        cond_signal(&s->not_empty);
        mutex_unlock(&s->mtx);
    }
}

scheduler *scheduler_new(int workers, task_runner runner, void *worker_ud) {
    if (workers < 1)
        workers = 1;
    scheduler *s = calloc(1, sizeof(scheduler));
    if (!s)
        return NULL;
    s->workers = workers;
    s->runner = runner;
    s->worker_ud = worker_ud;
    mutex_init(&s->mtx);
    cond_init(&s->not_empty);

    s->threads = calloc((size_t)workers, sizeof(thread_t *));
    if (!s->threads) {
        free(s);
        return NULL;
    }
    for (int i = 0; i < workers; i++) {
        s->threads[i] = thread_create(worker_main, s);
        if (!s->threads[i]) {
            /* shrink worker count; still usable */
            for (int j = 0; j < i; j++)
                thread_join(s->threads[j]);
            free(s->threads);
            free(s);
            return NULL;
        }
    }
    return s;
}

void scheduler_free(scheduler *s) {
    if (!s)
        return;
    for (size_t i = 0; i < s->alen; i++) {
        free(s->all[i]->input);
        free(s->all[i]->output);
        free(s->all[i]->tag);
        free(s->all[i]);
    }
    free(s->all);
    free(s->queue);
    free(s->threads);
    cond_destroy(&s->not_empty);
    mutex_destroy(&s->mtx);
    free(s);
}

int64_t scheduler_submit(scheduler *s, int priority, const char *input, void *userdata, int64_t timeout_ms) {
    return scheduler_submit_tag(s, priority, input, userdata, timeout_ms, NULL);
}

int64_t scheduler_submit_tag(scheduler *s, int priority, const char *input, void *userdata, int64_t timeout_ms,
                                 const char *tag) {
    task *t = calloc(1, sizeof(task));
    if (!t)
        return -1;
    mutex_lock(&s->mtx);
    t->id = s->next_id++;
    t->priority = priority;
    t->timeout_ms = timeout_ms;
    t->created_ms = time_now_ms();
    t->status = TS_QUEUED;
    t->input = input ? xstrdup(input) : xstrdup("");
    t->tag = tag ? xstrdup(tag) : NULL;
    t->userdata = userdata;
    t->sched = s;
    queue_insert(s, t);
    add_all(s, t);
    s->active++;
    int64_t id = t->id;
    cond_broadcast(&s->not_empty);
    mutex_unlock(&s->mtx);
    return id;
}

task *scheduler_get(scheduler *s, int64_t id) {
    task *r = NULL;
    mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->alen; i++)
        if (s->all[i]->id == id) {
            r = s->all[i];
            break;
        }
    mutex_unlock(&s->mtx);
    return r;
}

int scheduler_total(scheduler *s) {
    int r;
    mutex_lock(&s->mtx);
    r = (int)s->alen;
    mutex_unlock(&s->mtx);
    return r;
}

void scheduler_set_completion_cb(scheduler *s, task_completion cb, void *ud) {
    mutex_lock(&s->mtx);
    s->on_complete = cb;
    s->complete_ud = ud;
    mutex_unlock(&s->mtx);
}

int scheduler_wait_idle(scheduler *s, int timeout_ms) {
    int64_t deadline = timeout_ms > 0 ? time_now_ms() + timeout_ms : 0;
    mutex_lock(&s->mtx);
    while (s->active > 0) {
        if (timeout_ms > 0 && time_now_ms() >= deadline) {
            mutex_unlock(&s->mtx);
            return -1;
        }
        cond_timedwait_ms(&s->not_empty, &s->mtx, 50);
    }
    mutex_unlock(&s->mtx);
    return 0;
}

int scheduler_shutdown(scheduler *s, int timeout_ms) {
    mutex_lock(&s->mtx);
    s->shutdown_flag = 1;
    cond_broadcast(&s->not_empty);
    mutex_unlock(&s->mtx);

    int64_t deadline = timeout_ms > 0 ? time_now_ms() + timeout_ms : 0;
    for (int i = 0; i < s->workers; i++) {
        if (timeout_ms > 0 && time_now_ms() >= deadline)
            return -1;
        if (s->threads[i]) {
            thread_join(s->threads[i]);
            s->threads[i] = NULL;
        }
    }
    return 0;
}
