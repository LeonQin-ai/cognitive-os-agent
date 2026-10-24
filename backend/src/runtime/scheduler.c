#include "runtime/scheduler.h"
#include "os/os_thread.h"
#include "os/os_time.h"
#include "os/os_coro.h"
#include "infra/util.h"
#include "runtime/task.h"

#include <stdlib.h>
#include <string.h>

typedef struct worker_slot {
    struct scheduler *sched;
    task *yield_head, *yield_tail; /* coroutine resumes only on its owner thread */
} worker_slot;

struct scheduler {
    int exited;
    size_t history_count, history_bytes;
    int workers;
    int max_workers;
    int max_active;
    uint64_t rejected;
    task_runner runner;
    void *worker_ud;

    task **queue; /* min-heap by (priority asc, id asc) */
    size_t qlen, qcap;

    task **all; /* active and retained tasks, sorted by monotonically assigned ID */
    size_t alen, acap;

    mutex_t mtx;
    cond not_empty;
    cond idle;
    int shutdown_flag;
    int active; /* queued + running */
    int completing; /* callbacks/final accounting outside active count */

    task_completion on_complete;
    void *complete_ud;

    int64_t next_id;
    thread_t **threads;
    worker_slot *slots;
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

/* O(log n) ready queue: a million virtual tasks must not shift an array on
 * every submit/pop. Returns -1 on allocation failure. */
static int queue_insert(scheduler *s, task *t) {
    if (s->qlen == s->qcap) {
        size_t cap = s->qcap ? s->qcap * 2 : 16;
        task **next = realloc(s->queue, cap * sizeof(task *));
        if (!next) return -1;
        s->queue = next;
        s->qcap = cap;
    }
    size_t i = s->qlen++;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!task_less(t, s->queue[parent])) break;
        s->queue[i] = s->queue[parent];
        i = parent;
    }
    s->queue[i] = t;
    return 0;
}

static task *queue_pop(scheduler *s) {
    task *t;

    if (s->qlen == 0)
        return NULL;
    t = s->queue[0];
    task *last = s->queue[--s->qlen];
    if (s->qlen) {
        size_t i = 0;
        while (2 * i + 1 < s->qlen) {
            size_t child = 2 * i + 1;
            if (child + 1 < s->qlen && task_less(s->queue[child + 1], s->queue[child]))
                child++;
            if (!task_less(s->queue[child], last)) break;
            s->queue[i] = s->queue[child];
            i = child;
        }
        s->queue[i] = last;
    }
    return t;
}

static int add_all(scheduler *s, task *t) {
    if (s->alen == s->acap) {
        size_t cap = s->acap ? s->acap * 2 : 16;
        task **next = realloc(s->all, cap * sizeof(task *));
        if (!next) return -1;
        s->all = next;
        s->acap = cap;
    }
    s->all[s->alen++] = t;
    return 0;
}

/* Scheduler lock stabilizes completion/output; progress has its own lock. */
static size_t task_bytes(task *t) {
    size_t bytes = sizeof(*t) + (t->workspace ? strlen(t->workspace) : 0) + (t->attachments_json ? strlen(t->attachments_json) : 0) +
        (t->input ? strlen(t->input) : 0) + (t->tag ? strlen(t->tag) : 0);
    if (t->completed && t->output) bytes += strlen(t->output);
    mutex_lock(&t->progress_mtx);
    bytes+=t->preview_text.cap;
    if (t->progress_json) bytes += strlen(t->progress_json);
    if (t->trace_json) bytes += strlen(t->trace_json);
    mutex_unlock(&t->progress_mtx);
    return bytes;
}

