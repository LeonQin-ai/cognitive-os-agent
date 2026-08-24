/* event_bus.c — publish/subscribe event bus backed by a lock-free MPMC ring
 * buffer (src/infra/ringbuf.c). Publishing is lock-free: the event is
 * enqueued into the ring, then the publishing thread tries to become the
 * batch dispatcher via an atomic "draining" flag. Whichever thread wins
 * drains the ring and fans out to subscribers. The subscription registry is
 * only mutated on subscribe/free (rare), so it keeps a small mutex.
 * Events: SYSTEM, TASK, MEMORY, TOOL, MODEL. Payloads are cJSON objects;
 * ownership transfers to the bus and is released after dispatch. */
#include "cagent/runtime/event_bus.h"
#include "cagent/infra/ringbuf.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define RING_CAPACITY 1024

typedef struct subscription {
    int type;                  /* -1 = all */
    ca_event_handler fn;
    void *ud;
} subscription;

struct ca_event_bus {
    ca_ringbuf *queue;         /* lock-free MPMC ring of ca_event* */
    subscription *subs;
    size_t count, cap;
    ca_mutex sub_mtx;          /* guards subs array only (rare mutation) */
    _Atomic int draining;      /* 1 = a thread is draining/dispatching */
};

ca_event_bus *ca_event_bus_new(void) {
    ca_event_bus *b = calloc(1, sizeof(ca_event_bus));
    if (!b) return NULL;
    b->queue = ca_ringbuf_new(RING_CAPACITY);
    if (!b->queue) { free(b); return NULL; }
    ca_mutex_init(&b->sub_mtx);
    atomic_init(&b->draining, 0);
    return b;
}

void ca_event_bus_free(ca_event_bus *b) {
    if (!b) return;
    /* drain and free any pending events */
    void *it;
    while (ca_ringbuf_pop(b->queue, &it) == 1) {
        ca_event *ev = (ca_event *)it;
        if (ev->payload) cJSON_Delete(ev->payload);
        free(ev);
    }
    ca_ringbuf_free(b->queue);
    free(b->subs);
    ca_mutex_destroy(&b->sub_mtx);
    free(b);
}

int ca_event_bus_subscribe(ca_event_bus *b, int type, ca_event_handler fn, void *ud) {
    if (!b || !fn) return -1;
    ca_mutex_lock(&b->sub_mtx);
    if (b->count == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8;
        subscription *ns = realloc(b->subs, cap * sizeof(subscription));
        if (!ns) { ca_mutex_unlock(&b->sub_mtx); return -1; }
        b->subs = ns;
        b->cap = cap;
    }
    b->subs[b->count].type = type;
    b->subs[b->count].fn = fn;
    b->subs[b->count].ud = ud;
    int id = (int)b->count;
    b->count++;
    ca_mutex_unlock(&b->sub_mtx);
    return id;
}

/* Dispatch one event to a snapshot of matching subscribers. The snapshot is
 * taken under the subscription lock; dispatch itself runs unlocked. */
static void dispatch_one(ca_event_bus *b, ca_event *ev) {
    subscription *snap = NULL;
    size_t n = 0;
    ca_mutex_lock(&b->sub_mtx);
    n = b->count;
    if (n) {
        snap = malloc(n * sizeof(subscription));
        if (snap) memcpy(snap, b->subs, n * sizeof(subscription));
        else n = 0;
    }
    ca_mutex_unlock(&b->sub_mtx);
    if (!snap) return;
    for (size_t i = 0; i < n; i++) {
        if (snap[i].type == (int)ev->type || snap[i].type == -1)
            snap[i].fn(ev, snap[i].ud);
    }
    free(snap);
}

/* Drain the ring and dispatch every event. One thread wins the draining flag;
 * late producers re-check the flag so no event is stranded. */
static void drain(ca_event_bus *b) {
    for (;;) {
        int expected = 0;
        if (!atomic_compare_exchange_strong_explicit(&b->draining, &expected, 1,
                                                     memory_order_acq_rel,
                                                     memory_order_acquire)) {
            return; /* another thread is draining; it will pick up our events */
        }
        void *it;
        while (ca_ringbuf_pop(b->queue, &it) == 1) {
            ca_event *ev = (ca_event *)it;
            dispatch_one(b, ev);
            if (ev->payload) cJSON_Delete(ev->payload);
            free(ev);
        }
        atomic_store_explicit(&b->draining, 0, memory_order_release);
        /* A producer may have enqueued between our last pop and clearing the
         * flag. Re-check once; if something arrived, loop and drain again. */
        if (ca_ringbuf_pop(b->queue, &it) != 1) break;
        /* put it back so the loop drains it in order */
        ca_ringbuf_push(b->queue, it);
    }
}

void ca_event_bus_publish(ca_event_bus *b, ca_event_type type, const char *source, cJSON *payload) {
    if (!b) { if (payload) cJSON_Delete(payload); return; }
    ca_event *ev = malloc(sizeof(ca_event));
    if (!ev) { if (payload) cJSON_Delete(payload); return; }
    ev->type = type;
    ev->source = source;
    ev->ts_ms = ca_time_now_ms();
    ev->payload = payload;
    if (ca_ringbuf_push(b->queue, ev) != 1) {
        /* ring full: fall back to a synchronous best-effort dispatch */
        free(ev);
        if (payload) cJSON_Delete(payload);
        return;
    }
    drain(b);
}

void ca_event_bus_publish_json(ca_event_bus *b, ca_event_type type, const char *source, const char *json_text) {
    cJSON *p = json_text ? cJSON_Parse(json_text) : NULL;
    if (!p && json_text) p = cJSON_CreateString(json_text);
    ca_event_bus_publish(b, type, source, p);
}
