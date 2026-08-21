#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cagent/os/os_thread.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

typedef struct { SRWLOCK lock; } WinMutex;
typedef struct { CONDITION_VARIABLE cv; } WinCond;
typedef struct { HANDLE h; ca_thread_fn fn; void *arg; } WinThread;

int ca_mutex_init(ca_mutex *m) {
    WinMutex *w = (WinMutex *)m;
    InitializeSRWLock(&w->lock);
    return 0;
}
void ca_mutex_destroy(ca_mutex *m) { (void)m; }
void ca_mutex_lock(ca_mutex *m) { AcquireSRWLockExclusive(&((WinMutex *)m)->lock); }
void ca_mutex_unlock(ca_mutex *m) { ReleaseSRWLockExclusive(&((WinMutex *)m)->lock); }

int ca_cond_init(ca_cond *c) {
    InitializeConditionVariable(&((WinCond *)c)->cv);
    return 0;
}
void ca_cond_destroy(ca_cond *c) { (void)c; }
void ca_cond_wait(ca_cond *c, ca_mutex *m) {
    SleepConditionVariableSRW(&((WinCond *)c)->cv, &((WinMutex *)m)->lock, INFINITE, 0);
}
int ca_cond_timedwait_ms(ca_cond *c, ca_mutex *m, int ms) {
    BOOL ok = SleepConditionVariableSRW(&((WinCond *)c)->cv, &((WinMutex *)m)->lock,
                                        (DWORD)(ms < 0 ? INFINITE : ms), 0);
    return ok ? 0 : -1;
}
void ca_cond_signal(ca_cond *c) { WakeConditionVariable(&((WinCond *)c)->cv); }
void ca_cond_broadcast(ca_cond *c) { WakeAllConditionVariable(&((WinCond *)c)->cv); }

static unsigned __stdcall win_thread_proc(void *arg) {
    WinThread *wt = (WinThread *)arg;
    wt->fn(wt->arg);
    return 0;
}

ca_thread *ca_thread_create(ca_thread_fn fn, void *arg) {
    ca_thread *t = (ca_thread *)malloc(sizeof(ca_thread));
    if (!t) return NULL;
    WinThread *wt = (WinThread *)t;
    wt->fn = fn;
    wt->arg = arg;
    wt->h = (HANDLE)_beginthreadex(NULL, 0, win_thread_proc, wt, 0, NULL);
    if (!wt->h) { free(t); return NULL; }
    return t;
}
void ca_thread_join(ca_thread *t) {
    WinThread *wt = (WinThread *)t;
    if (wt->h) { WaitForSingleObject(wt->h, INFINITE); CloseHandle(wt->h); wt->h = NULL; }
    free(t);
}
void ca_thread_detach(ca_thread *t) {
    WinThread *wt = (WinThread *)t;
    if (wt->h) { CloseHandle(wt->h); wt->h = NULL; }
    free(t);
}
uint64_t ca_thread_self_id(void) { return (uint64_t)GetCurrentThreadId(); }

#else /* POSIX */

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

typedef struct { pthread_mutex_t m; } PosixMutex;
typedef struct { pthread_cond_t c; } PosixCond;
typedef struct { pthread_t t; ca_thread_fn fn; void *arg; } PosixThread;

int ca_mutex_init(ca_mutex *m) {
    return pthread_mutex_init(&((PosixMutex *)m)->m, NULL) == 0 ? 0 : -1;
}
void ca_mutex_destroy(ca_mutex *m) { pthread_mutex_destroy(&((PosixMutex *)m)->m); }
void ca_mutex_lock(ca_mutex *m) { pthread_mutex_lock(&((PosixMutex *)m)->m); }
void ca_mutex_unlock(ca_mutex *m) { pthread_mutex_unlock(&((PosixMutex *)m)->m); }

int ca_cond_init(ca_cond *c) {
    return pthread_cond_init(&((PosixCond *)c)->c, NULL) == 0 ? 0 : -1;
}
void ca_cond_destroy(ca_cond *c) { pthread_cond_destroy(&((PosixCond *)c)->c); }
void ca_cond_wait(ca_cond *c, ca_mutex *m) {
    pthread_cond_wait(&((PosixCond *)c)->c, &((PosixMutex *)m)->m);
}
int ca_cond_timedwait_ms(ca_cond *c, ca_mutex *m, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int r = pthread_cond_timedwait(&((PosixCond *)c)->c, &((PosixMutex *)m)->m, &ts);
    return r == 0 ? 0 : -1;
}
void ca_cond_signal(ca_cond *c) { pthread_cond_signal(&((PosixCond *)c)->c); }
void ca_cond_broadcast(ca_cond *c) { pthread_cond_broadcast(&((PosixCond *)c)->c); }

static void *posix_thread_proc(void *arg) {
    PosixThread *pt = (PosixThread *)arg;
    pt->fn(pt->arg);
    return NULL;
}

ca_thread *ca_thread_create(ca_thread_fn fn, void *arg) {
    ca_thread *t = (ca_thread *)malloc(sizeof(ca_thread));
    if (!t) return NULL;
    PosixThread *pt = (PosixThread *)t;
    pt->fn = fn;
    pt->arg = arg;
    if (pthread_create(&pt->t, NULL, posix_thread_proc, pt) != 0) { free(t); return NULL; }
    return t;
}
void ca_thread_join(ca_thread *t) {
    PosixThread *pt = (PosixThread *)t;
    pthread_join(pt->t, NULL);
    free(t);
}
void ca_thread_detach(ca_thread *t) {
    PosixThread *pt = (PosixThread *)t;
    pthread_detach(pt->t);
    free(t);
}
uint64_t ca_thread_self_id(void) { return (uint64_t)(size_t)pthread_self(); }

#endif