/* Completion callbacks must finish before a task becomes reclaimable. */
static void prune_history(scheduler *s) {
    size_t count = 0, bytes = 0;
    for (size_t i = 0; i < s->alen; i++) if (s->all[i]->completed) {
        count++; bytes += task_bytes(s->all[i]);
    }
    for (size_t i = 0; i < s->alen;) {
        task *t = s->all[i];
        if ((!s->history_count || count <= s->history_count) &&
            (!s->history_bytes || bytes <= s->history_bytes)) break;
        if (!t->completed || t->references || t->borrowed) { i++; continue; }
        count--; bytes -= task_bytes(t);
        task_free(t);
        memmove(s->all + i, s->all + i + 1, (s->alen-i-1)*sizeof(task *));
        s->alen--;
    }
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
    worker_slot *slot = (worker_slot *)arg;
    scheduler *s = slot->sched;
    for (;;) {
        task *t = NULL;
        mutex_lock(&s->mtx);
        while (!s->shutdown_flag && s->qlen == 0 && !slot->yield_head)
            cond_wait(&s->not_empty, &s->mtx);
        if (s->shutdown_flag && s->qlen == 0 && !slot->yield_head) {
            s->exited++;
            cond_broadcast(&s->idle);
            mutex_unlock(&s->mtx);
            break;
        }
        if (slot->yield_head) {
            t = slot->yield_head;
            slot->yield_head = t->ready_next;
            if (!slot->yield_head) slot->yield_tail = NULL;
            t->ready_next = NULL;
        } else t = queue_pop(s);
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
            s->completing++;
            finalize_task(s, t);
            if (t->coro) {
                coro_free((coro *)t->coro);
                t->coro = NULL;
            }
        } else {
            /* ucontext/Fiber state is tied to the thread where the task
             * yielded. Keep the continuation on that worker. */
            t->ready_next = NULL;
            if (slot->yield_tail) slot->yield_tail->ready_next = t;
            else slot->yield_head = t;
            slot->yield_tail = t;
        }
        task_completion cb = done ? s->on_complete : NULL;
        void *cud = s->complete_ud;
        mutex_unlock(&s->mtx);

        if (cb)
            cb(t, cud);
        if (done) {
            mutex_lock(&s->mtx);
            free(t->attachments_json); t->attachments_json = NULL;
            t->completed = 1;
            prune_history(s);
            s->completing--;
            if (s->active == 0 && s->completing == 0)
                cond_broadcast(&s->idle);
            mutex_unlock(&s->mtx);
        }
    }
}

scheduler *scheduler_new(int workers, task_runner runner, void *worker_ud) {
    if (workers < 1) workers = 1;
    return scheduler_new_limited(workers, workers >= 32 ? workers : 32, 0, runner, worker_ud);
}

scheduler *scheduler_new_limited(int workers, int max_workers, int max_active,
                                 task_runner runner, void *worker_ud) {
    scheduler *s;

    if (workers < 1 || max_workers < workers || max_workers > 256 || max_active < 0)
        return NULL;
    s = calloc(1, sizeof(scheduler));
    if (!s)
        return NULL;
    s->workers = workers;
    s->max_workers = max_workers;
    s->max_active = max_active;
    s->runner = runner;
    s->worker_ud = worker_ud;
    mutex_init(&s->mtx);
    cond_init(&s->not_empty);
    cond_init(&s->idle);

    s->threads = calloc((size_t)s->max_workers, sizeof(thread_t *));
    s->slots = calloc((size_t)s->max_workers, sizeof(worker_slot));
    if (!s->threads || !s->slots) {
        free(s->threads);
        free(s->slots);
        cond_destroy(&s->not_empty);
        cond_destroy(&s->idle);
        mutex_destroy(&s->mtx);
        free(s);
        return NULL;
    }

    for (int i = 0; i < workers; i++) {
        s->slots[i].sched = s;
        s->threads[i] = thread_create(worker_main, &s->slots[i]);
        if (!s->threads[i]) {
            mutex_lock(&s->mtx);
            s->shutdown_flag = 1;
            cond_broadcast(&s->not_empty);
            mutex_unlock(&s->mtx);
            for (int j = 0; j < i; j++)
                thread_join(s->threads[j]);
            free(s->threads);
            free(s->slots);
            cond_destroy(&s->not_empty);
            cond_destroy(&s->idle);
            mutex_destroy(&s->mtx);
            free(s);
            return NULL;
        }
    }

    return s;
}

