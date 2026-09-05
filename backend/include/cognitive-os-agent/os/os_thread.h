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
#define COA_OPAQUE64 _Alignas(16) unsigned char _d[64]

typedef struct coa_mutex   { COA_OPAQUE64; } coa_mutex;
typedef struct coa_cond    { COA_OPAQUE64; } coa_cond;
typedef struct coa_thread  { COA_OPAQUE64; } coa_thread;

/* ---------- mutex ---------- */
int  coa_mutex_init(coa_mutex *m);
void coa_mutex_destroy(coa_mutex *m);
void coa_mutex_lock(coa_mutex *m);
void coa_mutex_unlock(coa_mutex *m);

/* ---------- condition variable ---------- */
int  coa_cond_init(coa_cond *c);
void coa_cond_destroy(coa_cond *c);
/* Atomically unlock mtx and wait until signaled. Re-locks before returning. */
void coa_cond_wait(coa_cond *c, coa_mutex *m);
/* Like wait but with a timeout in milliseconds. Returns 0 on signal, -1 on timeout. */
int  coa_cond_timedwait_ms(coa_cond *c, coa_mutex *m, int ms);
void coa_cond_signal(coa_cond *c);
void coa_cond_broadcast(coa_cond *c);

/* ---------- thread ---------- */
typedef void (*coa_thread_fn)(void *arg);
/* Start a thread; returns NULL on failure. The thread runs fn(arg). */
coa_thread *coa_thread_create(coa_thread_fn fn, void *arg);
void coa_thread_join(coa_thread *t);
void coa_thread_detach(coa_thread *t);
/* OS thread id (for logging/debug). */
uint64_t coa_thread_self_id(void);

#ifdef __cplusplus
}
#endif

#undef COA_OPAQUE64
