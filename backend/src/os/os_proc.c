#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cagent/os/os_proc.h"
#include "cagent/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>

/* Convert a Windows directory to its MSYS POSIX form so it can be prepended
 * to $PATH inside bash: "C:\Program Files\Git\usr\bin" -> "/c/Program Files/Git/usr/bin". */
static void to_posix_dir(const char *win, char *out, size_t cap) {
    size_t n = strlen(win);
    if (n >= 2 && win[1] == ':') {
        size_t o = 0;
        if (cap > 3) {
            out[o++] = '/';
            out[o++] = (char)(win[0] | 0x20); /* drive letter -> lowercase */
            for (size_t i = 2; i < n && o + 1 < cap; i++)
                out[o++] = (win[i] == '\\') ? '/' : win[i];
            out[o] = '\0';
            return;
        }
    }
    snprintf(out, cap, "%s", win);
}

/* Choose the shell invocation format string (single %s = the command).
 * Honors CA_SHELL override (e.g. "C:\\...\\bash.exe -c"), then probes for a
 * POSIX shell so POSIX commands (mkdir -p, cp, ls) work on Windows, and
 * finally falls back to cmd.exe. */
static const char *shell_fmt(void) {
    static char buf[768];
    static int done = 0;
    if (done) return buf;
    done = 1;
    const char *override = getenv("CA_SHELL");
    if (override && *override) {
        snprintf(buf, sizeof(buf), "%s \"%%s\"", override);
        return buf;
    }
    /* Probe common POSIX shell installs: Git for Windows, MSYS2, per-user Git. */
    const char *rel[] = {
        "\\Git\\usr\\bin\\bash.exe",
        "\\Git\\Git\\usr\\bin\\bash.exe",
        "\\Git\\bin\\bash.exe",
        "\\msys64\\usr\\bin\\bash.exe",
    };
    char base[8][64];
    int nb = 0;
    const char *pf = getenv("ProgramFiles");
    const char *pf86 = getenv("ProgramFiles(x86)");
    const char *local = getenv("LOCALAPPDATA");
    if (pf)   snprintf(base[nb++], 64, "%s", pf);
    if (pf86) snprintf(base[nb++], 64, "%s", pf86);
    if (local) snprintf(base[nb++], 64, "%s\\Programs", local);
    for (char d = 'C'; d <= 'F' && nb < 8; d++)
        snprintf(base[nb++], 64, "%c:\\Program Files", d);
    for (char d = 'C'; d <= 'F' && nb < 8; d++)
        snprintf(base[nb++], 64, "%c:\\", d);

    for (int i = 0; i < nb; i++) {
        for (size_t j = 0; j < sizeof(rel) / sizeof(rel[0]); j++) {
            char path[300];
            snprintf(path, sizeof(path), "%s%s", base[i], rel[j]);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
                /* Non-login `bash -c` does not source /etc/profile, so $PATH
                 * lacks bash's own /usr/bin and even `ls` is "command not
                 * found". Prepend the bash directory (POSIX form) to PATH. */
                char dir[300];
                snprintf(dir, sizeof(dir), "%s", path);
                char *slash = strrchr(dir, '\\');
                if (slash) *slash = '\0';
                char posixdir[320];
                to_posix_dir(dir, posixdir, sizeof(posixdir));
                snprintf(buf, sizeof(buf),
                         "\"%s\" -c \"PATH=\\\"%s:$PATH\\\"; export PATH; %%s\"",
                         path, posixdir);
                return buf;
            }
        }
    }
    snprintf(buf, sizeof(buf), "cmd.exe /c \"%%s\"");
    return buf;
}

