/* sandbox.h — process execution sandbox.
 * Runs commands with a timeout and a deny-list of dangerous operations. This is
 * the isolation primitive the plugin runtime uses before executing untrusted
 * plugin shell steps. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_sandbox ca_sandbox;

typedef struct ca_sandbox_result {
    int ok;            /* 1 = exit code 0 and not timed out */
    int exit_code;
    int timed_out;
    char *output;      /* combined stdout+stderr (malloc'd) */
} ca_sandbox_result;

ca_sandbox *ca_sandbox_new(int timeout_ms);   /* timeout_ms <= 0 = none */
void ca_sandbox_free(ca_sandbox *sb);

/* Deny-list check: returns 1 if `cmd` is forbidden, 0 otherwise. */
int ca_sandbox_forbidden(const char *cmd);

/* Run `cmd` in the sandbox. Returns NULL if the command is forbidden or spawn
 * failed (call ca_sandbox_forbidden() first to distinguish). Caller frees with
 * ca_sandbox_result_free(). */
ca_sandbox_result *ca_sandbox_run(ca_sandbox *sb, const char *cmd);
void ca_sandbox_result_free(ca_sandbox_result *r);

/* --- Wasm seam ---
 * The native sandbox executes untrusted shell. Wasm is the upgrade path for
 * stronger isolation: a host embeds a Wasm runtime (wasm3/wasmtime) and
 * registers a runner callback here. The seam is the stable interface the
 * plugin runtime calls; until a runner is registered, Wasm execution reports
 * "unsupported" without crashing. */
typedef char *(*ca_sandbox_wasm_fn)(const void *wasm, size_t wasm_len,
                                    const char *fn_name, const char *args_json);

/* Register a Wasm runner (set once, before any wasm execution). */
void ca_sandbox_set_wasm_runner(ca_sandbox_wasm_fn fn);
/* 1 if a runner is registered, else 0. */
int ca_sandbox_wasm_supported(void);
/* Invoke `fn_name` in `wasm` with JSON `args_json`. Returns a malloc'd JSON
 * result string (caller frees), or a malloc'd error string if unsupported. */
char *ca_sandbox_run_wasm(const void *wasm, size_t wasm_len,
                          const char *fn_name, const char *args_json);

#ifdef __cplusplus
}
#endif
