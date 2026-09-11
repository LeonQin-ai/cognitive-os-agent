/* os_thread.h — cross-platform threads, mutexes, condition variables.
 * Windows (SRWLOCK/CONDITION_VARIABLE/threads) and POSIX (pthread) backends.
 * Objects are opaque fixed-size structs so they can live on the stack. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 64-byte opaque storage (pthread_cond_t is 48B on glibc/x86_64). */
#define OPAQUE64 _Alignas(16) unsigned char _d[64]

typedef struct mutex_t {
    OPAQUE64;
} mutex_t;
typedef struct cond {
    OPAQUE64;
} cond;
typedef struct thread_t {
    OPAQUE64;
} thread_t;

/* ---------- mutex ---------- */
int mutex_init(mutex_t *m);
void mutex_destroy(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/* ---------- condition variable ---------- */
int cond_init(cond *c);
void cond_destroy(cond *c);
/* Atomically unlock mtx and wait until signaled. Re-locks before returning. */
void cond_wait(cond *c, mutex_t *m);
/* Like wait but with a timeout in milliseconds. Returns 0 on signal, -1 on timeout. */
int cond_timedwait_ms(cond *c, mutex_t *m, int ms);
void cond_signal(cond *c);
void cond_broadcast(cond *c);

/* ---------- thread ---------- */
typedef void (*thread_fn)(void *arg);
/* Start a thread; returns NULL on failure. The thread runs fn(arg). */
thread_t *thread_create(thread_fn fn, void *arg);
void thread_join(thread_t *t);
void thread_detach(thread_t *t);

#ifdef __cplusplus
}
#endif

#undef OPAQUE64
