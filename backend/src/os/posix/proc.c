#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "os/os_proc.h"
#include "os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>


#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

proc_result *proc_run_in(const char *cmd, int timeout_ms, const char *cwd) {
    int pfd[2];
    /* make read end non-blocking for the read loop */
    int flags;
    pid_t pid;
    proc_result *r;
    char *buf;
    int64_t deadline;
    /* timeout handling */
    int status = 0;

    if (pipe(pfd) != 0)
        return NULL;
    /* make read end non-blocking for the read loop */
    flags = fcntl(pfd[0], F_GETFL, 0);
    fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return NULL;
    }

    if (pid == 0) {
        /* child */
        if (cwd && *cwd) {
            if (chdir(cwd) != 0)
                _exit(126);
        }
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);

    r = calloc(1, sizeof(proc_result));
    buf = malloc(65536);
    size_t cap = 65536, len = 0;
    if (!r || !buf) {
        if (r)
            free(r);
        if (buf)
            free(buf);
        close(pfd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return NULL;
    }

    deadline = timeout_ms > 0 ? time_now_ms() + timeout_ms : 0;
    for (;;) {
        if (timeout_ms > 0 && time_now_ms() >= deadline)
            break;
        ssize_t got = read(pfd[0], buf + len, cap - len - 1);
        if (got > 0) {
            len += (size_t)got;
            if (len + 1024 > cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb)
                    break;
                buf = nb;
            }
            continue;
        }
        /* check if child exited */
        int status = 0;
        pid_t wr = waitpid(pid, &status, WNOHANG);
        if (wr == pid) {
            r->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        if (got < 0 && errno != EAGAIN && errno != EINTR)
            break;
        time_sleep_ms(5);
    }

    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        r->timed_out = 1;
        r->exit_code = -1;
    }

    /* drain remaining */
    for (;;) {
        ssize_t got = read(pfd[0], buf + len, cap - len - 1);
        if (got <= 0)
            break;
        len += (size_t)got;
    }

    close(pfd[0]);
    buf[len] = '\0';
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
        buf[--len] = '\0';
    r->output = buf;
    return r;
}

void proc_result_free(proc_result *r) {
    if (!r)
        return;
    free(r->output);
    free(r);
}

proc_result *proc_run(const char *cmd, int timeout_ms) {
    return proc_run_in(cmd, timeout_ms, NULL);
}

proc_result *proc_run_native_in(const char *cmd, int timeout_ms, const char *cwd) {
    return proc_run_in(cmd, timeout_ms, cwd);
}

int proc_spawn_detached(const char *cmd) {
    pid_t pid;

    if (!cmd || !*cmd)
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* child: new session, no controlling tty, stdio to /dev/null */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    return 0;
}

/* --- Persistent piped child process (for stdio MCP servers) --- */

#include <poll.h>

struct proc_popen {
    pid_t pid;
    int in_wr;  /* write end of child stdin */
    int out_rd; /* read end of child stdout */
    char *buf;
    size_t len, cap;
    int dead;
};

proc_popen *proc_popen_new(char *const argv[]) {
    return proc_popen_new_ex(argv, 0);
}

proc_popen *proc_popen_new_ex(char *const argv[], int merge_stderr) {
    pid_t pid;
    /* non-blocking read end for polling */
    int fl;
    proc_popen *p;

    if (!argv || !argv[0])
        return NULL;
    int in_p[2], out_p[2];
    if (pipe(in_p) != 0)
        return NULL;
    if (pipe(out_p) != 0) {
        close(in_p[0]);
        close(in_p[1]);
        return NULL;
    }

    pid = fork();
    if (pid < 0) {
        close(in_p[0]);
        close(in_p[1]);
        close(out_p[0]);
        close(out_p[1]);
        return NULL;
    }

    if (pid == 0) {
        dup2(in_p[0], STDIN_FILENO);
        dup2(out_p[1], STDOUT_FILENO);
        if (merge_stderr) {
            dup2(out_p[1], STDERR_FILENO);
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0)
                dup2(devnull, STDERR_FILENO);
        }
        close(in_p[0]);
        close(in_p[1]);
        close(out_p[0]);
        close(out_p[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(in_p[0]);
    close(out_p[1]);
    /* non-blocking read end for polling */
    fl = fcntl(out_p[0], F_GETFL, 0);
    fcntl(out_p[0], F_SETFL, fl | O_NONBLOCK);

    p = calloc(1, sizeof(*p));
    if (!p) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(in_p[1]);
        close(out_p[0]);
        return NULL;
    }

    p->pid = pid;
    p->in_wr = in_p[1];
    p->out_rd = out_p[0];
    p->cap = 65536;
    p->buf = malloc(p->cap);
    if (!p->buf) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(in_p[1]);
        close(out_p[0]);
        free(p);
        return NULL;
    }

    p->buf[0] = '\0';
    return p;
}

