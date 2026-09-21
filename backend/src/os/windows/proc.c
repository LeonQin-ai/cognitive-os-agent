#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "os/os_proc.h"
#include "os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>


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

/* Which quoting style the chosen backend needs for the embedded command. */
typedef enum {
    SH_QUOTED = 0, /* command sits inside a "..." argument
                    * (bash -c "...", custom override) — inner
                    * quotes need \" escaping */
    SH_CMDLINE = 1 /* cmd.exe /s /c "..." — /s strips ONLY the
                    * outer quotes, inner quotes pass through */
} sh_kind;

/* Choose the shell invocation format string (single %s = the command).
 * Honors SHELL override (e.g. "C:\\...\\bash.exe -c"), then probes for a
 * POSIX shell so POSIX commands (mkdir -p, cp, ls) work on Windows, and
 * finally falls back to cmd.exe. */
static const char *shell_fmt_kind(sh_kind *kind) {
    static char buf[768];
    static int done = 0;
    int nb = 0;

    if (done) {
        *kind = (strstr(buf, "cmd.exe /s /c") == buf) ? SH_CMDLINE : SH_QUOTED;
        return buf;
    }

    done = 1;
    *kind = SH_QUOTED;
    const char *override = getenv("COA_SHELL");
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
    const char *pf = getenv("ProgramFiles");
    const char *pf86 = getenv("ProgramFiles(x86)");
    const char *local = getenv("LOCALAPPDATA");
    if (pf)
        snprintf(base[nb++], 64, "%s", pf);
    if (pf86)
        snprintf(base[nb++], 64, "%s", pf86);
    if (local)
        snprintf(base[nb++], 64, "%s\\Programs", local);
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
                if (slash)
                    *slash = '\0';
                char posixdir[320];
                to_posix_dir(dir, posixdir, sizeof(posixdir));
                snprintf(buf, sizeof(buf), "\"%s\" -c \"PATH=\\\"%s:$PATH\\\"; export PATH; %%s\"", path, posixdir);
                return buf;
            }
        }
    }

    /* /s makes cmd strip ONLY the outer quotes of the /c argument, so inner
     * quotes in the command survive (plain /c mangles multi-quoted lines). */
    *kind = SH_CMDLINE;
    snprintf(buf, sizeof(buf), "cmd.exe /s /c \"%%s\"");
    return buf;
}

/* Compose the full CreateProcess command line for `cmd` according to the
 * chosen backend's quoting style. */
static void compose_shell_command(const char *cmd, char *full, size_t cap) {
    sh_kind kind = SH_QUOTED;
    const char *fmt = shell_fmt_kind(&kind);
    if (kind == SH_QUOTED) {
        /* the command is embedded inside a double-quoted argument: escape
         * inner quotes so e.g. python -c "import pptx; ..." reaches the
         * shell intact */
        char *esc = (char *)malloc(strlen(cmd) * 2 + 1);
        if (esc) {
            char *o = esc;
            for (const char *p = cmd; *p; p++) {
                if (*p == '"')
                    *o++ = '\\';
                *o++ = *p;
            }
            *o = '\0';
            snprintf(full, cap, fmt, esc);
            free(esc);
            return;
        }
    }

    snprintf(full, cap, fmt, cmd);
}

/* UTF-8 -> UTF-16 (malloc'd). CreateProcessA treats the command line in the
 * system ANSI code page (GBK on Chinese Windows), so UTF-8 command text with
 * non-ASCII characters reaches the child mangled — LLM-planned shell commands
 * containing Chinese then fail inside python/bash. All process creation in
 * this file therefore goes through the W API with an explicit CP_UTF8
 * conversion. Returns NULL on conversion failure. */
static wchar_t *utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }

    return w;
}

