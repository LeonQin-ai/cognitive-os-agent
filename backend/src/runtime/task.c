/* task.c — standalone task lifecycle helpers. */
#include "cagent/runtime/task.h"
#include "cagent/os/os_time.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

ca_task *ca_task_new(int64_t id, int priority, const char *input, int64_t timeout_ms) {
    ca_task *t = (ca_task *)calloc(1, sizeof(ca_task));
    if (!t) return NULL;
    t->id = id;
    t->priority = priority;
    t->timeout_ms = timeout_ms;
    t->created_ms = ca_time_now_ms();
    t->status = CA_TS_QUEUED;
    t->input = input ? ca_strdup(input) : NULL;
    return t;
}

void ca_task_free(ca_task *t) {
    if (!t) return;
    free(t->input);
    free(t->output);
    free(t);
}

void ca_task_transition(ca_task *t, ca_task_status st, int64_t now_ms) {
    if (!t) return;
    if (now_ms == 0) now_ms = ca_time_now_ms();
    if (st == CA_TS_RUNNING && t->started_ms == 0) t->started_ms = now_ms;
    if ((st == CA_TS_DONE || st == CA_TS_FAILED || st == CA_TS_CANCELLED || st == CA_TS_TIMEOUT) &&
        t->finished_ms == 0)
        t->finished_ms = now_ms;
    t->status = st;
}

const char *ca_task_status_name(ca_task_status st) {
    switch (st) {
        case CA_TS_QUEUED:    return "queued";
        case CA_TS_RUNNING:   return "running";
        case CA_TS_DONE:      return "done";
        case CA_TS_FAILED:    return "failed";
        case CA_TS_CANCELLED: return "cancelled";
        case CA_TS_TIMEOUT:   return "timeout";
        default:              return "unknown";
    }
}

char *ca_task_to_json(const ca_task *t) {
    if (!t) return ca_strdup("{}");
    cJSON *o = cJSON_CreateObject();
    if (!o) return ca_strdup("{}");
    cJSON_AddNumberToObject(o, "id", (double)t->id);
    cJSON_AddNumberToObject(o, "priority", t->priority);
    cJSON_AddStringToObject(o, "status", ca_task_status_name(t->status));
    cJSON_AddStringToObject(o, "input", t->input ? t->input : "");
    cJSON_AddStringToObject(o, "output", t->output ? t->output : "");
    cJSON_AddNumberToObject(o, "timeout_ms", (double)t->timeout_ms);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s ? s : ca_strdup("{}");
}
