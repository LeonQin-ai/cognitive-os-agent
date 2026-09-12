/* episode.c — episodic memory store with strength-based lifecycle
 * (reinforce on re-experience, decay with age, drop below a threshold). */
#include "cognitive-os-agent/memory/episode.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

#define EPISODES_CAP 512 /* bounded store: oldest entries dropped first */
#define DECAY_MAX_HALVINGS 30

struct episodic {
    mutex_t mtx;
    char **task;
    char **result;
    long long *ts;    /* ms since epoch */
    double *strength; /* access weight: +1 per re-experience, decays with age */
    size_t count;
    size_t cap;
};

episodic *episodic_new(void) {
    episodic *e = (episodic *)calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    mutex_init(&e->mtx);
    return e;
}

void episodic_free(episodic *e) {
    if (!e)
        return;
    mutex_lock(&e->mtx);
    for (size_t i = 0; i < e->count; i++) {
        free(e->task[i]);
        free(e->result[i]);
    }

    free(e->task);
    free(e->result);
    free(e->ts);
    free(e->strength);
    e->task = e->result = NULL;
    e->ts = NULL;
    e->strength = NULL;
    e->count = e->cap = 0;
    mutex_unlock(&e->mtx);
    mutex_destroy(&e->mtx);
    free(e);
}

void episodic_add_ts(episodic *e, const char *task, const char *result, long long ts) {
    episodic_add_full(e, task, result, ts, 0);
}

void episodic_add_full(episodic *e, const char *task, const char *result, long long ts, double strength) {
    char *t2;
    char *r2;

    if (!e || !task)
        return;
    long long now = ts > 0 ? ts : time_now_ms();
    mutex_lock(&e->mtx);
    /* dedup: same task -> REINFORCE (+1 strength) and refresh instead of dup */
    for (size_t i = e->count; i-- > 0;) {
        if (e->task[i] && strcmp(e->task[i], task) == 0) {
            char *nr = xstrdup(result ? result : "");
            if (nr) {
                free(e->result[i]);
                e->result[i] = nr;
                e->ts[i] = now;
                e->strength[i] += 1.0;
            }
            mutex_unlock(&e->mtx);
            return;
        }
    }

    if (e->count == e->cap) {
        size_t cap = e->cap ? e->cap * 2 : 8;
        char **nt = (char **)realloc(e->task, cap * sizeof(char *));
        char **nr = (char **)realloc(e->result, cap * sizeof(char *));
        long long *ns = (long long *)realloc(e->ts, cap * sizeof(long long));
        double *nw = (double *)realloc(e->strength, cap * sizeof(double));
        if (!nt || !nr || !ns || !nw) {
            free(nt);
            free(nr);
            free(ns);
            free(nw);
            mutex_unlock(&e->mtx);
            return;
        }
        e->task = nt;
        e->result = nr;
        e->ts = ns;
        e->strength = nw;
        e->cap = cap;
    }

    t2 = xstrdup(task);
    r2 = xstrdup(result ? result : "");
    if (!t2 || !r2) {
        free(t2);
        free(r2);
        mutex_unlock(&e->mtx);
        return;
    }

    /* bounded: drop the oldest entry when at capacity */
    if (e->count >= EPISODES_CAP) {
        free(e->task[0]);
        free(e->result[0]);
        memmove(e->task, e->task + 1, (e->count - 1) * sizeof(char *));
        memmove(e->result, e->result + 1, (e->count - 1) * sizeof(char *));
        memmove(e->ts, e->ts + 1, (e->count - 1) * sizeof(long long));
        memmove(e->strength, e->strength + 1, (e->count - 1) * sizeof(double));
        e->count--;
    }

    e->task[e->count] = t2;
    e->result[e->count] = r2;
    e->ts[e->count] = now;
    e->strength[e->count] = strength > 0 ? strength : 1.0;
    e->count++;
    mutex_unlock(&e->mtx);
}

void episodic_add(episodic *e, const char *task, const char *result) {
    episodic_add_full(e, task, result, 0, 0);
}

int episodic_count(episodic *e) {
    int n;

    if (!e)
        return 0;
    mutex_lock(&e->mtx);
    n = (int)e->count;
    mutex_unlock(&e->mtx);
    return n;
}

const char *episodic_task(episodic *e, int i) {
    if (!e || i < 0)
        return NULL;
    mutex_lock(&e->mtx);
    const char *v = ((size_t)i < e->count) ? e->task[i] : NULL;
    mutex_unlock(&e->mtx);
    return v;
}