proc_result *proc_run_in(const char *cmd, int timeout_ms, const char *cwd) {
    char full[4096];
    proc_result *r;
    char *buf;
    int64_t deadline;
    int alive = 1;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return NULL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.hStdOutput = wr;
    si.hStdError = wr;
    /* STARTF_USESTDHANDLES with a NULL hStdInput leaves the child with an
     * invalid stdin: python subprocess (and anything calling GetStdHandle)
     * then fails with "handle is invalid" when it spawns its own children.
     * Point stdin at the NUL device instead. */
    si.hStdInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, 0, NULL);
    si.dwFlags |= STARTF_USESTDHANDLES;

    compose_shell_command(cmd, full, sizeof(full));
    wchar_t *wfull = utf8_to_wide(full);

    /* lpCurrentDirectory must be a FULL path; a relative one makes
     * CreateProcess fail (or behave nondeterministically). Resolve first. */
    wchar_t wabs_cwd[1024];
    wchar_t *cwd_heap = NULL; /* allocation to free after CreateProcess */
    const wchar_t *cwd_arg = NULL;
    if (cwd && *cwd) {
        wchar_t *wcwd = utf8_to_wide(cwd);
        if (wcwd) {
            if (GetFullPathNameW(wcwd, 1024, wabs_cwd, NULL) && wabs_cwd[0])
                cwd_arg = wabs_cwd;
            else
                cwd_arg = wcwd;
            cwd_heap = wcwd; /* wcwd is always heap — free it either way */
        }
    }

    BOOL ok = FALSE;
    if (wfull)
        ok = CreateProcessW(NULL, wfull, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd_arg, &si, &pi);
    free(wfull);
    free(cwd_heap);
    CloseHandle(wr);
    if (si.hStdInput && si.hStdInput != INVALID_HANDLE_VALUE)
        CloseHandle(si.hStdInput); /* child inherited its own copy */
    if (!ok) {
        CloseHandle(rd);
        return NULL;
    }

    r = calloc(1, sizeof(proc_result));
    if (!r) {
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return NULL;
    }

    buf = malloc(65536);
    size_t cap = 65536, len = 0;
    if (!buf) {
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        free(r);
        return NULL;
    }

    deadline = timeout_ms > 0 ? time_now_ms() + timeout_ms : 0;
    while (alive) {
        if (timeout_ms > 0 && time_now_ms() >= deadline)
            break;
        DWORD avail = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD to_read = avail;
            if (len + to_read + 1 > cap) {
                cap = (len + to_read + 1) * 2;
                char *nb = realloc(buf, cap);
                if (!nb)
                    break;
                buf = nb;
            }
            DWORD got = 0;
            if (!ReadFile(rd, buf + len, to_read, &got, NULL))
                break;
            len += got;
        } else {
            /* No data available (avail==0), or the pipe is broken because the
             * child closed its stdout/stderr. Either way, keep polling until the
             * process actually exits so a fast child is not mistaken for a
             * timeout (GetExitCodeProcess can transiently report STILL_ACTIVE
             * right after the child closes the pipe). */
            DWORD code = 0;
            if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE)
                alive = 0;
            else
                time_sleep_ms(5);
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
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
        buf[--len] = '\0';

    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
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

