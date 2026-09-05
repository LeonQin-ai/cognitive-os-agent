#include "cognitive-os-agent/runtime/scheduler.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/os/os_coro.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>

struct coa_scheduler {
    int workers;
    coa_task_runner runner;
    void *worker_ud;

    coa_task **queue;      /* sorted by (priority asc, id asc) */
    size_t qlen, qcap;

    coa_task **all;        /* all tasks ever, for lookup */
    size_t alen, acap;

    coa_mutex mtx;
    coa_cond  not_empty;
    int shutdown_flag;
    int active;           /* queued + running */

    coa_task_completion on_complete;
    void *complete_ud;

    int64_t next_id;
    coa_thread **threads;
};

int coa_task_should_abort(const coa_task *t) {
    if (!t) return 1;
    if (t->cancel_flag) return 1;
    if (t->timeout_ms > 0 && (coa_time_now_ms() - t->started_ms) > t->timeout_ms) return 1;
    return 0;
}

static int task_less(const coa_task *a, const coa_task *b) {
    if (a->priority != b->priority) return a->priority < b->priority;
    return a->id < b->id;
}

/* insert into sorted queue */
static void queue_insert(coa_scheduler *s, coa_task *t) {
    if (s->qlen == s->qcap) {
        size_t cap = s->qcap ? s->qcap * 2 : 16;
        s->queue = realloc(s->queue, cap * sizeof(coa_task *));
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

static coa_task *queue_pop(coa_scheduler *s) {
    if (s->qlen == 0) return NULL;
    coa_task *t = s->queue[0];
    memmove(s->queue, s->queue + 1, (s->qlen - 1) * sizeof(coa_task *));
    s->qlen--;
    return t;
}

static void add_all(coa_scheduler *s, coa_task *t) {
    if (s->alen == s->acap) {
        size_t cap = s->acap ? s->acap * 2 : 16;
        s->all = realloc(s->all, cap * sizeof(coa_task *));
        s->acap = cap;
    }
    s->all[s->alen++] = t;
}

/* Coroutine body: run the task's runner to completion (or yield). */
static void task_coro_entry(void *arg) {
    coa_task *t = (coa_task *)arg;
    coa_scheduler *s = (coa_scheduler *)t->sched;
    if (s && s->runner) s->runner(t, s, s->worker_ud);
}

/* Set the terminal status and decrement the active counter. Called under lock. */
static void finalize_task(coa_scheduler *s, coa_task *t) {
    t->finished_ms = coa_time_now_ms();
    if (t->timed_out) t->status = COA_TS_TIMEOUT;
    else if (t->cancel_flag) t->status = COA_TS_CANCELLED;
    else if (t->status == COA_TS_RUNNING) t->status = COA_TS_DONE;
    s->active--;
}

void coa_scheduler_yield(void) { coa_coro_yield(); }

static void worker_main(void *arg) {
    coa_scheduler *s = (coa_scheduler *)arg;
    for (;;) {
        coa_task *t = NULL;
        coa_mutex_lock(&s->mtx);
        while (!s->shutdown_flag && s->qlen == 0)
            coa_cond_wait(&s->not_empty, &s->mtx);
        if (s->shutdown_flag && s->qlen == 0) {
            coa_mutex_unlock(&s->mtx);
            break;
        }
        t = queue_pop(s);
        if (!t->coro) {
            /* first run: create the coroutine (lazily, avoids eager 256KB stacks) */
            t->started_ms = coa_time_now_ms();
            t->coro = coa_coro_new(task_coro_entry, t, 0);
            t->status = t->coro ? COA_TS_RUNNING : COA_TS_FAILED;
        }
        coa_mutex_unlock(&s->mtx);

        if (t->coro) coa_coro_resume(t->coro); /* runs until yield or finish */

        coa_mutex_lock(&s->mtx);
        int done = !t->coro || coa_coro_done((coa_coro *)t->coro);
        if (done) {
            finalize_task(s, t);
            if (t->coro) { coa_coro_free((coa_coro *)t->coro); t->coro = NULL; }
        } else {
            queue_insert(s, t); /* yielded: re-enter the ready queue */
        }
        coa_task_completion cb = done ? s->on_complete : NULL;
        void *cud = s->complete_ud;
        coa_mutex_unlock(&s->mtx);

        if (cb) cb(t, cud);
        coa_mutex_lock(&s->mtx);
        coa_cond_signal(&s->not_empty);
        coa_mutex_unlock(&s->mtx);
    }
}

coa_scheduler *coa_scheduler_new(int workers, coa_task_runner runner, void *worker_ud) {
    if (workers < 1) workers = 1;
    coa_scheduler *s = calloc(1, sizeof(coa_scheduler));
    if (!s) return NULL;
    s->workers = workers;
    s->runner = runner;
    s->worker_ud = worker_ud;
    coa_mutex_init(&s->mtx);
    coa_cond_init(&s->not_empty);

    s->threads = calloc((size_t)workers, sizeof(coa_thread *));
    if (!s->threads) { free(s); return NULL; }
    for (int i = 0; i < workers; i++) {
        s->threads[i] = coa_thread_create(worker_main, s);
        if (!s->threads[i]) {
            /* shrink worker count; still usable */
            for (int j = 0; j < i; j++) coa_thread_join(s->threads[j]);
            free(s->threads);
            free(s);
            return NULL;
        }
    }
    return s;
}

void coa_scheduler_free(coa_scheduler *s) {
    if (!s) return;
    for (size_t i = 0; i < s->alen; i++) {
        free(s->all[i]->input);
        free(s->all[i]->output);
        free(s->all[i]);
    }
    free(s->all);
    free(s->queue);
    free(s->threads);
    coa_cond_destroy(&s->not_empty);
    coa_mutex_destroy(&s->mtx);
    free(s);
}

int64_t coa_scheduler_submit(coa_scheduler *s, int priority, const char *input,
                            void *userdata, int64_t timeout_ms) {
    coa_task *t = calloc(1, sizeof(coa_task));
    if (!t) return -1;
    coa_mutex_lock(&s->mtx);
    t->id = s->next_id++;
    t->priority = priority;
    t->timeout_ms = timeout_ms;
    t->created_ms = coa_time_now_ms();
    t->status = COA_TS_QUEUED;
    t->input = input ? coa_strdup(input) : coa_strdup("");
    t->userdata = userdata;
    t->sched = s;
    queue_insert(s, t);
    add_all(s, t);
    s->active++;
    int64_t id = t->id;
    coa_cond_broadcast(&s->not_empty);
    coa_mutex_unlock(&s->mtx);
    return id;
}

int coa_scheduler_cancel(coa_scheduler *s, int64_t id) {
    int found = 0;
    coa_mutex_lock(&s->mtx);
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
    coa_mutex_unlock(&s->mtx);
    return found;
}

coa_task *coa_scheduler_get(coa_scheduler *s, int64_t id) {
    coa_task *r = NULL;
    coa_mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->alen; i++)
        if (s->all[i]->id == id) { r = s->all[i]; break; }
    coa_mutex_unlock(&s->mtx);
    return r;
}

int coa_scheduler_active(coa_scheduler *s) {
    int r;
    coa_mutex_lock(&s->mtx);
    r = s->active;
    coa_mutex_unlock(&s->mtx);
    return r;
}

int coa_scheduler_total(coa_scheduler *s) {
    int r;
    coa_mutex_lock(&s->mtx);
    r = (int)s->alen;
    coa_mutex_unlock(&s->mtx);
    return r;
}

void coa_scheduler_set_completion_cb(coa_scheduler *s, coa_task_completion cb, void *ud) {
    coa_mutex_lock(&s->mtx);
    s->on_complete = cb;
    s->complete_ud = ud;
    coa_mutex_unlock(&s->mtx);
}

int coa_scheduler_wait_idle(coa_scheduler *s, int timeout_ms) {
    int64_t deadline = timeout_ms > 0 ? coa_time_now_ms() + timeout_ms : 0;
    coa_mutex_lock(&s->mtx);
    while (s->active > 0) {
        if (timeout_ms > 0 && coa_time_now_ms() >= deadline) {
            coa_mutex_unlock(&s->mtx);
            return -1;
        }
        coa_cond_timedwait_ms(&s->not_empty, &s->mtx, 50);
    }
    coa_mutex_unlock(&s->mtx);
    return 0;
}

int coa_scheduler_shutdown(coa_scheduler *s, int timeout_ms) {
    coa_mutex_lock(&s->mtx);
    s->shutdown_flag = 1;
    coa_cond_broadcast(&s->not_empty);
    coa_mutex_unlock(&s->mtx);

    int64_t deadline = timeout_ms > 0 ? coa_time_now_ms() + timeout_ms : 0;
    for (int i = 0; i < s->workers; i++) {
        if (timeout_ms > 0 && coa_time_now_ms() >= deadline) return -1;
        if (s->threads[i]) {
            coa_thread_join(s->threads[i]);
            s->threads[i] = NULL;
        }
    }
    return 0;
}