int proc_popen_write(proc_popen *p, const char *data, size_t len) {
    size_t off = 0;
    sigset_t blocked, previous, pending;
    int had_sigpipe, saved_errno = 0, rc = 0;

    if (!p || !data)
        return -1;
    /* A dead stdio MCP child must not terminate the host. Block SIGPIPE
     * only in this writer, preserving the caller's disposition and mask. */
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    int mask_rc = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (mask_rc != 0) {
        errno = mask_rc;
        return -1;
    }
    if (sigpending(&pending) != 0) {
        saved_errno = errno;
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
        errno = saved_errno;
        return -1;
    }
    had_sigpipe = sigismember(&pending, SIGPIPE);
    while (off < len) {
        ssize_t w = write(p->in_wr, data + off, len - off);
        if (w <= 0) {
            if (w < 0 && (errno == EINTR))
                continue;
            saved_errno = w < 0 ? errno : EIO;
            rc = -1;
            break;
        }
        off += (size_t)w;
    }

    /* Consume only a newly pending signal from our broken-pipe write.
     * Ignored SIGPIPE need not become pending; never wait in that case. */
    if (saved_errno == EPIPE && !had_sigpipe &&
        sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
        int signo;
        sigwait(&blocked, &signo);
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (rc != 0)
        errno = saved_errno;
    return rc;
}

size_t proc_popen_read(proc_popen *p, int timeout_ms) {
    int64_t deadline;
    size_t start_len;

    if (!p)
        return 0;
    deadline = timeout_ms > 0 ? time_now_ms() + timeout_ms : 0;
    start_len = p->len;
    for (;;) {
        struct pollfd pf = {p->out_rd, POLLIN, 0};
        int timeout = timeout_ms > 0 ? (int)(deadline - time_now_ms()) : 100;
        if (timeout < 0)
            timeout = 0;
        int pr = poll(&pf, 1, timeout);
        if (pr > 0 && (pf.revents & (POLLIN | POLLHUP))) {
            if (p->len + 4096 + 1 > p->cap) {
                size_t ncap = (p->len + 4096 + 1) * 2;
                char *nb = realloc(p->buf, ncap);
                if (!nb)
                    break;
                p->buf = nb;
                p->cap = ncap;
            }
            ssize_t got = read(p->out_rd, p->buf + p->len, p->cap - p->len - 1);
            if (got > 0) {
                p->len += (size_t)got;
                p->buf[p->len] = '\0';
                return p->len - start_len; /* one burst per call */
            }
            if (got == 0) {
                p->dead = 1;
                break;
            }                                        /* EOF: child closed stdout */
            if (errno == EAGAIN || errno == EINTR) { /* spurious; keep polling */
            } else {
                p->dead = 1;
                break;
            }
        }
        if (p->dead)
            break;
        if (timeout_ms > 0 && time_now_ms() >= deadline)
            break;
        if (pr == 0 && timeout_ms <= 0)
            break; /* poll timeout in no-deadline mode */
    }

    return p->len - start_len;
}

const char *proc_popen_buffer(proc_popen *p) {
    return (p && p->buf) ? p->buf : "";
}

void proc_popen_reset(proc_popen *p) {
    if (p) {
        p->len = 0;
        if (p->buf)
            p->buf[0] = '\0';
    }
}

/* Discard the first `n` bytes of the read buffer, keeping the rest. */
void proc_popen_trim(proc_popen *p, size_t n) {
    if (!p || n == 0)
        return;
    if (n >= p->len) {
        proc_popen_reset(p);
        return;
    }

    memmove(p->buf, p->buf + n, p->len - n);
    p->len -= n;
    p->buf[p->len] = '\0';
}

int proc_popen_alive(proc_popen *p) {
    int status = 0;
    pid_t wr;

    if (!p)
        return 0;
    if (p->dead)
        return 0;
    wr = waitpid(p->pid, &status, WNOHANG);
    if (wr == p->pid) {
        p->dead = 1;
        return 0;
    }

    return 1;
}

void proc_popen_free(proc_popen *p) {
    if (!p)
        return;
    kill(p->pid, SIGKILL);
    waitpid(p->pid, NULL, 0);
    close(p->in_wr);
    close(p->out_rd);
    free(p->buf);
    free(p);
}
