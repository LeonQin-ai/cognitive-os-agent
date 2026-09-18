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
    thread_fn fn;
    void *arg;
} WinThread;

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
    WinThread *wt = (WinThread *)arg;
    wt->fn(wt->arg);
    return 0;
}

thread_t *thread_create(thread_fn fn, void *arg) {
    thread_t *t = (thread_t *)malloc(sizeof(thread_t));
    if (!t)
        return NULL;
    WinThread *wt = (WinThread *)t;
    wt->fn = fn;
    wt->arg = arg;
    wt->h = (HANDLE)_beginthreadex(NULL, 0, win_thread_proc, wt, 0, NULL);
    if (!wt->h) {
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

