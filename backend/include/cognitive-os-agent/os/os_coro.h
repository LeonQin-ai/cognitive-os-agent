/* os_coro.h — portable stackful coroutine primitive.
 * Linux: ucontext (getcontext/makecontext/swapcontext). Windows: Fiber.
 *
 * A coroutine runs `fn(arg)` on its own stack. It cooperatively yields back to
 * whoever resumed it via coa_coro_yield(); when `fn` returns the coroutine is
 * "done". Only one coroutine runs per OS thread at a time (they never nest
 * yield-to-each-other — a coroutine always yields to the resumer).
 *
 * Thread-safety: a coa_coro must be resumed from a single thread at a time.
 * The resumer's return point is tracked per-thread via TLS (_Thread_local).
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_coro coa_coro;
typedef void (*coa_coro_fn)(void *arg);

/* Default coroutine stack size (256 KB — deep reasoning + cJSON + HTTP frames). */
#define COA_CORO_STACK_DEFAULT (256u * 1024u)

/* Create a coroutine. fn runs on first coa_coro_resume. stack_size 0 = default. */
coa_coro *coa_coro_new(coa_coro_fn fn, void *arg, size_t stack_size);

/* Free a finished coroutine (must be done; free also frees the stack). */
void coa_coro_free(coa_coro *c);

/* Run/switch into the coroutine. Returns when it yields or finishes. */
void coa_coro_resume(coa_coro *c);

/* Yield back to the resumer. No-op if not running inside a coroutine. */
void coa_coro_yield(void);

/* 1 if fn has returned, 0 otherwise. */
int coa_coro_done(const coa_coro *c);

#ifdef __cplusplus
}
#endif
