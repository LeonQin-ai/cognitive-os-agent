#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cognitive-os-agent/os/os_thread.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

typedef struct { SRWLOCK lock; } WinMutex;
typedef struct { CONDITION_VARIABLE cv; } WinCond;
typedef struct { HANDLE h; coa_thread_fn fn; void *arg; } WinThread;

int coa_mutex_init(coa_mutex *m) {
    WinMutex *w = (WinMutex *)m;
    InitializeSRWLock(&w->lock);
    return 0;
}
void coa_mutex_destroy(coa_mutex *m) { (void)m; }
void coa_mutex_lock(coa_mutex *m) { AcquireSRWLockExclusive(&((WinMutex *)m)->lock); }
void coa_mutex_unlock(coa_mutex *m) { ReleaseSRWLockExclusive(&((WinMutex *)m)->lock); }

int coa_cond_init(coa_cond *c) {
    InitializeConditionVariable(&((WinCond *)c)->cv);
    return 0;
}
void coa_cond_destroy(coa_cond *c) { (void)c; }
void coa_cond_wait(coa_cond *c, coa_mutex *m) {
    SleepConditionVariableSRW(&((WinCond *)c)->cv, &((WinMutex *)m)->lock, INFINITE, 0);
}
int coa_cond_timedwait_ms(coa_cond *c, coa_mutex *m, int ms) {
    BOOL ok = SleepConditionVariableSRW(&((WinCond *)c)->cv, &((WinMutex *)m)->lock,
                                        (DWORD)(ms < 0 ? INFINITE : ms), 0);
    return ok ? 0 : -1;
}
void coa_cond_signal(coa_cond *c) { WakeConditionVariable(&((WinCond *)c)->cv); }
void coa_cond_broadcast(coa_cond *c) { WakeAllConditionVariable(&((WinCond *)c)->cv); }

static unsigned __stdcall win_thread_proc(void *arg) {
    WinThread *wt = (WinThread *)arg;
    wt->fn(wt->arg);
    return 0;
}

coa_thread *coa_thread_create(coa_thread_fn fn, void *arg) {
    coa_thread *t = (coa_thread *)malloc(sizeof(coa_thread));
    if (!t) return NULL;
    WinThread *wt = (WinThread *)t;
    wt->fn = fn;
    wt->arg = arg;
    wt->h = (HANDLE)_beginthreadex(NULL, 0, win_thread_proc, wt, 0, NULL);
    if (!wt->h) { free(t); return NULL; }
    return t;
}
void coa_thread_join(coa_thread *t) {
    WinThread *wt = (WinThread *)t;
    if (wt->h) { WaitForSingleObject(wt->h, INFINITE); CloseHandle(wt->h); wt->h = NULL; }
    free(t);
}
void coa_thread_detach(coa_thread *t) {
    WinThread *wt = (WinThread *)t;
    if (wt->h) { CloseHandle(wt->h); wt->h = NULL; }
    free(t);
}
uint64_t coa_thread_self_id(void) { return (uint64_t)GetCurrentThreadId(); }

#else /* POSIX */

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

typedef struct { pthread_mutex_t m; } PosixMutex;
typedef struct { pthread_cond_t c; } PosixCond;
typedef struct { pthread_t t; coa_thread_fn fn; void *arg; } PosixThread;
typedef struct { coa_thread_fn fn; void *arg; } PosixBoot;

int coa_mutex_init(coa_mutex *m) {
    return pthread_mutex_init(&((PosixMutex *)m)->m, NULL) == 0 ? 0 : -1;
}
void coa_mutex_destroy(coa_mutex *m) { pthread_mutex_destroy(&((PosixMutex *)m)->m); }
void coa_mutex_lock(coa_mutex *m) { pthread_mutex_lock(&((PosixMutex *)m)->m); }
void coa_mutex_unlock(coa_mutex *m) { pthread_mutex_unlock(&((PosixMutex *)m)->m); }

int coa_cond_init(coa_cond *c) {
    return pthread_cond_init(&((PosixCond *)c)->c, NULL) == 0 ? 0 : -1;
}
void coa_cond_destroy(coa_cond *c) { pthread_cond_destroy(&((PosixCond *)c)->c); }
void coa_cond_wait(coa_cond *c, coa_mutex *m) {
    pthread_cond_wait(&((PosixCond *)c)->c, &((PosixMutex *)m)->m);
}
int coa_cond_timedwait_ms(coa_cond *c, coa_mutex *m, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int r = pthread_cond_timedwait(&((PosixCond *)c)->c, &((PosixMutex *)m)->m, &ts);
    return r == 0 ? 0 : -1;
}
void coa_cond_signal(coa_cond *c) { pthread_cond_signal(&((PosixCond *)c)->c); }
void coa_cond_broadcast(coa_cond *c) { pthread_cond_broadcast(&((PosixCond *)c)->c); }

/* The bootstrap struct is read once by the new thread and freed by that
 * thread itself — coa_thread_detach may free the PosixThread handle at any
 * moment, so the handle must never be touched from inside the thread. */
static void *posix_thread_proc(void *arg) {
    PosixBoot *b = (PosixBoot *)arg;
    coa_thread_fn fn = b->fn;
    void *a = b->arg;
    free(b);
    fn(a);
    return NULL;
}

coa_thread *coa_thread_create(coa_thread_fn fn, void *arg) {
    coa_thread *t = (coa_thread *)malloc(sizeof(coa_thread));
    if (!t) return NULL;
    PosixThread *pt = (PosixThread *)t;
    PosixBoot *b = (PosixBoot *)malloc(sizeof(*b));
    if (!b) { free(t); return NULL; }
    b->fn = fn;
    b->arg = arg;
    if (pthread_create(&pt->t, NULL, posix_thread_proc, b) != 0) { free(b); free(t); return NULL; }
    return t;
}
void coa_thread_join(coa_thread *t) {
    PosixThread *pt = (PosixThread *)t;
    pthread_join(pt->t, NULL);
    free(t);
}
void coa_thread_detach(coa_thread *t) {
    PosixThread *pt = (PosixThread *)t;
    pthread_detach(pt->t);
    free(t);
}
uint64_t coa_thread_self_id(void) { return (uint64_t)(size_t)pthread_self(); }

#endif
