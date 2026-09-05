/* usage.c — per-model token accounting. */
#include "cognitive-os-agent/llm/usage.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

typedef struct model_entry {
    char *model;
    long prompt, completion, calls;
} model_entry;

struct coa_usage {
    coa_mutex mtx;
    model_entry *models;
    size_t count, cap;
    long prompt_total, completion_total;
};

coa_usage *coa_usage_new(void) {
    coa_usage *u = (coa_usage *)calloc(1, sizeof(coa_usage));
    if (!u) return NULL;
    coa_mutex_init(&u->mtx);
    return u;
}

void coa_usage_free(coa_usage *u) {
    if (!u) return;
    coa_mutex_lock(&u->mtx);
    for (size_t i = 0; i < u->count; i++) free(u->models[i].model);
    free(u->models);
    coa_mutex_unlock(&u->mtx);
    coa_mutex_destroy(&u->mtx);
    free(u);
}

void coa_usage_add(coa_usage *u, const char *model, long prompt_tokens, long completion_tokens) {
    if (!u || !model) return;
    coa_mutex_lock(&u->mtx);
    model_entry *e = NULL;
    for (size_t i = 0; i < u->count; i++)
        if (strcmp(u->models[i].model, model) == 0) { e = &u->models[i]; break; }
    if (!e) {
        if (u->count == u->cap) {
            size_t ncap = u->cap ? u->cap * 2 : 8;
            model_entry *nm = (model_entry *)realloc(u->models, ncap * sizeof(model_entry));
            if (!nm) { coa_mutex_unlock(&u->mtx); return; }
            u->models = nm;
            u->cap = ncap;
        }
        e = &u->models[u->count++];
        memset(e, 0, sizeof(*e));
        e->model = coa_strdup(model);
    }
    e->prompt += prompt_tokens;
    e->completion += completion_tokens;
    e->calls++;
    u->prompt_total += prompt_tokens;
    u->completion_total += completion_tokens;
    coa_mutex_unlock(&u->mtx);
}

long coa_usage_prompt_total(coa_usage *u) {
    if (!u) return 0;
    coa_mutex_lock(&u->mtx);
    long v = u->prompt_total;
    coa_mutex_unlock(&u->mtx);
    return v;
}

long coa_usage_completion_total(coa_usage *u) {
    if (!u) return 0;
    coa_mutex_lock(&u->mtx);
    long v = u->completion_total;
    coa_mutex_unlock(&u->mtx);
    return v;
}

char *coa_usage_json(coa_usage *u) {
    cJSON *root = cJSON_CreateObject();
    cJSON *models = cJSON_CreateObject();
    if (u) {
        coa_mutex_lock(&u->mtx);
        for (size_t i = 0; i < u->count; i++) {
            model_entry *e = &u->models[i];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "prompt", (double)e->prompt);
            cJSON_AddNumberToObject(o, "completion", (double)e->completion);
            cJSON_AddNumberToObject(o, "calls", (double)e->calls);
            cJSON_AddItemToObject(models, e->model, o);
        }
        cJSON *tot = cJSON_CreateObject();
        cJSON_AddNumberToObject(tot, "prompt", (double)u->prompt_total);
        cJSON_AddNumberToObject(tot, "completion", (double)u->completion_total);
        coa_mutex_unlock(&u->mtx);
        cJSON_AddItemToObject(root, "total", tot);
    } else {
        cJSON *tot = cJSON_CreateObject();
        cJSON_AddNumberToObject(tot, "prompt", 0);
        cJSON_AddNumberToObject(tot, "completion", 0);
        cJSON_AddItemToObject(root, "total", tot);
    }
    cJSON_AddItemToObject(root, "models", models);
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return js;
}
