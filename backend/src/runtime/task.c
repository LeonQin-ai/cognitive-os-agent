/* task.c — standalone task lifecycle helpers. */
#include "cognitive-os-agent/runtime/task.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

coa_task *coa_task_new(int64_t id, int priority, const char *input, int64_t timeout_ms) {
    coa_task *t = (coa_task *)calloc(1, sizeof(coa_task));
    if (!t) return NULL;
    t->id = id;
    t->priority = priority;
    t->timeout_ms = timeout_ms;
    t->created_ms = coa_time_now_ms();
    t->status = COA_TS_QUEUED;
    t->input = input ? coa_strdup(input) : NULL;
    return t;
}

void coa_task_free(coa_task *t) {
    if (!t) return;
    free(t->input);
    free(t->output);
    free(t);
}

void coa_task_transition(coa_task *t, coa_task_status st, int64_t now_ms) {
    if (!t) return;
    if (now_ms == 0) now_ms = coa_time_now_ms();
    if (st == COA_TS_RUNNING && t->started_ms == 0) t->started_ms = now_ms;
    if ((st == COA_TS_DONE || st == COA_TS_FAILED || st == COA_TS_CANCELLED || st == COA_TS_TIMEOUT) &&
        t->finished_ms == 0)
        t->finished_ms = now_ms;
    t->status = st;
}

const char *coa_task_status_name(coa_task_status st) {
    switch (st) {
        case COA_TS_QUEUED:    return "queued";
        case COA_TS_RUNNING:   return "running";
        case COA_TS_DONE:      return "done";
        case COA_TS_FAILED:    return "failed";
        case COA_TS_CANCELLED: return "cancelled";
        case COA_TS_TIMEOUT:   return "timeout";
        default:              return "unknown";
    }
}

char *coa_task_to_json(const coa_task *t) {
    if (!t) return coa_strdup("{}");
    cJSON *o = cJSON_CreateObject();
    if (!o) return coa_strdup("{}");
    cJSON_AddNumberToObject(o, "id", (double)t->id);
    cJSON_AddNumberToObject(o, "priority", t->priority);
    cJSON_AddStringToObject(o, "status", coa_task_status_name(t->status));
    cJSON_AddStringToObject(o, "input", t->input ? t->input : "");
    cJSON_AddStringToObject(o, "output", t->output ? t->output : "");
    cJSON_AddNumberToObject(o, "timeout_ms", (double)t->timeout_ms);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s ? s : coa_strdup("{}");
}