ca_proc_result *ca_proc_run_in(const char *cmd, int timeout_ms, const char *cwd) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return NULL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.dwFlags |= STARTF_USESTDHANDLES;

    char full[4096];
    snprintf(full, sizeof(full), shell_fmt(), cmd);

    /* lpCurrentDirectory must be a FULL path; a relative one makes
     * CreateProcess fail (or behave nondeterministically). Resolve first. */
    char abs_cwd[1024];
    const char *cwd_arg = NULL;
    if (cwd && *cwd) {
        if (GetFullPathNameA(cwd, sizeof(abs_cwd), abs_cwd, NULL) && abs_cwd[0])
            cwd_arg = abs_cwd;
        else
            cwd_arg = cwd;
    }

    BOOL ok = CreateProcessA(NULL, full, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL,
                             cwd_arg, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return NULL;
    }

    ca_proc_result *r = calloc(1, sizeof(ca_proc_result));
    if (!r) { CloseHandle(rd); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return NULL; }

    char *buf = malloc(65536);
    size_t cap = 65536, len = 0;
    if (!buf) { CloseHandle(rd); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); free(r); return NULL; }

    int64_t deadline = timeout_ms > 0 ? ca_time_now_ms() + timeout_ms : 0;
    int alive = 1;
    while (alive) {
        if (timeout_ms > 0 && ca_time_now_ms() >= deadline) break;
        DWORD avail = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD to_read = avail;
            if (len + to_read + 1 > cap) {
                cap = (len + to_read + 1) * 2;
                char *nb = realloc(buf, cap);
                if (!nb) break;
                buf = nb;
            }
            DWORD got = 0;
            if (!ReadFile(rd, buf + len, to_read, &got, NULL)) break;
            len += got;
        } else {
            /* No data available (avail==0), or the pipe is broken because the
             * child closed its stdout/stderr. Either way, keep polling until the
             * process actually exits so a fast child is not mistaken for a
             * timeout (GetExitCodeProcess can transiently report STILL_ACTIVE
             * right after the child closes the pipe). */
            DWORD code = 0;
            if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE) alive = 0;
            else ca_time_sleep_ms(5);
        }
    }

    DWORD exitc = 0;
    if (GetExitCodeProcess(pi.hProcess, &exitc)) {
        if (exitc == STILL_ACTIVE) {
            TerminateProcess(pi.hProcess, 1);
            r->timed_out = 1;
            exitc = (DWORD)-1;
        }
    } else {
        r->timed_out = 1;
        exitc = (DWORD)-1;
    }
    r->exit_code = (int)exitc;
    r->output = buf;
    buf[len] = '\0';
    /* trim trailing whitespace */
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) buf[--len] = '\0';

    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

void ca_proc_result_free(ca_proc_result *r) {
    if (!r) return;
    free(r->output);
    free(r);
}

ca_proc_result *ca_proc_run(const char *cmd, int timeout_ms) {
    return ca_proc_run_in(cmd, timeout_ms, NULL);
}

int ca_proc_spawn_detached(const char *cmd) {
    if (!cmd || !*cmd) return -1;
    char full[4096];
    snprintf(full, sizeof(full), shell_fmt(), cmd);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    BOOL ok = CreateProcessA(NULL, full, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                             NULL, NULL, &si, &pi);
    if (!ok) return -1;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

/* --- Persistent piped child process (for stdio MCP servers) --- */

struct ca_proc_popen {
    HANDLE proc;
    HANDLE in_wr;    /* write end of child stdin */
    HANDLE out_rd;   /* read end of child stdout */
    char *buf;
    size_t len, cap;
};

/* Quote a single argv element for a Windows command line. */
static void quote_arg(const char *a, char *out, size_t cap) {
    size_t o = 0;
    if (o < cap) out[o++] = '"';
    for (const char *p = a; *p && o + 2 < cap; p++) {
        if (*p == '"') { if (o + 2 < cap) { out[o++] = '\\'; out[o++] = '"'; } }
        else out[o++] = *p;
    }
    if (o < cap) out[o++] = '"';
    out[o] = '\0';
}

ca_proc_popen *ca_proc_popen_new(char *const argv[]) {
    if (!argv || !argv[0]) return NULL;

    /* Build "cmd.exe /s /c "<argv0> <argv1> ..."" so batch shims like npx.cmd
     * also work. /s makes cmd strip ONLY the outer quotes, preserving the
     * per-argument quotes (plain /c mangles multi-quoted command lines). */
    char cmdline[4096];
    size_t off = (size_t)snprintf(cmdline, sizeof(cmdline), "cmd.exe /s /c \"");
    for (int i = 0; argv[i] && off < sizeof(cmdline); i++) {
        char q[800];
        quote_arg(argv[i], q, sizeof(q));
        int wr = snprintf(cmdline + off, sizeof(cmdline) - off, " %s", q);
        if (wr < 0 || (size_t)wr >= sizeof(cmdline) - off) break;
        off += (size_t)wr;
    }
    if (off < sizeof(cmdline) - 1) { cmdline[off++] = '"'; cmdline[off] = '\0'; }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE in_rd = NULL, in_wr = NULL, out_rd = NULL, out_wr = NULL;
    if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) return NULL;
    if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) {
        CloseHandle(in_rd); CloseHandle(in_wr);
        return NULL;
    }
    SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);

    /* child stderr -> NUL so server logs never pollute the JSON stream */
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, NULL);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_rd;
    si.hStdOutput = out_wr;
    si.hStdError = nul ? nul : out_wr;

    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(in_rd);
    CloseHandle(out_wr);
    if (nul) CloseHandle(nul);
    if (!ok) {
        CloseHandle(in_wr);
        CloseHandle(out_rd);
        return NULL;
    }
    CloseHandle(pi.hThread);

    ca_proc_popen *p = calloc(1, sizeof(*p));
    if (!p) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(in_wr);
        CloseHandle(out_rd);
        return NULL;
    }
    p->proc = pi.hProcess;
    p->in_wr = in_wr;
    p->out_rd = out_rd;
    p->cap = 65536;
    p->buf = malloc(p->cap);
    if (!p->buf) {
        CloseHandle(pi.hProcess);
        CloseHandle(in_wr);
        CloseHandle(out_rd);
        free(p);
        return NULL;
    }
    p->buf[0] = '\0';
    return p;
}

