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
#define CA_OPAQUE64 _Alignas(16) unsigned char _d[64]

typedef struct ca_mutex   { CA_OPAQUE64; } ca_mutex;
typedef struct ca_cond    { CA_OPAQUE64; } ca_cond;
typedef struct ca_thread  { CA_OPAQUE64; } ca_thread;

/* ---------- mutex ---------- */
int  ca_mutex_init(ca_mutex *m);
void ca_mutex_destroy(ca_mutex *m);
void ca_mutex_lock(ca_mutex *m);
void ca_mutex_unlock(ca_mutex *m);

/* ---------- condition variable ---------- */
int  ca_cond_init(ca_cond *c);
void ca_cond_destroy(ca_cond *c);
/* Atomically unlock mtx and wait until signaled. Re-locks before returning. */
void ca_cond_wait(ca_cond *c, ca_mutex *m);
/* Like wait but with a timeout in milliseconds. Returns 0 on signal, -1 on timeout. */
int  ca_cond_timedwait_ms(ca_cond *c, ca_mutex *m, int ms);
void ca_cond_signal(ca_cond *c);
void ca_cond_broadcast(ca_cond *c);

/* ---------- thread ---------- */
typedef void (*ca_thread_fn)(void *arg);
/* Start a thread; returns NULL on failure. The thread runs fn(arg). */
ca_thread *ca_thread_create(ca_thread_fn fn, void *arg);
void ca_thread_join(ca_thread *t);
void ca_thread_detach(ca_thread *t);
/* OS thread id (for logging/debug). */
uint64_t ca_thread_self_id(void);

#ifdef __cplusplus
}
#endif

#undef CA_OPAQUE64
