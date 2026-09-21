/* os_coro.c — stackful coroutines: ucontext (POSIX) / Fiber (Windows). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "os/os_coro.h"

#include <stdlib.h>


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
        tls_main_fiber = ConvertThreadToFiber(NULL);
        if (!tls_main_fiber && GetLastError() == ERROR_ALREADY_FIBER)
            tls_main_fiber = GetCurrentFiber();
        if (!tls_main_fiber)
            return;
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
