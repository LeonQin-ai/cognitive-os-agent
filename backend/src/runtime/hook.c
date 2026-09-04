/* hook.c — horizontal hook system: named-event callback registry with
 * before/after semantics (see hook.h). Thread-safe: hooks may be registered
 * from the REST layer while the scheduler dispatches from worker threads. */
#include "cagent/runtime/hook.h"
#include "cagent/infra/util.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/os_time.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct ca_hook {
    int id;
    char *event;
    ca_hook_fn fn;
    void *ud;
    struct ca_hook *next;
} ca_hook;

struct ca_hook_registry {
    ca_mutex mtx;
    ca_hook *head;
    int next_id;
};

ca_hook_registry *ca_hook_registry_new(void) {
    ca_hook_registry *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    ca_mutex_init(&h->mtx);
    h->next_id = 1;
    return h;
}

void ca_hook_registry_free(ca_hook_registry *h) {
    if (!h) return;
    ca_mutex_lock(&h->mtx);
    ca_hook *c = h->head;
    while (c) {
        ca_hook *n = c->next;
        free(c->event);
        free(c);
        c = n;
    }
    ca_mutex_unlock(&h->mtx);
    ca_mutex_destroy(&h->mtx);
    free(h);
}

int ca_hook_register(ca_hook_registry *h, const char *event, ca_hook_fn fn, void *ud) {
    if (!h || !event || !*event || !fn) return -1;
    ca_hook *hk = calloc(1, sizeof(*hk));
    if (!hk) return -1;
    hk->event = ca_strdup(event);
    if (!hk->event) { free(hk); return -1; }
    ca_mutex_lock(&h->mtx);
    hk->id = h->next_id++;
    hk->fn = fn;
    hk->ud = ud;
    hk->next = h->head;
    h->head = hk;
    int id = hk->id;
    ca_mutex_unlock(&h->mtx);
    return id;
}

int ca_hook_unregister(ca_hook_registry *h, int id) {
    if (!h || id <= 0) return -1;
    ca_mutex_lock(&h->mtx);
    ca_hook **pp = &h->head;
    while (*pp) {
        if ((*pp)->id == id) {
            ca_hook *dead = *pp;
            *pp = dead->next;
            free(dead->event);
            free(dead);
            ca_mutex_unlock(&h->mtx);
            return 0;
        }
        pp = &(*pp)->next;
    }
    ca_mutex_unlock(&h->mtx);
    return -1;
}

int ca_hook_dispatch(ca_hook_registry *h, const char *event, const char *payload_json) {
    if (!h || !event || !*event) return -1;
    int blocked = 0;
    /* registration order is reversed (head insert); collect matching ids first
     * under the lock, then fire outside it so a hook may register/unregister */
    int ids[64];
    ca_hook_fn fns[64];
    void *uds[64];
    int n = 0;
    ca_mutex_lock(&h->mtx);
    for (ca_hook *c = h->head; c && n < 64; c = c->next) {
        if (strcmp(c->event, event) == 0 || strcmp(c->event, "*") == 0) {
            ids[n] = c->id;
            fns[n] = c->fn;
            uds[n] = c->ud;
            n++;
        }
    }
    ca_mutex_unlock(&h->mtx);
    for (int i = n - 1; i >= 0; i--) { /* fire in registration order */
        int rc = fns[i](event, payload_json, uds[i]);
        if (rc != 0) blocked = 1;
    }
    return blocked ? 1 : 0;
}

char *ca_hook_registry_json(ca_hook_registry *h) {
    if (!h) return ca_strdup("[]");
    ca_mutex_lock(&h->mtx);
    cJSON *arr = cJSON_CreateArray();
    for (ca_hook *c = h->head; c; c = c->next) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", c->id);
        cJSON_AddStringToObject(o, "event", c->event);
        cJSON_AddItemToArray(arr, o);
    }
    ca_mutex_unlock(&h->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : ca_strdup("[]");
}

int ca_hook_audit_file(const char *event, const char *payload_json, void *ud) {
    const char *path = ud;
    if (!path || !event) return 0;
    FILE *f = fopen(path, "a");
    if (!f) return 0;
    cJSON *o = cJSON_CreateObject();
    if (o) {
        cJSON_AddNumberToObject(o, "ts_ms", (double)ca_time_now_ms());
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
