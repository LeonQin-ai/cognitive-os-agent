#include "cagent/runtime/scheduler.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/os_time.h"
#include "cagent/os/os_coro.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>

struct ca_scheduler {
    int workers;
    ca_task_runner runner;
    void *worker_ud;

    ca_task **queue;      /* sorted by (priority asc, id asc) */
    size_t qlen, qcap;

    ca_task **all;        /* all tasks ever, for lookup */
    size_t alen, acap;

    ca_mutex mtx;
    ca_cond  not_empty;
    int shutdown_flag;
    int active;           /* queued + running */

    ca_task_completion on_complete;
    void *complete_ud;

    int64_t next_id;
    ca_thread **threads;
};

int ca_task_should_abort(const ca_task *t) {
    if (!t) return 1;
    if (t->cancel_flag) return 1;
    if (t->timeout_ms > 0 && (ca_time_now_ms() - t->started_ms) > t->timeout_ms) return 1;
    return 0;
}

static int task_less(const ca_task *a, const ca_task *b) {
    if (a->priority != b->priority) return a->priority < b->priority;
    return a->id < b->id;
}

/* insert into sorted queue */
static void queue_insert(ca_scheduler *s, ca_task *t) {
    if (s->qlen == s->qcap) {
        size_t cap = s->qcap ? s->qcap * 2 : 16;
        s->queue = realloc(s->queue, cap * sizeof(ca_task *));
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

static ca_task *queue_pop(ca_scheduler *s) {
    if (s->qlen == 0) return NULL;
    ca_task *t = s->queue[0];
    memmove(s->queue, s->queue + 1, (s->qlen - 1) * sizeof(ca_task *));
    s->qlen--;
    return t;
}

static void add_all(ca_scheduler *s, ca_task *t) {
    if (s->alen == s->acap) {
        size_t cap = s->acap ? s->acap * 2 : 16;
        s->all = realloc(s->all, cap * sizeof(ca_task *));
        s->acap = cap;
    }
    s->all[s->alen++] = t;
}

/* Coroutine body: run the task's runner to completion (or yield). */
static void task_coro_entry(void *arg) {
    ca_task *t = (ca_task *)arg;
    ca_scheduler *s = (ca_scheduler *)t->sched;
    if (s && s->runner) s->runner(t, s, s->worker_ud);
}

/* Set the terminal status and decrement the active counter. Called under lock. */
static void finalize_task(ca_scheduler *s, ca_task *t) {
    t->finished_ms = ca_time_now_ms();
    if (t->timed_out) t->status = CA_TS_TIMEOUT;
    else if (t->cancel_flag) t->status = CA_TS_CANCELLED;
    else if (t->status == CA_TS_RUNNING) t->status = CA_TS_DONE;
    s->active--;
}

void ca_scheduler_yield(void) { ca_coro_yield(); }

static void worker_main(void *arg) {
    ca_scheduler *s = (ca_scheduler *)arg;
    for (;;) {
        ca_task *t = NULL;
        ca_mutex_lock(&s->mtx);
        while (!s->shutdown_flag && s->qlen == 0)
            ca_cond_wait(&s->not_empty, &s->mtx);
        if (s->shutdown_flag && s->qlen == 0) {
            ca_mutex_unlock(&s->mtx);
            break;
        }
        t = queue_pop(s);
        if (!t->coro) {
            /* first run: create the coroutine (lazily, avoids eager 256KB stacks) */
            t->started_ms = ca_time_now_ms();
            t->coro = ca_coro_new(task_coro_entry, t, 0);
            t->status = t->coro ? CA_TS_RUNNING : CA_TS_FAILED;
        }
        ca_mutex_unlock(&s->mtx);

        if (t->coro) ca_coro_resume(t->coro); /* runs until yield or finish */

        ca_mutex_lock(&s->mtx);
        int done = !t->coro || ca_coro_done((ca_coro *)t->coro);
        if (done) {
            finalize_task(s, t);
            if (t->coro) { ca_coro_free((ca_coro *)t->coro); t->coro = NULL; }
        } else {
            queue_insert(s, t); /* yielded: re-enter the ready queue */
        }
        ca_task_completion cb = done ? s->on_complete : NULL;
        void *cud = s->complete_ud;
        ca_mutex_unlock(&s->mtx);

        if (cb) cb(t, cud);
        ca_mutex_lock(&s->mtx);
        ca_cond_signal(&s->not_empty);
        ca_mutex_unlock(&s->mtx);
    }
}

ca_scheduler *ca_scheduler_new(int workers, ca_task_runner runner, void *worker_ud) {
    if (workers < 1) workers = 1;
    ca_scheduler *s = calloc(1, sizeof(ca_scheduler));
    if (!s) return NULL;
    s->workers = workers;
    s->runner = runner;
    s->worker_ud = worker_ud;
    ca_mutex_init(&s->mtx);
    ca_cond_init(&s->not_empty);

    s->threads = calloc((size_t)workers, sizeof(ca_thread *));
    if (!s->threads) { free(s); return NULL; }
    for (int i = 0; i < workers; i++) {
        s->threads[i] = ca_thread_create(worker_main, s);
        if (!s->threads[i]) {
            /* shrink worker count; still usable */
            for (int j = 0; j < i; j++) ca_thread_join(s->threads[j]);
            free(s->threads);
            free(s);
            return NULL;
        }
    }
    return s;
}

void ca_scheduler_free(ca_scheduler *s) {
    if (!s) return;
    for (size_t i = 0; i < s->alen; i++) {
        free(s->all[i]->input);
        free(s->all[i]->output);
        free(s->all[i]);
    }
    free(s->all);
    free(s->queue);
    free(s->threads);
    ca_cond_destroy(&s->not_empty);
    ca_mutex_destroy(&s->mtx);
    free(s);
}

int64_t ca_scheduler_submit(ca_scheduler *s, int priority, const char *input,
                            void *userdata, int64_t timeout_ms) {
    ca_task *t = calloc(1, sizeof(ca_task));
    if (!t) return -1;
    ca_mutex_lock(&s->mtx);
    t->id = s->next_id++;
    t->priority = priority;
    t->timeout_ms = timeout_ms;
    t->created_ms = ca_time_now_ms();
    t->status = CA_TS_QUEUED;
    t->input = input ? ca_strdup(input) : ca_strdup("");
    t->userdata = userdata;
    t->sched = s;
    queue_insert(s, t);
    add_all(s, t);
    s->active++;
    int64_t id = t->id;
    ca_cond_broadcast(&s->not_empty);
    ca_mutex_unlock(&s->mtx);
    return id;
}

int ca_scheduler_cancel(ca_scheduler *s, int64_t id) {
    int found = 0;
    ca_mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->qlen; i++) {
        if (s->queue[i]->id == id) {
            s->queue[i]->cancel_flag = 1;
            found = 1;
            break;
        }
    }
    if (!found) {
        for (size_t i = 0; i < s->alen; i++) {
            if (s->all[i]->id == id) {
                s->all[i]->cancel_flag = 1;
                found = 1;
                break;
            }
        }
    }
    ca_mutex_unlock(&s->mtx);
    return found;
}

ca_task *ca_scheduler_get(ca_scheduler *s, int64_t id) {
    ca_task *r = NULL;
    ca_mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->alen; i++)
        if (s->all[i]->id == id) { r = s->all[i]; break; }
    ca_mutex_unlock(&s->mtx);
    return r;
}