int ca_proc_popen_write(ca_proc_popen *p, const char *data, size_t len) {
    if (!p || !data) return -1;
    DWORD written = 0;
    if (!WriteFile(p->in_wr, data, (DWORD)len, &written, NULL) || written != len)
        return -1;
    return 0;
}

size_t ca_proc_popen_read(ca_proc_popen *p, int timeout_ms) {
    if (!p) return 0;
    int64_t deadline = timeout_ms > 0 ? ca_time_now_ms() + timeout_ms : 0;
    size_t start_len = p->len;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(p->out_rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            if (p->len + avail + 1 > p->cap) {
                size_t ncap = (p->len + avail + 1) * 2;
                char *nb = realloc(p->buf, ncap);
                if (!nb) break;
                p->buf = nb;
                p->cap = ncap;
            }
            DWORD got = 0;
            if (!ReadFile(p->out_rd, p->buf + p->len, avail, &got, NULL) || got == 0) break;
            p->len += got;
            p->buf[p->len] = '\0';
            return p->len - start_len; /* return after one read burst */
        }
        /* no data: stop when dead or deadline passed */
        DWORD code = 0;
        if (GetExitCodeProcess(p->proc, &code) && code != STILL_ACTIVE) break;
        if (timeout_ms > 0 && ca_time_now_ms() >= deadline) break;
        ca_time_sleep_ms(10);
    }
    return p->len - start_len;
}

const char *ca_proc_popen_buffer(ca_proc_popen *p) {
    return (p && p->buf) ? p->buf : "";
}

void ca_proc_popen_reset(ca_proc_popen *p) {
    if (p) { p->len = 0; if (p->buf) p->buf[0] = '\0'; }
}

/* Discard the first `n` bytes of the read buffer, keeping the rest. */
void ca_proc_popen_trim(ca_proc_popen *p, size_t n) {
    if (!p || n == 0) return;
    if (n >= p->len) { ca_proc_popen_reset(p); return; }
    memmove(p->buf, p->buf + n, p->len - n);
    p->len -= n;
    p->buf[p->len] = '\0';
}

int ca_proc_popen_alive(ca_proc_popen *p) {
    if (!p) return 0;
    DWORD code = 0;
    if (GetExitCodeProcess(p->proc, &code) && code != STILL_ACTIVE) return 0;
    return 1;
}

void ca_proc_popen_free(ca_proc_popen *p) {
    if (!p) return;
    if (p->proc != INVALID_HANDLE_VALUE) {
        TerminateProcess(p->proc, 1);
        CloseHandle(p->proc);
    }
    if (p->in_wr) CloseHandle(p->in_wr);
    if (p->out_rd) CloseHandle(p->out_rd);
    free(p->buf);
    free(p);
}

#else /* POSIX */

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

ca_proc_result *ca_proc_run_in(const char *cmd, int timeout_ms, const char *cwd) {
    int pfd[2];
    if (pipe(pfd) != 0) return NULL;
    /* make read end non-blocking for the read loop */
    int flags = fcntl(pfd[0], F_GETFL, 0);
    fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return NULL;
    }
    if (pid == 0) {
        /* child */
        if (cwd && *cwd) {
            if (chdir(cwd) != 0) _exit(126);
        }
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);

    ca_proc_result *r = calloc(1, sizeof(ca_proc_result));
    char *buf = malloc(65536);
    size_t cap = 65536, len = 0;
    if (!r || !buf) {
        if (r) free(r);
        if (buf) free(buf);
        close(pfd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return NULL;
    }

    int64_t deadline = timeout_ms > 0 ? ca_time_now_ms() + timeout_ms : 0;
    for (;;) {
        if (timeout_ms > 0 && ca_time_now_ms() >= deadline) break;
        ssize_t got = read(pfd[0], buf + len, cap - len - 1);
        if (got > 0) {
            len += (size_t)got;
            if (len + 1024 > cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) break;
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
        if (got < 0 && errno != EAGAIN && errno != EINTR) break;
        ca_time_sleep_ms(5);
    }

    /* timeout handling */
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        r->timed_out = 1;
        r->exit_code = -1;
    }
    /* drain remaining */
    for (;;) {
        ssize_t got = read(pfd[0], buf + len, cap - len - 1);
        if (got <= 0) break;
        len += (size_t)got;
    }
    close(pfd[0]);
    buf[len] = '\0';
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) buf[--len] = '\0';
    r->output = buf;
    return r;
}

