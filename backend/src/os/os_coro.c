/* os_coro.c — stackful coroutines: ucontext (POSIX) / Fiber (Windows). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cognitive-os-agent/os/os_coro.h"

#include <stdlib.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct coro {
    LPVOID fiber;
    coro_fn fn;
    void *arg;
    int done;
};

/* Thread-local: the thread's main fiber (created lazily on first resume) and
 * the currently-running coroutine. Both are thread-level (all fibers of a
 * thread share its TLS), which is exactly the semantics we need. */
static _Thread_local LPVOID tls_main_fiber = NULL;

static void WINAPI fiber_proc(LPVOID p) {
    coro *c = (coro *)p;
    c->fn(c->arg);
    c->done = 1;
    /* A fiber must not return; switch back to the main fiber explicitly. */
    SwitchToFiber(tls_main_fiber);
}

coro *coro_new(coro_fn fn, void *arg, size_t stack_size) {
    coro *c = (coro *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fn = fn;
    c->arg = arg;
    c->fiber = CreateFiber(stack_size ? stack_size : CORO_STACK_DEFAULT, fiber_proc, c);
    if (!c->fiber) {
        free(c);
        return NULL;
    }
    return c;
}

void coro_free(coro *c) {
    if (!c)
        return;
    if (c->fiber)
        DeleteFiber(c->fiber);
    free(c);
}

void coro_resume(coro *c) {
    if (!c || c->done)
        return;
    if (!tls_main_fiber) {
        /* Convert this thread into a fiber; it becomes the "main" fiber. */
        ConvertThreadToFiber(NULL);
        tls_main_fiber = GetCurrentFiber();
    }
    SwitchToFiber(c->fiber);
}

void coro_yield(void) {
    if (tls_main_fiber)
        SwitchToFiber(tls_main_fiber);
}

int coro_done(const coro *c) {
    return c ? c->done : 1;
}

#else /* POSIX (ucontext) */

#include <ucontext.h>

/* getcontext/makecontext/setcontext use non-local jumps internally, so gcc's
 * -Wclobbered warns that locals may be clobbered across them. The accesses here
 * are safe: swapcontext is only reached from resume/yield after those locals are
 * no longer live. Silence the false positive for this translation unit. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclobbered"

struct coro {
    ucontext_t ctx;
    char *stack;
    coro_fn fn;
    void *arg;
    int done;
    ucontext_t *resume_ctx; /* where to swap back to on yield/finish */
};

static _Thread_local ucontext_t tls_main_ctx;
static _Thread_local coro *tls_current = NULL;

/* Entry trampoline. Reads the coroutine pointer from TLS (set by resume just
 * before swapcontext) — avoids passing a pointer through makecontext's int
 * varargs, which truncates on 64-bit. */
static void coro_entry(void) {
    coro *c = tls_current;
    c->fn(c->arg);
    c->done = 1;
    tls_current = NULL;
    swapcontext(&c->ctx, c->resume_ctx); /* never returns */
}

coro *coro_new(coro_fn fn, void *arg, size_t stack_size) {
    coro *c = (coro *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    size_t sz = stack_size ? stack_size : CORO_STACK_DEFAULT;
    c->stack = (char *)malloc(sz);
    if (!c->stack) {
        free(c);
        return NULL;
    }
    c->fn = fn;
    c->arg = arg;
    if (getcontext(&c->ctx) != 0) {
        free(c->stack);
        free(c);
        return NULL;
    }
    c->ctx.uc_stack.ss_sp = c->stack;
    c->ctx.uc_stack.ss_size = sz;
    c->ctx.uc_link = NULL;
    makecontext(&c->ctx, (void (*)(void))coro_entry, 0);
    return c;
}

void coro_free(coro *c) {
    if (!c)
        return;
    free(c->stack);
    free(c);
}

void coro_resume(coro *c) {
    if (!c || c->done)
        return;
    c->resume_ctx = &tls_main_ctx;
    tls_current = c;
    swapcontext(&tls_main_ctx, &c->ctx);
    /* returns here when the coroutine yields or finishes */
}

void coro_yield(void) {
    coro *cur = tls_current;
    if (!cur || cur->done)
        return;
    swapcontext(&cur->ctx, cur->resume_ctx);
}

int coro_done(const coro *c) {
    return c ? c->done : 1;
}

#pragma GCC diagnostic pop

#endif
