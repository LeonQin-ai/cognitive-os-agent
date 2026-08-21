/* episode.c — episodic memory store. */
#include "cagent/memory/episode.h"
#include "cagent/os/os_thread.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

struct ca_episodic {
    ca_mutex mtx;
    char **task;
    char **result;
    size_t count;
    size_t cap;
};

ca_episodic *ca_episodic_new(void) {
    ca_episodic *e = (ca_episodic *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    ca_mutex_init(&e->mtx);
    return e;
}

void ca_episodic_free(ca_episodic *e) {
    if (!e) return;
    ca_mutex_lock(&e->mtx);
    for (size_t i = 0; i < e->count; i++) { free(e->task[i]); free(e->result[i]); }
    free(e->task);
    free(e->result);
    e->task = e->result = NULL;
    e->count = e->cap = 0;
    ca_mutex_unlock(&e->mtx);
    ca_mutex_destroy(&e->mtx);
    free(e);
}

void ca_episodic_add(ca_episodic *e, const char *task, const char *result) {
    if (!e || !task) return;
    ca_mutex_lock(&e->mtx);
    if (e->count == e->cap) {
        size_t cap = e->cap ? e->cap * 2 : 8;
        char **nt = (char **)realloc(e->task, cap * sizeof(char *));
        char **nr = (char **)realloc(e->result, cap * sizeof(char *));
        if (!nt || !nr) { ca_mutex_unlock(&e->mtx); return; }
        e->task = nt;
        e->result = nr;
        e->cap = cap;
    }
    e->task[e->count] = ca_strdup(task);
    e->result[e->count] = ca_strdup(result ? result : "");
    e->count++;
    ca_mutex_unlock(&e->mtx);
}

int ca_episodic_count(ca_episodic *e) {
    if (!e) return 0;
    ca_mutex_lock(&e->mtx);
    int n = (int)e->count;
    ca_mutex_unlock(&e->mtx);
    return n;
}

const char *ca_episodic_task(ca_episodic *e, int i) {
    if (!e || i < 0) return NULL;
    ca_mutex_lock(&e->mtx);
    const char *v = ((size_t)i < e->count) ? e->task[i] : NULL;
    ca_mutex_unlock(&e->mtx);
    return v;
}

const char *ca_episodic_result(ca_episodic *e, int i) {
    if (!e || i < 0) return NULL;
    ca_mutex_lock(&e->mtx);
    const char *v = ((size_t)i < e->count) ? e->result[i] : NULL;
    ca_mutex_unlock(&e->mtx);
    return v;
}

char *ca_episodic_json(ca_episodic *e) {
    if (!e) return ca_strdup("[]");
    ca_mutex_lock(&e->mtx);
    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (size_t i = 0; i < e->count; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "task", e->task[i]);
            cJSON_AddStringToObject(o, "result", e->result[i]);
            cJSON_AddItemToArray(arr, o);
        }
    }
    char *s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr) cJSON_Delete(arr);
    ca_mutex_unlock(&e->mtx);
    return s ? s : ca_strdup("[]");
}