int proc_spawn_detached(const char *cmd) {
    char full[4096];

    if (!cmd || !*cmd)
        return -1;
    compose_shell_command(cmd, full, sizeof(full));
    wchar_t *wfull = utf8_to_wide(full);
    if (!wfull)
        return -1;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    BOOL ok = CreateProcessW(NULL, wfull, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, NULL, NULL,
                             &si, &pi);
    free(wfull);
    if (!ok)
        return -1;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

/* --- Persistent piped child process (for stdio MCP servers) --- */

struct proc_popen {
    HANDLE proc;
    HANDLE in_wr;  /* write end of child stdin */
    HANDLE out_rd; /* read end of child stdout */
    char *buf;
    size_t len, cap;
};

/* Quote a single argv element for a Windows command line. */
static void quote_arg(const char *a, char *out, size_t cap) {
    size_t o = 0;
    if (o < cap)
        out[o++] = '"';
    for (const char *p = a; *p && o + 2 < cap; p++) {
        if (*p == '"') {
            if (o + 2 < cap) {
                out[o++] = '\\';
                out[o++] = '"';
            }
        } else
            out[o++] = *p;
    }

    if (o < cap)
        out[o++] = '"';
    out[o] = '\0';
}

proc_popen *proc_popen_new(char *const argv[]) {
    return proc_popen_new_ex(argv, 0);
}

proc_popen *proc_popen_new_ex(char *const argv[], int merge_stderr) {
    char cmdline[4096];
    size_t off;
    proc_popen *p;

    if (!argv || !argv[0])
        return NULL;

    /* Build "cmd.exe /s /c "<argv0> <argv1> ..."" so batch shims like npx.cmd
     * also work. /s makes cmd strip ONLY the outer quotes, preserving the
     * per-argument quotes (plain /c mangles multi-quoted command lines).
     * argv[0] stays bare when it has no spaces: quoting a bare command name
     * breaks cmd's PATH-search semantics (%~dp0 inside the resolved .cmd
     * shim then resolves to the CWD instead of the shim's directory, which
     * crashes nvm4w/npm shims instantly). */
    off = (size_t)snprintf(cmdline, sizeof(cmdline), "cmd.exe /s /c \"");
    for (int i = 0; argv[i] && off < sizeof(cmdline); i++) {
        char q[800];
        if (i == 0 && !strchr(argv[i], ' ') && !strchr(argv[i], '"'))
            snprintf(q, sizeof(q), "%s", argv[i]);
        else
            quote_arg(argv[i], q, sizeof(q));
        int wr;
        if (i == 0) {
            /* argv[0] must be the FIRST character after the opening quote:
             * cmd /s only strips the outer quotes when a quote (not space)
             * directly follows /c — a leading space leaves the quotes intact
             * and cmd falls back to interactive stdin-eating. */
            wr = snprintf(cmdline + off, sizeof(cmdline) - off, "%s", q);
        } else {
            wr = snprintf(cmdline + off, sizeof(cmdline) - off, " %s", q);
        }
        if (wr < 0 || (size_t)wr >= sizeof(cmdline) - off)
            break;
        off += (size_t)wr;
    }

    if (off < sizeof(cmdline) - 1) {
        cmdline[off++] = '"';
        cmdline[off] = '\0';
    }

    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE in_rd = NULL, in_wr = NULL, out_rd = NULL, out_wr = NULL;
    if (!CreatePipe(&in_rd, &in_wr, &sa, 0))
        return NULL;
    if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) {
        CloseHandle(in_rd);
        CloseHandle(in_wr);
        return NULL;
    }

    SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);

    /* child stderr -> NUL so server logs never pollute the JSON stream */
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_rd;
    si.hStdOutput = out_wr;
    si.hStdError = merge_stderr ? out_wr : (nul ? nul : out_wr);

    BOOL ok = FALSE;
    {
        wchar_t *wcmdline = utf8_to_wide(cmdline);
        if (wcmdline) {
            ok = CreateProcessW(NULL, wcmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
            free(wcmdline);
        }
    }

    CloseHandle(in_rd);
    CloseHandle(out_wr);
    if (nul)
        CloseHandle(nul);
    if (!ok) {
        CloseHandle(in_wr);
        CloseHandle(out_rd);
        return NULL;
    }

    CloseHandle(pi.hThread);

    p = calloc(1, sizeof(*p));
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

int proc_popen_write(proc_popen *p, const char *data, size_t len) {
    if (!p || !data)
        return -1;
    DWORD written = 0;
    if (!WriteFile(p->in_wr, data, (DWORD)len, &written, NULL) || written != len)
        return -1;
    return 0;
}

size_t proc_popen_read(proc_popen *p, int timeout_ms) {
    int64_t deadline;
    size_t start_len;

    if (!p)
        return 0;
    deadline = timeout_ms > 0 ? time_now_ms() + timeout_ms : 0;
    start_len = p->len;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(p->out_rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            if (p->len + avail + 1 > p->cap) {
                size_t ncap = (p->len + avail + 1) * 2;
                char *nb = realloc(p->buf, ncap);
                if (!nb)
                    break;
                p->buf = nb;
                p->cap = ncap;
            }
            DWORD got = 0;
            if (!ReadFile(p->out_rd, p->buf + p->len, avail, &got, NULL) || got == 0)
                break;
            p->len += got;
            p->buf[p->len] = '\0';
            return p->len - start_len; /* return after one read burst */
        }
        /* no data: stop when dead or deadline passed */
        DWORD code = 0;
        if (GetExitCodeProcess(p->proc, &code) && code != STILL_ACTIVE)
            break;
        if (timeout_ms > 0 && time_now_ms() >= deadline)
            break;
        time_sleep_ms(10);
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
    if (!p)
        return 0;
    DWORD code = 0;
    if (GetExitCodeProcess(p->proc, &code) && code != STILL_ACTIVE)
        return 0;
    return 1;
}

void proc_popen_free(proc_popen *p) {
    if (!p)
        return;
    if (p->proc != INVALID_HANDLE_VALUE) {
        TerminateProcess(p->proc, 1);
        CloseHandle(p->proc);
    }

    if (p->in_wr)
        CloseHandle(p->in_wr);
    if (p->out_rd)
        CloseHandle(p->out_rd);
    free(p->buf);
    free(p);
}

