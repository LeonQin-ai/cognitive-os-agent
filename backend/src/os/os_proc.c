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
                snprintf(buf, sizeof(buf), "\"%s\" -c \"%%s\"", path);
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

    BOOL ok = CreateProcessA(NULL, full, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL,
                             (cwd && *cwd) ? cwd : NULL, &si, &pi);
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

#endif
