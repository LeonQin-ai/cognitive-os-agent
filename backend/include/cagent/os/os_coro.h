/* os_coro.h — portable stackful coroutine primitive.
 * Linux: ucontext (getcontext/makecontext/swapcontext). Windows: Fiber.
 *
 * A coroutine runs `fn(arg)` on its own stack. It cooperatively yields back to
 * whoever resumed it via ca_coro_yield(); when `fn` returns the coroutine is
 * "done". Only one coroutine runs per OS thread at a time (they never nest
 * yield-to-each-other — a coroutine always yields to the resumer).
 *
 * Thread-safety: a ca_coro must be resumed from a single thread at a time.
 * The resumer's return point is tracked per-thread via TLS (_Thread_local).
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_coro ca_coro;
typedef void (*ca_coro_fn)(void *arg);

/* Default coroutine stack size (256 KB — deep reasoning + cJSON + HTTP frames). */
#define CA_CORO_STACK_DEFAULT (256u * 1024u)

/* Create a coroutine. fn runs on first ca_coro_resume. stack_size 0 = default. */
ca_coro *ca_coro_new(ca_coro_fn fn, void *arg, size_t stack_size);

/* Free a finished coroutine (must be done; free also frees the stack). */
void ca_coro_free(ca_coro *c);

/* Run/switch into the coroutine. Returns when it yields or finishes. */
void ca_coro_resume(ca_coro *c);

/* Yield back to the resumer. No-op if not running inside a coroutine. */
void ca_coro_yield(void);

/* 1 if fn has returned, 0 otherwise. */
int ca_coro_done(const ca_coro *c);

#ifdef __cplusplus
}
#endif
