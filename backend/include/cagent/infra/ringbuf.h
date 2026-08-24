/* ringbuf.h — bounded lock-free MPMC ring buffer.
 *
 * Classic Dmitry Vyukov bounded MPMC queue: producers and consumers advance
 * per-slot sequence numbers with C11 atomics; no locks on the hot path.
 * Slots hold void* payloads (heap pointers transferred to the consumer).
 * Capacity must be a power of two. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_ringbuf ca_ringbuf;

/* Create a ring buffer with the given capacity (must be a power of two >= 2).
 * Returns NULL on invalid capacity / allocation failure. */
ca_ringbuf *ca_ringbuf_new(size_t capacity);
void ca_ringbuf_free(ca_ringbuf *r);

/* Lock-free enqueue. Returns 1 ok, 0 buffer full, -1 bad arguments. */
int ca_ringbuf_push(ca_ringbuf *r, void *item);

/* Lock-free dequeue. Returns 1 ok (*item set), 0 buffer empty, -1 bad args. */
int ca_ringbuf_pop(ca_ringbuf *r, void **item);

size_t ca_ringbuf_capacity(ca_ringbuf *r);

#ifdef __cplusplus
}
#endif
