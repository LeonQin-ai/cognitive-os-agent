#include "cagent/runtime/event_bus.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/os_time.h"

#include <stdlib.h>
#include <string.h>

typedef struct subscription {
    int type;                  /* -1 = all */
    ca_event_handler fn;
    void *ud;
} subscription;

struct ca_event_bus {
    subscription *subs;
    size_t count, cap;
    ca_mutex mtx;
};

ca_event_bus *ca_event_bus_new(void) {
    ca_event_bus *b = calloc(1, sizeof(ca_event_bus));
    if (b) ca_mutex_init(&b->mtx);
    return b;
}

void ca_event_bus_free(ca_event_bus *b) {
    if (!b) return;
    free(b->subs);
    ca_mutex_destroy(&b->mtx);
    free(b);
}

int ca_event_bus_subscribe(ca_event_bus *b, int type, ca_event_handler fn, void *ud) {
    if (!b || !fn) return -1;
    ca_mutex_lock(&b->mtx);
    if (b->count == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8;
        subscription *ns = realloc(b->subs, cap * sizeof(subscription));
        if (!ns) { ca_mutex_unlock(&b->mtx); return -1; }
        b->subs = ns;
        b->cap = cap;
    }
    b->subs[b->count].type = type;
    b->subs[b->count].fn = fn;
    b->subs[b->count].ud = ud;
    int id = (int)b->count;
    b->count++;
    ca_mutex_unlock(&b->mtx);
    return id;
}

void ca_event_bus_publish(ca_event_bus *b, ca_event_type type, const char *source, cJSON *payload) {
    if (!b) { if (payload) cJSON_Delete(payload); return; }
    /* snapshot subscribers under lock */
    ca_mutex_lock(&b->mtx);
    size_t n = b->count;
    subscription *snap = NULL;
    if (n) {
        snap = malloc(n * sizeof(subscription));
        if (snap) memcpy(snap, b->subs, n * sizeof(subscription));
    }
    ca_mutex_unlock(&b->mtx);

    if (!snap) {
        if (payload) cJSON_Delete(payload);
        return;
    }

    ca_event ev;
    ev.type = type;
    ev.source = source;
    ev.ts_ms = ca_time_now_ms();
    ev.payload = payload;

    for (size_t i = 0; i < n; i++) {
        if (snap[i].type == (int)type || snap[i].type == -1)
            snap[i].fn(&ev, snap[i].ud);
    }
    free(snap);
    if (payload) cJSON_Delete(payload);
}

void ca_event_bus_publish_json(ca_event_bus *b, ca_event_type type, const char *source, const char *json_text) {
    cJSON *p = json_text ? cJSON_Parse(json_text) : NULL;
    if (!p && json_text) p = cJSON_CreateString(json_text);
    ca_event_bus_publish(b, type, source, p);
}
