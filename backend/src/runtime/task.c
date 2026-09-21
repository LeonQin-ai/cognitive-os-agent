/* task.c — standalone task lifecycle helpers. */
#include "runtime/task.h"
#include "os/os_time.h"
#include "infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

task *task_new(int64_t id, int priority, const char *input, int64_t timeout_ms) {
    task *t = (task *)calloc(1, sizeof(task));
    if (!t)
        return NULL;
    if (mutex_init(&t->progress_mtx) != 0) { free(t); return NULL; }
    t->id = id;
    t->priority = priority;
    t->timeout_ms = timeout_ms;
    t->created_ms = time_now_ms();
    t->status = TS_QUEUED;
    t->input = input ? xstrdup(input) : NULL;
    return t;
}

void task_free(task *t) {
    if (!t)
        return;
    free(t->input);
    free(t->output);
    free(t->tag);
    free(t->progress_json);
    free(t->pending_input);
    mutex_destroy(&t->progress_mtx);
    free(t);
}

void task_transition(task *t, task_status st, int64_t now_ms) {
    if (!t)
        return;
    if (now_ms == 0)
        now_ms = time_now_ms();
    if (st == TS_RUNNING && t->started_ms == 0)
        t->started_ms = now_ms;
    if ((st == TS_DONE || st == TS_FAILED || st == TS_CANCELLED || st == TS_TIMEOUT) &&
        t->finished_ms == 0)
        t->finished_ms = now_ms;
    t->status = st;
}

const char *task_status_name(task_status st) {
    switch (st) {
    case TS_QUEUED:
        return "queued";
    case TS_RUNNING:
        return "running";
    case TS_DONE:
        return "done";
    case TS_FAILED:
        return "failed";
    case TS_CANCELLED:
        return "cancelled";
    case TS_TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}

char *task_to_json(const task *t) {
    cJSON *o;
    char *s;

    if (!t)
        return xstrdup("{}");
    o = cJSON_CreateObject();
    if (!o)
        return xstrdup("{}");
    cJSON_AddNumberToObject(o, "id", (double)t->id);
    cJSON_AddNumberToObject(o, "priority", t->priority);
    cJSON_AddStringToObject(o, "status", task_status_name(t->status));
    cJSON_AddStringToObject(o, "input", t->input ? t->input : "");
    cJSON_AddStringToObject(o, "output", t->output ? t->output : "");
    cJSON_AddNumberToObject(o, "timeout_ms", (double)t->timeout_ms);
    s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s ? s : xstrdup("{}");
}

void task_set_progress(task *t, const char *json) {
    char *copy = json ? xstrdup(json) : NULL;
    if (!t || !copy) { free(copy); return; }
    mutex_lock(&t->progress_mtx);
    free(t->progress_json);
    t->progress_json = copy;
    mutex_unlock(&t->progress_mtx);
}
char *task_progress_copy(task *t) {
    if (!t) return NULL;
    mutex_lock(&t->progress_mtx);
    char *copy = t->progress_json ? xstrdup(t->progress_json) : NULL;
    mutex_unlock(&t->progress_mtx);
    return copy;
}

int task_add_message(task *t, const char *message) {
    if (!t || !message || !*message) return -2;
    mutex_lock(&t->progress_mtx);
    if (t->updates_closed || t->status >= TS_DONE || t->cancel_flag) {
        mutex_unlock(&t->progress_mtx); return -1;
    }
    size_t old = t->pending_input ? strlen(t->pending_input) : 0;
    size_t len = strlen(message);
    if (old + len > 16384 || t->update_count >= 32) {
        mutex_unlock(&t->progress_mtx); return -2;
    }
    char *text = realloc(t->pending_input, old + len + 2);
    if (!text) { mutex_unlock(&t->progress_mtx); return -2; }
    if (old) text[old++] = '\n';
    memcpy(text + old, message, len + 1);
    t->pending_input = text;
    int revision = (int)++t->update_count;
    mutex_unlock(&t->progress_mtx);
    return revision;
}
char *task_take_messages(task *t, unsigned *revision) {
    if (!t) return NULL;
    mutex_lock(&t->progress_mtx);
    char *text = t->pending_input; t->pending_input = NULL;
    if (revision) *revision = t->update_count;
    mutex_unlock(&t->progress_mtx);
    return text;
}
int task_has_messages(task *t) {
    if (!t) return 0;
    mutex_lock(&t->progress_mtx);
    int pending = t->pending_input != NULL;
    mutex_unlock(&t->progress_mtx);
    return pending;
}
int task_close_messages(task *t) {
    if (!t) return 1;
    mutex_lock(&t->progress_mtx);
    int ready = t->pending_input == NULL;
    if (ready) t->updates_closed = 1;
    mutex_unlock(&t->progress_mtx);
    return ready;
}
