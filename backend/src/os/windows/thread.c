#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "os/os_thread.h"


#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

typedef struct {
    SRWLOCK lock;
} WinMutex;
typedef struct {
    CONDITION_VARIABLE cv;
} WinCond;
typedef struct {
    HANDLE h;
} WinThread;
/* Boot payload handed to the new thread: the thread copies fn/arg out and
 * frees it before calling fn, so thread_detach() freeing the WinThread (or
 * reusing its heap block) can never race the thread's own read of fn/arg
 * (use-after-free crash: call [rax+8] with garbage fn, 0xC0000005). */
typedef struct {
    thread_fn fn;
    void *arg;
} WinBoot;

int mutex_init(mutex_t *m) {
    WinMutex *w = (WinMutex *)m;
    InitializeSRWLock(&w->lock);
    return 0;
}
void mutex_destroy(mutex_t *m) {
    (void)m;
}
void mutex_lock(mutex_t *m) {
    AcquireSRWLockExclusive(&((WinMutex *)m)->lock);
}
void mutex_unlock(mutex_t *m) {
    ReleaseSRWLockExclusive(&((WinMutex *)m)->lock);
}

int cond_init(cond *c) {
    InitializeConditionVariable(&((WinCond *)c)->cv);
    return 0;
}
void cond_destroy(cond *c) {
    (void)c;
}
void cond_wait(cond *c, mutex_t *m) {
    SleepConditionVariableSRW(&((WinCond *)c)->cv, &((WinMutex *)m)->lock, INFINITE, 0);
}
int cond_timedwait_ms(cond *c, mutex_t *m, int ms) {
    BOOL ok =
        SleepConditionVariableSRW(&((WinCond *)c)->cv, &((WinMutex *)m)->lock, (DWORD)(ms < 0 ? INFINITE : ms), 0);
    return ok ? 0 : -1;
}
void cond_signal(cond *c) {
    WakeConditionVariable(&((WinCond *)c)->cv);
}
void cond_broadcast(cond *c) {
    WakeAllConditionVariable(&((WinCond *)c)->cv);
}

static unsigned __stdcall win_thread_proc(void *arg) {
    WinBoot *b = (WinBoot *)arg;
    thread_fn fn = b->fn;
    void *a = b->arg;

    free(b);
    fn(a);
    return 0;
}

thread_t *thread_create(thread_fn fn, void *arg) {
    thread_t *t = (thread_t *)malloc(sizeof(thread_t));
    WinThread *wt;
    WinBoot *b;

    if (!t)
        return NULL;
    wt = (WinThread *)t;
    b = (WinBoot *)malloc(sizeof(*b));
    if (!b) {
        free(t);
        return NULL;
    }

    b->fn = fn;
    b->arg = arg;
    wt->h = (HANDLE)_beginthreadex(NULL, 0, win_thread_proc, b, 0, NULL);
    if (!wt->h) {
        free(b);
        free(t);
        return NULL;
    }

    return t;
}
void thread_join(thread_t *t) {
    WinThread *wt = (WinThread *)t;
    if (wt->h) {
        WaitForSingleObject(wt->h, INFINITE);
        CloseHandle(wt->h);
        wt->h = NULL;
    }

    free(t);
}
void thread_detach(thread_t *t) {
    WinThread *wt = (WinThread *)t;
    if (wt->h) {
        CloseHandle(wt->h);
        wt->h = NULL;
    }

    free(t);
}

