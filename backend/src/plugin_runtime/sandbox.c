/* sandbox.c — process execution sandbox with file-access tracking. */
#include "cognitive-os-agent/plugin_runtime/sandbox.h"
#include "cognitive-os-agent/plugin_runtime/filetracker.h"
#include "cognitive-os-agent/os/os_proc.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>

struct sandbox {
    int timeout_ms;
    char *workspace;     /* dir scanned before/after each run (may be NULL) */
    filetracker *ft; /* created lazily with workspace */
};

sandbox *sandbox_new(int timeout_ms) {
    sandbox *sb = (sandbox *)calloc(1, sizeof(sandbox));
    if (sb)
        sb->timeout_ms = timeout_ms;
    return sb;
}

void sandbox_free(sandbox *sb) {
    if (!sb)
        return;
    free(sb->workspace);
    if (sb->ft)
        filetracker_free(sb->ft);
    free(sb);
}

void sandbox_set_workspace(sandbox *sb, const char *dir) {
    if (!sb)
        return;
    free(sb->workspace);
    sb->workspace = (dir && *dir) ? xstrdup(dir) : NULL;
    if (sb->workspace && !sb->ft)
        sb->ft = filetracker_new();
}

filetracker *sandbox_filetracker(sandbox *sb) {
    return sb ? sb->ft : NULL;
}

static const char *FORBIDDEN[] = {"rm -rf",     "rm -fr",      "mkfs",      "format c:", "shutdown", "reboot",
                                  "dd if=",     ":(){",        "fork bomb", "del /s",    "del /q",   "rd /s",
                                  "> /dev/sda", "chmod 777 /", "sudo rm",   "del /f /s"};
#define N_FORBIDDEN (sizeof(FORBIDDEN) / sizeof(FORBIDDEN[0]))

static int ci_contains(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    size_t hlen = strlen(hay);
    if (nlen == 0 || hlen < nlen)
        return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for (j = 0; j < nlen; j++) {
            int a = (unsigned char)hay[i + j], b = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b)
                break;
        }
        if (j == nlen)
            return 1;
    }
    return 0;
}

int sandbox_forbidden(const char *cmd) {
    if (!cmd)
        return 0;
    for (size_t i = 0; i < N_FORBIDDEN; i++)
        if (ci_contains(cmd, FORBIDDEN[i]))
            return 1;
    return 0;
}

sandbox_result *sandbox_run(sandbox *sb, const char *cmd) {
    if (!cmd || sandbox_forbidden(cmd))
        return NULL;
    /* file tracking: capture the workspace state + command reads before the
     * run, diff after (see filetracker.h) */
    ft_snapshot *snap = NULL;
    if (sb && sb->ft && sb->workspace) {
        filetracker_cmd_reads(sb->ft, cmd, sb->workspace);
        snap = filetracker_dir_snapshot(sb->workspace);
    }
    proc_result *pr = proc_run(cmd, sb ? sb->timeout_ms : 0);
    if (!pr) {
        if (snap)
            filetracker_snapshot_free(snap);
        return NULL;
    }
    sandbox_result *r = (sandbox_result *)calloc(1, sizeof(sandbox_result));
    if (!r) {
        if (snap)
            filetracker_snapshot_free(snap);
        proc_result_free(pr);
        return NULL;
    }
    r->exit_code = pr->exit_code;
    r->timed_out = pr->timed_out;
    r->ok = (pr->exit_code == 0 && !pr->timed_out) ? 1 : 0;
    r->output = pr->output ? xstrdup(pr->output) : xstrdup("");
    proc_result_free(pr);
    if (snap && sb->ft) {
        filetracker_dir_diff(sb->ft, snap, sb->workspace);
        r->files_json = filetracker_json(sb->ft);
    }
    if (snap)
        filetracker_snapshot_free(snap);
    return r;
}

void sandbox_result_free(sandbox_result *r) {
    if (!r)
        return;
    free(r->output);
    free(r->files_json);
    free(r);
}

/* --- Wasm seam --- */
static sandbox_wasm_fn g_wasm_runner = NULL;

void sandbox_set_wasm_runner(sandbox_wasm_fn fn) {
    g_wasm_runner = fn;
}

int sandbox_wasm_supported(void) {
    return g_wasm_runner != NULL;
}

char *sandbox_run_wasm(const void *wasm, size_t wasm_len, const char *fn_name, const char *args_json) {
    if (!g_wasm_runner)
        return xstrdup("{\"error\":\"wasm runtime not registered\"}");
    return g_wasm_runner(wasm, wasm_len, fn_name, args_json);
}
