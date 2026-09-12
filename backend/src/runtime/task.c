/* task.c — standalone task lifecycle helpers. */
#include "cognitive-os-agent/runtime/task.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

task *task_new(int64_t id, int priority, const char *input, int64_t timeout_ms) {
    task *t = (task *)calloc(1, sizeof(task));
    if (!t)
        return NULL;
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
