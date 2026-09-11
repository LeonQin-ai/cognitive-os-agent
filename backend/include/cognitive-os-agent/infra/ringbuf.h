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

typedef struct ringbuf ringbuf;

/* Create a ring buffer with the given capacity (must be a power of two >= 2).
 * Returns NULL on invalid capacity / allocation failure. */
ringbuf *ringbuf_new(size_t capacity);
void ringbuf_free(ringbuf *r);

/* Lock-free enqueue. Returns 1 ok, 0 buffer full, -1 bad arguments. */
int ringbuf_push(ringbuf *r, void *item);

/* Lock-free dequeue. Returns 1 ok (*item set), 0 buffer empty, -1 bad args. */
int ringbuf_pop(ringbuf *r, void **item);

#ifdef __cplusplus
}
#endif
