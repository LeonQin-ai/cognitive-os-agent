/* hook.c — horizontal hook system: named-event callback registry with
 * before/after semantics (see hook.h). Thread-safe: hooks may be registered
 * from the REST layer while the scheduler dispatches from worker threads. */
#include "cognitive-os-agent/runtime/hook.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct hook {
    int id;
    char *event;
    hook_fn fn;
    void *ud;
    struct hook *next;
} hook;

struct hook_registry {
    mutex_t mtx;
    hook *head;
    int next_id;
};

hook_registry *hook_registry_new(void) {
    hook_registry *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    mutex_init(&h->mtx);
    h->next_id = 1;
    return h;
}

void hook_registry_free(hook_registry *h) {
    if (!h)
        return;
    mutex_lock(&h->mtx);
    hook *c = h->head;
    while (c) {
        hook *n = c->next;
        free(c->event);
        free(c);
        c = n;
    }

    mutex_unlock(&h->mtx);
    mutex_destroy(&h->mtx);
    free(h);
}

int hook_register(hook_registry *h, const char *event, hook_fn fn, void *ud) {
    int id;

    if (!h || !event || !*event || !fn)
        return -1;
    hook *hk = calloc(1, sizeof(*hk));
    if (!hk)
        return -1;
    hk->event = xstrdup(event);
    if (!hk->event) {
        free(hk);
        return -1;
    }

    mutex_lock(&h->mtx);
    hk->id = h->next_id++;
    hk->fn = fn;
    hk->ud = ud;
    hk->next = h->head;
    h->head = hk;
    id = hk->id;
    mutex_unlock(&h->mtx);
    return id;
}

int hook_unregister(hook_registry *h, int id) {
    if (!h || id <= 0)
        return -1;
    mutex_lock(&h->mtx);
    hook **pp = &h->head;
    while (*pp) {
        if ((*pp)->id == id) {
            hook *dead = *pp;
            *pp = dead->next;
            free(dead->event);
            free(dead);
            mutex_unlock(&h->mtx);
            return 0;
        }
        pp = &(*pp)->next;
    }

    mutex_unlock(&h->mtx);
    return -1;
}

int hook_dispatch(hook_registry *h, const char *event, const char *payload_json) {
    int blocked = 0;
    int ids[64];
    void *uds[64];
    int n = 0;

    if (!h || !event || !*event)
        return -1;
    /* registration order is reversed (head insert); collect matching ids first
     * under the lock, then fire outside it so a hook may register/unregister */
    hook_fn fns[64];
    mutex_lock(&h->mtx);
    for (hook *c = h->head; c && n < 64; c = c->next) {
        if (strcmp(c->event, event) == 0 || strcmp(c->event, "*") == 0) {
            ids[n] = c->id;
            fns[n] = c->fn;
            uds[n] = c->ud;
            n++;
        }
    }

    mutex_unlock(&h->mtx);
    for (int i = n - 1; i >= 0; i--) { /* fire in registration order */
        int rc = fns[i](event, payload_json, uds[i]);
        if (rc != 0)
            blocked = 1;
    }

    return blocked ? 1 : 0;
}

char *hook_registry_json(hook_registry *h) {
    cJSON *arr;
    char *s;

    if (!h)
        return xstrdup("[]");
    mutex_lock(&h->mtx);
    arr = cJSON_CreateArray();
    for (hook *c = h->head; c; c = c->next) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", c->id);
        cJSON_AddStringToObject(o, "event", c->event);
        cJSON_AddItemToArray(arr, o);
    }

    mutex_unlock(&h->mtx);
    s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : xstrdup("[]");
}

int hook_audit_file(const char *event, const char *payload_json, void *ud) {
    const char *path = ud;
    FILE *f;
    cJSON *o;

    if (!path || !event)
        return 0;
    f = fopen(path, "a");
    if (!f)
        return 0;
    o = cJSON_CreateObject();
    if (o) {
        cJSON_AddNumberToObject(o, "ts_ms", (double)time_now_ms());
        cJSON_AddStringToObject(o, "event", event);
        cJSON_AddStringToObject(o, "payload", payload_json ? payload_json : "");
        char *line = cJSON_PrintUnformatted(o);
        if (line) {
            fprintf(f, "%s\n", line);
            free(line);
        }
        cJSON_Delete(o);
    }

    fclose(f);
    return 0; /* audit never blocks */
}
