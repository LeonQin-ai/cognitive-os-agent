#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "os/os_thread.h"


#include <pthread.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    pthread_mutex_t m;
} PosixMutex;
typedef struct {
    pthread_cond_t c;
} PosixCond;
typedef struct {
    pthread_t t;
    thread_fn fn;
    void *arg;
} PosixThread;
typedef struct {
    thread_fn fn;
    void *arg;
} PosixBoot;

int mutex_init(mutex_t *m) {
    return pthread_mutex_init(&((PosixMutex *)m)->m, NULL) == 0 ? 0 : -1;
}
void mutex_destroy(mutex_t *m) {
    pthread_mutex_destroy(&((PosixMutex *)m)->m);
}
void mutex_lock(mutex_t *m) {
    pthread_mutex_lock(&((PosixMutex *)m)->m);
}
void mutex_unlock(mutex_t *m) {
    pthread_mutex_unlock(&((PosixMutex *)m)->m);
}

int cond_init(cond *c) {
    return pthread_cond_init(&((PosixCond *)c)->c, NULL) == 0 ? 0 : -1;
}
void cond_destroy(cond *c) {
    pthread_cond_destroy(&((PosixCond *)c)->c);
}
void cond_wait(cond *c, mutex_t *m) {
    pthread_cond_wait(&((PosixCond *)c)->c, &((PosixMutex *)m)->m);
}
int cond_timedwait_ms(cond *c, mutex_t *m, int ms) {
    struct timespec ts;
    int r;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    r = pthread_cond_timedwait(&((PosixCond *)c)->c, &((PosixMutex *)m)->m, &ts);
    return r == 0 ? 0 : -1;
}
void cond_signal(cond *c) {
    pthread_cond_signal(&((PosixCond *)c)->c);
}
void cond_broadcast(cond *c) {
    pthread_cond_broadcast(&((PosixCond *)c)->c);
}

/* The bootstrap struct is read once by the new thread and freed by that
 * thread itself — thread_detach may free the PosixThread handle at any
 * moment, so the handle must never be touched from inside the thread. */
static void *posix_thread_proc(void *arg) {
    void *a;

    PosixBoot *b = (PosixBoot *)arg;
    thread_fn fn = b->fn;
    a = b->arg;
    free(b);
    fn(a);
    return NULL;
}

thread_t *thread_create(thread_fn fn, void *arg) {
    thread_t *t = (thread_t *)malloc(sizeof(thread_t));
    if (!t)
        return NULL;
    PosixThread *pt = (PosixThread *)t;
    PosixBoot *b = (PosixBoot *)malloc(sizeof(*b));
    if (!b) {
        free(t);
        return NULL;
    }

    b->fn = fn;
    b->arg = arg;
    if (pthread_create(&pt->t, NULL, posix_thread_proc, b) != 0) {
        free(b);
        free(t);
        return NULL;
    }

    return t;
}
void thread_join(thread_t *t) {
    PosixThread *pt = (PosixThread *)t;
    pthread_join(pt->t, NULL);
    free(t);
}
void thread_detach(thread_t *t) {
    PosixThread *pt = (PosixThread *)t;
    pthread_detach(pt->t);
    free(t);
}

