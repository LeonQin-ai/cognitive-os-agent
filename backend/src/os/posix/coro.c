/* os_coro.c — stackful coroutines: ucontext (POSIX) / Fiber (Windows). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* macOS headers hide ucontext behind _XOPEN_SOURCE; glibc tolerates it too. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "os/os_coro.h"

#include <stdlib.h>


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
    size_t sz = stack_size ? stack_size : CORO_STACK_DEFAULT;

    if (!c)
        return NULL;
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

