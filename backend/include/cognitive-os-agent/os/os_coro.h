/* os_coro.h — portable stackful coroutine primitive.
 * Linux: ucontext (getcontext/makecontext/swapcontext). Windows: Fiber.
 *
 * A coroutine runs `fn(arg)` on its own stack. It cooperatively yields back to
 * whoever resumed it via coro_yield(); when `fn` returns the coroutine is
 * "done". Only one coroutine runs per OS thread at a time (they never nest
 * yield-to-each-other — a coroutine always yields to the resumer).
 *
 * Thread-safety: a coro must be resumed from a single thread at a time.
 * The resumer's return point is tracked per-thread via TLS (_Thread_local).
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coro coro;
typedef void (*coro_fn)(void *arg);

/* Default coroutine stack size (256 KB — deep reasoning + cJSON + HTTP frames). */
#define CORO_STACK_DEFAULT (256u * 1024u)

/* Create a coroutine. fn runs on first coro_resume. stack_size 0 = default. */
coro *coro_new(coro_fn fn, void *arg, size_t stack_size);

/* Free a finished coroutine (must be done; free also frees the stack). */
void coro_free(coro *c);

/* Run/switch into the coroutine. Returns when it yields or finishes. */
void coro_resume(coro *c);

/* Yield back to the resumer. No-op if not running inside a coroutine. */
void coro_yield(void);

/* 1 if fn has returned, 0 otherwise. */
int coro_done(const coro *c);

#ifdef __cplusplus
}
#endif
