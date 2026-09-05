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

typedef struct coa_hook {
    int id;
    char *event;
    coa_hook_fn fn;
    void *ud;
    struct coa_hook *next;
} coa_hook;

struct coa_hook_registry {
    coa_mutex mtx;
    coa_hook *head;
    int next_id;
};

coa_hook_registry *coa_hook_registry_new(void) {
    coa_hook_registry *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    coa_mutex_init(&h->mtx);
    h->next_id = 1;
    return h;
}

void coa_hook_registry_free(coa_hook_registry *h) {
    if (!h) return;
    coa_mutex_lock(&h->mtx);
    coa_hook *c = h->head;
    while (c) {
        coa_hook *n = c->next;
        free(c->event);
        free(c);
        c = n;
    }
    coa_mutex_unlock(&h->mtx);
    coa_mutex_destroy(&h->mtx);
    free(h);
}

int coa_hook_register(coa_hook_registry *h, const char *event, coa_hook_fn fn, void *ud) {
    if (!h || !event || !*event || !fn) return -1;
    coa_hook *hk = calloc(1, sizeof(*hk));
    if (!hk) return -1;
    hk->event = coa_strdup(event);
    if (!hk->event) { free(hk); return -1; }
    coa_mutex_lock(&h->mtx);
    hk->id = h->next_id++;
    hk->fn = fn;
    hk->ud = ud;
    hk->next = h->head;
    h->head = hk;
    int id = hk->id;
    coa_mutex_unlock(&h->mtx);
    return id;
}

int coa_hook_unregister(coa_hook_registry *h, int id) {
    if (!h || id <= 0) return -1;
    coa_mutex_lock(&h->mtx);
    coa_hook **pp = &h->head;
    while (*pp) {
        if ((*pp)->id == id) {
            coa_hook *dead = *pp;
            *pp = dead->next;
            free(dead->event);
            free(dead);
            coa_mutex_unlock(&h->mtx);
            return 0;
        }
        pp = &(*pp)->next;
    }
    coa_mutex_unlock(&h->mtx);
    return -1;
}

int coa_hook_dispatch(coa_hook_registry *h, const char *event, const char *payload_json) {
    if (!h || !event || !*event) return -1;
    int blocked = 0;
    /* registration order is reversed (head insert); collect matching ids first
     * under the lock, then fire outside it so a hook may register/unregister */
    int ids[64];
    coa_hook_fn fns[64];
    void *uds[64];
    int n = 0;
    coa_mutex_lock(&h->mtx);
    for (coa_hook *c = h->head; c && n < 64; c = c->next) {
        if (strcmp(c->event, event) == 0 || strcmp(c->event, "*") == 0) {
            ids[n] = c->id;
            fns[n] = c->fn;
            uds[n] = c->ud;
            n++;
        }
    }
    coa_mutex_unlock(&h->mtx);
    for (int i = n - 1; i >= 0; i--) { /* fire in registration order */
        int rc = fns[i](event, payload_json, uds[i]);
        if (rc != 0) blocked = 1;
    }
    return blocked ? 1 : 0;
}

char *coa_hook_registry_json(coa_hook_registry *h) {
    if (!h) return coa_strdup("[]");
    coa_mutex_lock(&h->mtx);
    cJSON *arr = cJSON_CreateArray();
    for (coa_hook *c = h->head; c; c = c->next) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", c->id);
        cJSON_AddStringToObject(o, "event", c->event);
        cJSON_AddItemToArray(arr, o);
    }
    coa_mutex_unlock(&h->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : coa_strdup("[]");
}

int coa_hook_audit_file(const char *event, const char *payload_json, void *ud) {
    const char *path = ud;
    if (!path || !event) return 0;
    FILE *f = fopen(path, "a");
    if (!f) return 0;
    cJSON *o = cJSON_CreateObject();
    if (o) {
        cJSON_AddNumberToObject(o, "ts_ms", (double)coa_time_now_ms());
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
