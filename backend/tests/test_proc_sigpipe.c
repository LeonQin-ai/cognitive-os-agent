#define _POSIX_C_SOURCE 200809L
#include "os/os_proc.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void) {
    char *args[] = {"/bin/sh", "-c", "exec 0<&-; printf ready", NULL};
    sigset_t pipe_set, original, before, after, pending;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    CHECK(pthread_sigmask(SIG_UNBLOCK, &pipe_set, &original) == 0);
    for (int mode = 0; mode < 3; mode++) {
        CHECK(signal(SIGPIPE, mode == 1 ? SIG_IGN : SIG_DFL) != SIG_ERR);
        if (mode == 2) {
            CHECK(pthread_sigmask(SIG_BLOCK, &pipe_set, NULL) == 0);
            CHECK(raise(SIGPIPE) == 0);
        }
        CHECK(pthread_sigmask(SIG_BLOCK, NULL, &before) == 0);
        proc_popen *p = proc_popen_new(args);
        CHECK(p != NULL);
        for (int i = 0; i < 20 && !strstr(proc_popen_buffer(p), "ready"); i++)
            proc_popen_read(p, 100);
        CHECK(strstr(proc_popen_buffer(p), "ready") != NULL);
        errno = 0;
        CHECK(proc_popen_write(p, "request", 7) == -1);
        CHECK(errno == EPIPE);
        CHECK(pthread_sigmask(SIG_BLOCK, NULL, &after) == 0);
        CHECK(sigismember(&before, SIGPIPE) == sigismember(&after, SIGPIPE));
        CHECK(sigpending(&pending) == 0);
        CHECK(sigismember(&pending, SIGPIPE) == (mode == 2));
        proc_popen_free(p);
        if (mode == 2) {
            int signo;
            CHECK(sigwait(&pipe_set, &signo) == 0 && signo == SIGPIPE);
        }
    }
    CHECK(pthread_sigmask(SIG_SETMASK, &original, NULL) == 0);
    puts("PASS: closed child stdin, ignored SIGPIPE, pre-existing pending SIGPIPE");
    return 0;
}
