/* ringbuf.c — bounded lock-free MPMC ring buffer (Vyukov bounded MPMC).
 * Each slot pairs a sequence number with a data pointer. Producers only
 * write a slot when seq == enqueue_pos; consumers only read when
 * seq == dequeue_pos + 1. Sequence numbers grow monotonically, which makes
 * the full/empty distinction unambiguous and the algorithm ABA-safe. */
#include "cognitive-os-agent/infra/ringbuf.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

struct coa_ringbuf {
    _Atomic(void *) *data; /* slot payloads */
    _Atomic(size_t) *seq; /* per-slot sequence numbers */
    size_t capacity;  /* power of two */
    size_t mask;
    _Atomic size_t enqueue_pos;
    _Atomic size_t dequeue_pos;
};

coa_ringbuf *coa_ringbuf_new(size_t capacity) {
    if (capacity < 2 || (capacity & (capacity - 1)) != 0) return NULL;
    coa_ringbuf *r = (coa_ringbuf *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->data = (_Atomic(void *) *)calloc(capacity, sizeof(_Atomic(void *)));
    r->seq  = (_Atomic(size_t) *)malloc(capacity * sizeof(_Atomic(size_t)));
    if (!r->data || !r->seq) {
        free(r->data);
        free(r->seq);
        free(r);
        return NULL;
    }
    r->capacity = capacity;
    r->mask = capacity - 1;
    for (size_t i = 0; i < capacity; i++) r->seq[i] = i;
    atomic_init(&r->enqueue_pos, 0);
    atomic_init(&r->dequeue_pos, 0);
    return r;
}

void coa_ringbuf_free(coa_ringbuf *r) {
    if (!r) return;
    free(r->data);
    free(r->seq);
    free(r);
}

size_t coa_ringbuf_capacity(coa_ringbuf *r) {
    return r ? r->capacity : 0;
}

int coa_ringbuf_push(coa_ringbuf *r, void *item) {
    if (!r || !item) return -1;
    const size_t mask = r->mask;
    size_t pos = atomic_load_explicit(&r->enqueue_pos, memory_order_relaxed);
    for (;;) {
        const size_t cell = pos & mask;
        const size_t seq = atomic_load_explicit(&r->seq[cell], memory_order_acquire);
        const intptr_t diff = (intptr_t)seq - (intptr_t)pos;
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&r->enqueue_pos, &pos,
                                                      pos + 1, memory_order_relaxed,
                                                      memory_order_relaxed)) {
                atomic_store_explicit(&r->data[cell], item, memory_order_release);
                atomic_store_explicit(&r->seq[cell], pos + 1, memory_order_release);
                return 1;
            }
        } else if (diff < 0) {
            return 0; /* full */
        } else {
            pos = atomic_load_explicit(&r->enqueue_pos, memory_order_relaxed);
        }
    }
}

int coa_ringbuf_pop(coa_ringbuf *r, void **out) {
    if (!r || !out) return -1;
    const size_t mask = r->mask;
    size_t pos = atomic_load_explicit(&r->dequeue_pos, memory_order_relaxed);
    for (;;) {
        const size_t cell = pos & mask;
        const size_t seq = atomic_load_explicit(&r->seq[cell], memory_order_acquire);
        const intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&r->dequeue_pos, &pos,
                                                      pos + 1, memory_order_relaxed,
                                                      memory_order_relaxed)) {
                *out = atomic_load_explicit(&r->data[cell], memory_order_relaxed);
                atomic_store_explicit(&r->seq[cell], pos + mask + 1, memory_order_release);
                return 1;
            }
        } else if (diff < 0) {
            return 0; /* empty */
        } else {
            pos = atomic_load_explicit(&r->dequeue_pos, memory_order_relaxed);
        }
    }
}