void ca_proc_result_free(ca_proc_result *r) {
    if (!r) return;
    free(r->output);
    free(r);
}

ca_proc_result *ca_proc_run(const char *cmd, int timeout_ms) {
    return ca_proc_run_in(cmd, timeout_ms, NULL);
}

int ca_proc_spawn_detached(const char *cmd) {
    if (!cmd || !*cmd) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
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

struct ca_proc_popen {
    pid_t pid;
    int in_wr;    /* write end of child stdin */
    int out_rd;   /* read end of child stdout */
    char *buf;
    size_t len, cap;
    int dead;
};

ca_proc_popen *ca_proc_popen_new(char *const argv[]) {
    if (!argv || !argv[0]) return NULL;
    int in_p[2], out_p[2];
    if (pipe(in_p) != 0) return NULL;
    if (pipe(out_p) != 0) {
        close(in_p[0]); close(in_p[1]);
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(in_p[0]); close(in_p[1]);
        close(out_p[0]); close(out_p[1]);
        return NULL;
    }
    if (pid == 0) {
        dup2(in_p[0], STDIN_FILENO);
        dup2(out_p[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(in_p[0]); close(in_p[1]);
        close(out_p[0]); close(out_p[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in_p[0]);
    close(out_p[1]);
    /* non-blocking read end for polling */
    int fl = fcntl(out_p[0], F_GETFL, 0);
    fcntl(out_p[0], F_SETFL, fl | O_NONBLOCK);

    ca_proc_popen *p = calloc(1, sizeof(*p));
    if (!p) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(in_p[1]); close(out_p[0]);
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
        close(in_p[1]); close(out_p[0]);
        free(p);
        return NULL;
    }
    p->buf[0] = '\0';
    return p;
}

int ca_proc_popen_write(ca_proc_popen *p, const char *data, size_t len) {
    if (!p || !data) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(p->in_wr, data + off, len - off);
        if (w <= 0) {
            if (w < 0 && (errno == EINTR)) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

size_t ca_proc_popen_read(ca_proc_popen *p, int timeout_ms) {
    if (!p) return 0;
    int64_t deadline = timeout_ms > 0 ? ca_time_now_ms() + timeout_ms : 0;
    size_t start_len = p->len;
    for (;;) {
        struct pollfd pf = { p->out_rd, POLLIN, 0 };
        int timeout = timeout_ms > 0 ? (int)(deadline - ca_time_now_ms()) : 100;
        if (timeout < 0) timeout = 0;
        int pr = poll(&pf, 1, timeout);
        if (pr > 0 && (pf.revents & (POLLIN | POLLHUP))) {
            if (p->len + 4096 + 1 > p->cap) {
                size_t ncap = (p->len + 4096 + 1) * 2;
                char *nb = realloc(p->buf, ncap);
                if (!nb) break;
                p->buf = nb;
                p->cap = ncap;
            }
            ssize_t got = read(p->out_rd, p->buf + p->len, p->cap - p->len - 1);
            if (got > 0) {
                p->len += (size_t)got;
                p->buf[p->len] = '\0';
                return p->len - start_len; /* one burst per call */
            }
            if (got == 0) { p->dead = 1; break; } /* EOF: child closed stdout */
            if (errno == EAGAIN || errno == EINTR) { /* spurious; keep polling */ }
            else { p->dead = 1; break; }
        }
        if (p->dead) break;
        if (timeout_ms > 0 && ca_time_now_ms() >= deadline) break;
        if (pr == 0 && timeout_ms <= 0) break; /* poll timeout in no-deadline mode */
    }
    return p->len - start_len;
}

const char *ca_proc_popen_buffer(ca_proc_popen *p) {
    return (p && p->buf) ? p->buf : "";
}

void ca_proc_popen_reset(ca_proc_popen *p) {
    if (p) { p->len = 0; if (p->buf) p->buf[0] = '\0'; }
}

/* Discard the first `n` bytes of the read buffer, keeping the rest. */
void ca_proc_popen_trim(ca_proc_popen *p, size_t n) {
    if (!p || n == 0) return;
    if (n >= p->len) { ca_proc_popen_reset(p); return; }
    memmove(p->buf, p->buf + n, p->len - n);
    p->len -= n;
    p->buf[p->len] = '\0';
}

int ca_proc_popen_alive(ca_proc_popen *p) {
    if (!p) return 0;
    if (p->dead) return 0;
    int status = 0;
    pid_t wr = waitpid(p->pid, &status, WNOHANG);
    if (wr == p->pid) { p->dead = 1; return 0; }
    return 1;
}

void ca_proc_popen_free(ca_proc_popen *p) {
    if (!p) return;
    kill(p->pid, SIGKILL);
    waitpid(p->pid, NULL, 0);
    close(p->in_wr);
    close(p->out_rd);
    free(p->buf);
    free(p);
}

#endif
