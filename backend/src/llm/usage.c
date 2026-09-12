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

struct usage {
    mutex_t mtx;
    model_entry *models;
    size_t count, cap;
    long prompt_total, completion_total;
};

usage *usage_new(void) {
    usage *u = (usage *)calloc(1, sizeof(usage));
    if (!u)
        return NULL;
    mutex_init(&u->mtx);
    return u;
}

void usage_free(usage *u) {
    if (!u)
        return;
    mutex_lock(&u->mtx);
    for (size_t i = 0; i < u->count; i++)
        free(u->models[i].model);
    free(u->models);
    mutex_unlock(&u->mtx);
    mutex_destroy(&u->mtx);
    free(u);
}

void usage_add(usage *u, const char *model, long prompt_tokens, long completion_tokens) {
    if (!u || !model)
        return;
    mutex_lock(&u->mtx);
    model_entry *e = NULL;
    for (size_t i = 0; i < u->count; i++)
        if (strcmp(u->models[i].model, model) == 0) {
            e = &u->models[i];
            break;
        }

    if (!e) {
        if (u->count == u->cap) {
            size_t ncap = u->cap ? u->cap * 2 : 8;
            model_entry *nm = (model_entry *)realloc(u->models, ncap * sizeof(model_entry));
            if (!nm) {
                mutex_unlock(&u->mtx);
                return;
            }
            u->models = nm;
            u->cap = ncap;
        }
        e = &u->models[u->count++];
        memset(e, 0, sizeof(*e));
        e->model = xstrdup(model);
    }

    e->prompt += prompt_tokens;
    e->completion += completion_tokens;
    e->calls++;
    u->prompt_total += prompt_tokens;
    u->completion_total += completion_tokens;
    mutex_unlock(&u->mtx);
}

long usage_prompt_total(usage *u) {
    long v;

    if (!u)
        return 0;
    mutex_lock(&u->mtx);
    v = u->prompt_total;
    mutex_unlock(&u->mtx);
    return v;
}

long usage_completion_total(usage *u) {
    long v;

    if (!u)
        return 0;
    mutex_lock(&u->mtx);
    v = u->completion_total;
    mutex_unlock(&u->mtx);
    return v;
}

char *usage_json(usage *u) {
    cJSON *root = cJSON_CreateObject();
    cJSON *models = cJSON_CreateObject();
    char *js;

    if (u) {
        mutex_lock(&u->mtx);
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
        mutex_unlock(&u->mtx);
        cJSON_AddItemToObject(root, "total", tot);
    } else {
        cJSON *tot = cJSON_CreateObject();
        cJSON_AddNumberToObject(tot, "prompt", 0);
        cJSON_AddNumberToObject(tot, "completion", 0);
        cJSON_AddItemToObject(root, "total", tot);
    }

    cJSON_AddItemToObject(root, "models", models);
    js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return js;
}