int ca_scheduler_active(ca_scheduler *s) {
    int r;
    ca_mutex_lock(&s->mtx);
    r = s->active;
    ca_mutex_unlock(&s->mtx);
    return r;
}

int ca_scheduler_total(ca_scheduler *s) {
    int r;
    ca_mutex_lock(&s->mtx);
    r = (int)s->alen;
    ca_mutex_unlock(&s->mtx);
    return r;
}

void ca_scheduler_set_completion_cb(ca_scheduler *s, ca_task_completion cb, void *ud) {
    ca_mutex_lock(&s->mtx);
    s->on_complete = cb;
    s->complete_ud = ud;
    ca_mutex_unlock(&s->mtx);
}

int ca_scheduler_wait_idle(ca_scheduler *s, int timeout_ms) {
    int64_t deadline = timeout_ms > 0 ? ca_time_now_ms() + timeout_ms : 0;
    ca_mutex_lock(&s->mtx);
    while (s->active > 0) {
        if (timeout_ms > 0 && ca_time_now_ms() >= deadline) {
            ca_mutex_unlock(&s->mtx);
            return -1;
        }
        ca_cond_timedwait_ms(&s->not_empty, &s->mtx, 50);
    }
    ca_mutex_unlock(&s->mtx);
    return 0;
}

int ca_scheduler_shutdown(ca_scheduler *s, int timeout_ms) {
    ca_mutex_lock(&s->mtx);
    s->shutdown_flag = 1;
    ca_cond_broadcast(&s->not_empty);
    ca_mutex_unlock(&s->mtx);

    int64_t deadline = timeout_ms > 0 ? ca_time_now_ms() + timeout_ms : 0;
    for (int i = 0; i < s->workers; i++) {
        if (timeout_ms > 0 && ca_time_now_ms() >= deadline) return -1;
        if (s->threads[i]) {
            ca_thread_join(s->threads[i]);
            s->threads[i] = NULL;
        }
    }
    return 0;
}