void scheduler_get_stats(scheduler *s, scheduler_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s) return;
    mutex_lock(&s->mtx);
    out->workers = s->workers;
    out->max_workers = s->max_workers;
    out->active = s->active;
    out->max_active = s->max_active;
    out->queued = s->qlen;
    out->rejected = s->rejected;
    out->history_count = s->history_count; out->history_bytes = s->history_bytes;
    for (size_t i=0;i<s->alen;i++) if (s->all[i]->completed) {
        out->retained++; out->retained_bytes += task_bytes(s->all[i]);
    }
    mutex_unlock(&s->mtx);
}

void scheduler_free(scheduler *s) {
    if (!s)
        return;
    for (int i = 0; i < s->workers; i++) if (s->threads[i]) return; /* still owned by workers */
    for (size_t i = 0; i < s->alen; i++) {
        free(s->all[i]->workspace);
        strbuf_free(&s->all[i]->preview_text);
        free(s->all[i]->attachments_json);
        free(s->all[i]->input);
        free(s->all[i]->output);
        free(s->all[i]->progress_json);
        free(s->all[i]->trace_json);
        free(s->all[i]->pending_input);
        mutex_destroy(&s->all[i]->progress_mtx);
        free(s->all[i]->tag);
        free(s->all[i]);
    }

    free(s->all);
    free(s->queue);
    free(s->threads);
    free(s->slots);
    cond_destroy(&s->not_empty);
    cond_destroy(&s->idle);
    mutex_destroy(&s->mtx);
    free(s);
}

int64_t scheduler_submit(scheduler *s, int priority, const char *input, void *userdata, int64_t timeout_ms) {
    return scheduler_submit_tag(s, priority, input, userdata, timeout_ms, NULL);
}

int64_t scheduler_submit_tag(scheduler *s, int priority, const char *input, void *userdata, int64_t timeout_ms,
                                 const char *tag) {
    return scheduler_submit_tag_mode(s, priority, input, userdata, timeout_ms, tag, 0);
}

int64_t scheduler_submit_tag_mode(scheduler *s, int priority, const char *input, void *userdata, int64_t timeout_ms,
                                  const char *tag, int thinking_mode) {
    return scheduler_submit_attachments(s, priority, input, userdata, timeout_ms, tag, thinking_mode, NULL);
}
int64_t scheduler_submit_attachments(scheduler *s, int priority, const char *input, void *userdata,
    int64_t timeout_ms, const char *tag, int thinking_mode, const char *attachments) {
    return scheduler_submit_workspace(s,priority,input,userdata,timeout_ms,tag,thinking_mode,attachments,NULL);
}
int64_t scheduler_submit_workspace(scheduler *s, int priority, const char *input, void *userdata,
    int64_t timeout_ms, const char *tag, int thinking_mode, const char *attachments, const char *workspace) {
    return scheduler_submit_preview(s,priority,input,userdata,timeout_ms,tag,thinking_mode,attachments,workspace,0);
}
int64_t scheduler_submit_preview(scheduler *s,int priority,const char *input,void *userdata,
    int64_t timeout_ms,const char *tag,int thinking_mode,const char *attachments,const char *workspace,int stream_preview){
    task *t = calloc(1, sizeof(task));
    int64_t id;

    if (!t)
        return -1;
    if (mutex_init(&t->progress_mtx) != 0) { free(t); return -1; }
    mutex_lock(&s->mtx);
    if (s->shutdown_flag) {
        mutex_unlock(&s->mtx);
        mutex_destroy(&t->progress_mtx);
        free(t);
        return -1;
    }
    if (s->max_active > 0 && s->active >= s->max_active) {
        s->rejected++;
        mutex_unlock(&s->mtx);
        mutex_destroy(&t->progress_mtx);
        free(t);
        return SCHEDULER_FULL;
    }
    t->id = s->next_id;
    t->priority = priority;
    t->timeout_ms = timeout_ms;
    t->created_ms = time_now_ms();
    t->status = TS_QUEUED;
    t->input = input ? xstrdup(input) : xstrdup("");
    t->tag = tag ? xstrdup(tag) : NULL;
    t->attachments_json = attachments ? xstrdup(attachments) : NULL;
    t->workspace = workspace ? xstrdup(workspace) : NULL;
    t->stream_preview=stream_preview;
    t->thinking_mode = thinking_mode != 0;
    t->userdata = userdata;
    t->sched = s;
    if (!t->input || (tag && !t->tag) || (workspace && !t->workspace) || (attachments && !t->attachments_json) || add_all(s, t) != 0) {
        mutex_unlock(&s->mtx);
        free(t->workspace); free(t->attachments_json); free(t->input); free(t->tag); mutex_destroy(&t->progress_mtx); free(t);
        return -1;
    }
    if (queue_insert(s, t) != 0) {
        s->alen--; /* the new task is still the last index entry */
        mutex_unlock(&s->mtx);
        free(t->workspace); free(t->attachments_json); free(t->input); free(t->tag); mutex_destroy(&t->progress_mtx); free(t);
        return -1;
    }
    s->next_id++;
    s->active++;
    /* Keep the initial thread count small, then add bounded executors as
     * concurrent work arrives. Virtual tasks remain queued without stacks. */
    while (s->active > s->workers && s->workers < s->max_workers) {
        s->slots[s->workers].sched = s;
        thread_t *worker = thread_create(worker_main, &s->slots[s->workers]);
        if (!worker) break;
        s->threads[s->workers++] = worker;
    }
    id = t->id;
    cond_signal(&s->not_empty);
    mutex_unlock(&s->mtx);
    return id;
}