const char *episodic_result(episodic *e, int i) {
    if (!e || i < 0)
        return NULL;
    mutex_lock(&e->mtx);
    const char *v = ((size_t)i < e->count) ? e->result[i] : NULL;
    mutex_unlock(&e->mtx);
    return v;
}

long long episodic_ts(episodic *e, int i) {
    if (!e || i < 0)
        return 0;
    mutex_lock(&e->mtx);
    long long v = ((size_t)i < e->count) ? e->ts[i] : 0;
    mutex_unlock(&e->mtx);
    return v;
}

double episodic_strength(episodic *e, int i) {
    double v;

    if (!e || i < 0)
        return 0.0;
    mutex_lock(&e->mtx);
    v = ((size_t)i < e->count) ? e->strength[i] : 0.0;
    mutex_unlock(&e->mtx);
    return v;
}

void episodic_reinforce(episodic *e, const char *task) {
    if (!e || !task)
        return;
    mutex_lock(&e->mtx);
    for (size_t i = e->count; i-- > 0;) {
        if (e->task[i] && strcmp(e->task[i], task) == 0) {
            e->strength[i] += 1.0;
            e->ts[i] = time_now_ms();
            break;
        }
    }

    mutex_unlock(&e->mtx);
}

int episodic_decay(episodic *e, long long now_ms, long long half_life_ms, double floor_strength) {
    int decayed = 0;

    if (!e || half_life_ms <= 0)
        return 0;
    if (floor_strength <= 0)
        floor_strength = 0.001;
    mutex_lock(&e->mtx);
    for (size_t i = 0; i < e->count; i++) {
        long long age = now_ms - e->ts[i];
        if (age < half_life_ms)
            continue;
        long long halvings = age / half_life_ms;
        if (halvings > DECAY_MAX_HALVINGS)
            halvings = DECAY_MAX_HALVINGS;
        double ns = e->strength[i];
        for (long long h = 0; h < halvings && ns > floor_strength; h++)
            ns *= 0.5;
        if (ns < floor_strength)
            ns = floor_strength;
        if (ns != e->strength[i]) {
            e->strength[i] = ns;
            decayed++;
        }
    }

    mutex_unlock(&e->mtx);
    return decayed;
}

int episodic_drop_below(episodic *e, double min_strength) {
    int dropped = 0;
    size_t w = 0;

    if (!e || min_strength <= 0)
        return 0;
    mutex_lock(&e->mtx);
    for (size_t i = 0; i < e->count; i++) {
        if (e->strength[i] < min_strength) {
            free(e->task[i]);
            free(e->result[i]);
            dropped++;
            continue;
        }
        e->task[w] = e->task[i];
        e->result[w] = e->result[i];
        e->ts[w] = e->ts[i];
        e->strength[w] = e->strength[i];
        w++;
    }

    e->count = w;
    mutex_unlock(&e->mtx);
    return dropped;
}

char *episodic_below_json(episodic *e, double min_strength) {
    cJSON *arr;
    char *s;

    if (!e)
        return xstrdup("[]");
    mutex_lock(&e->mtx);
    arr = cJSON_CreateArray();
    if (arr) {
        for (size_t i = 0; i < e->count; i++) {
            if (e->strength[i] >= min_strength)
                continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "task", e->task[i]);
            cJSON_AddStringToObject(o, "result", e->result[i]);
            cJSON_AddNumberToObject(o, "ts", (double)e->ts[i]);
            cJSON_AddNumberToObject(o, "strength", e->strength[i]);
            cJSON_AddItemToArray(arr, o);
        }
    }

    s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr)
        cJSON_Delete(arr);
    mutex_unlock(&e->mtx);
    return s ? s : xstrdup("[]");
}

char *episodic_json(episodic *e) {
    cJSON *arr;
    char *s;

    if (!e)
        return xstrdup("[]");
    mutex_lock(&e->mtx);
    arr = cJSON_CreateArray();
    if (arr) {
        for (size_t i = 0; i < e->count; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "task", e->task[i]);
            cJSON_AddStringToObject(o, "result", e->result[i]);
            cJSON_AddNumberToObject(o, "ts", (double)e->ts[i]);
            cJSON_AddNumberToObject(o, "strength", e->strength[i]);
            cJSON_AddItemToArray(arr, o);
        }
    }

    s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr)
        cJSON_Delete(arr);
    mutex_unlock(&e->mtx);
    return s ? s : xstrdup("[]");
}
