/* sandbox.c — process execution sandbox. */
#include "cagent/plugin_runtime/sandbox.h"
#include "cagent/os/os_proc.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>

struct ca_sandbox { int timeout_ms; };

ca_sandbox *ca_sandbox_new(int timeout_ms) {
    ca_sandbox *sb = (ca_sandbox *)calloc(1, sizeof(ca_sandbox));
    if (sb) sb->timeout_ms = timeout_ms;
    return sb;
}

void ca_sandbox_free(ca_sandbox *sb) { free(sb); }

static const char *FORBIDDEN[] = {
    "rm -rf", "rm -fr", "mkfs", "format c:", "shutdown", "reboot",
    "dd if=", ":(){", "fork bomb", "del /s", "del /q", "rd /s",
    "> /dev/sda", "chmod 777 /", "sudo rm", "del /f /s"
};
#define N_FORBIDDEN (sizeof(FORBIDDEN) / sizeof(FORBIDDEN[0]))

static int ci_contains(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    size_t hlen = strlen(hay);
    if (nlen == 0 || hlen < nlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for (j = 0; j < nlen; j++) {
            int a = (unsigned char)hay[i + j], b = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

int ca_sandbox_forbidden(const char *cmd) {
    if (!cmd) return 0;
    for (size_t i = 0; i < N_FORBIDDEN; i++)
        if (ci_contains(cmd, FORBIDDEN[i])) return 1;
    return 0;
}

ca_sandbox_result *ca_sandbox_run(ca_sandbox *sb, const char *cmd) {
    if (!cmd || ca_sandbox_forbidden(cmd)) return NULL;
    ca_proc_result *pr = ca_proc_run(cmd, sb ? sb->timeout_ms : 0);
    if (!pr) return NULL;
    ca_sandbox_result *r = (ca_sandbox_result *)calloc(1, sizeof(ca_sandbox_result));
    if (!r) { ca_proc_result_free(pr); return NULL; }
    r->exit_code = pr->exit_code;
    r->timed_out = pr->timed_out;
    r->ok = (pr->exit_code == 0 && !pr->timed_out) ? 1 : 0;
    r->output = pr->output ? ca_strdup(pr->output) : ca_strdup("");
    ca_proc_result_free(pr);
    return r;
}

void ca_sandbox_result_free(ca_sandbox_result *r) {
    if (!r) return;
    free(r->output);
    free(r);
}

/* --- Wasm seam --- */
static ca_sandbox_wasm_fn g_wasm_runner = NULL;

void ca_sandbox_set_wasm_runner(ca_sandbox_wasm_fn fn) {
    g_wasm_runner = fn;
}

int ca_sandbox_wasm_supported(void) {
    return g_wasm_runner != NULL;
}

char *ca_sandbox_run_wasm(const void *wasm, size_t wasm_len,
                          const char *fn_name, const char *args_json) {
    if (!g_wasm_runner)
        return ca_strdup("{\"error\":\"wasm runtime not registered\"}");
    return g_wasm_runner(wasm, wasm_len, fn_name, args_json);
}