static task *find_task(scheduler *s, int64_t id) {
    size_t lo = 0, hi = s->alen;
    while (lo < hi) {
        size_t mid = lo + (hi-lo)/2;
        if (s->all[mid]->id < id) lo = mid+1; else hi = mid;
    }
    return lo < s->alen && s->all[lo]->id == id ? s->all[lo] : NULL;
}
task *scheduler_get(scheduler *s, int64_t id) {
    mutex_lock(&s->mtx);
    task *t = find_task(s, id);
    if (t) t->borrowed = 1; /* legacy borrowed pointers live until free */
    mutex_unlock(&s->mtx);
    return t;
}
task *scheduler_acquire(scheduler *s, int64_t id) {
    mutex_lock(&s->mtx);
    task *t = find_task(s, id);
    if (t) t->references++;
    mutex_unlock(&s->mtx);
    return t;
}
void scheduler_release(scheduler *s, task *t) {
    if (!t) return;
    mutex_lock(&s->mtx);
    if (t->references) t->references--;
    prune_history(s);
    mutex_unlock(&s->mtx);
}
void scheduler_set_history_limits(scheduler *s, size_t count, size_t bytes) {
    mutex_lock(&s->mtx);
    s->history_count = count; s->history_bytes = bytes;
    prune_history(s);
    mutex_unlock(&s->mtx);
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
    while (s->active > 0 || s->completing > 0) {
        if (timeout_ms > 0 && time_now_ms() >= deadline) {
            mutex_unlock(&s->mtx);
            return -1;
        }
        cond_timedwait_ms(&s->idle, &s->mtx, 50);
    }

    mutex_unlock(&s->mtx);
    return 0;
}

int scheduler_shutdown(scheduler *s, int timeout_ms) {
    int64_t deadline;

    mutex_lock(&s->mtx);
    s->shutdown_flag = 1;
    cond_broadcast(&s->not_empty);
    mutex_unlock(&s->mtx);

    deadline = timeout_ms > 0 ? time_now_ms() + timeout_ms : 0;
    mutex_lock(&s->mtx);
    while (s->exited < s->workers) {
        int64_t left = deadline ? deadline - time_now_ms() : 50;
        if (deadline && left <= 0) { mutex_unlock(&s->mtx); return -1; }
        cond_timedwait_ms(&s->idle, &s->mtx, left > 50 ? 50 : (int)left);
    }
    mutex_unlock(&s->mtx);
    for (int i = 0; i < s->workers; i++) {
        if (s->threads[i]) {
            thread_join(s->threads[i]);
            s->threads[i] = NULL;
        }
    }

    return 0;
}

void scheduler_ensure_next_id(scheduler *s,int64_t next_id) {
    if(!s || next_id<0) return;
    mutex_lock(&s->mtx); if(s->next_id<next_id) s->next_id=next_id; mutex_unlock(&s->mtx);
}
