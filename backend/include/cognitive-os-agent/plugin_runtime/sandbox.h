/* sandbox.h — process execution sandbox.
 * Runs commands with a timeout and a deny-list of dangerous operations. This is
 * the isolation primitive the plugin runtime uses before executing untrusted
 * plugin shell steps.
 *
 * File tracking (filetracker.h): when a workspace directory is configured via
 * coa_sandbox_set_workspace(), every run records which files the command read
 * (command tokens that exist), wrote or deleted (before/after workspace scan)
 * into the sandbox's tracker, exposed as result->files_json. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_sandbox coa_sandbox;
typedef struct coa_filetracker coa_filetracker;

typedef struct coa_sandbox_result {
    int ok;            /* 1 = exit code 0 and not timed out */
    int exit_code;
    int timed_out;
    char *output;      /* combined stdout+stderr (malloc'd) */
    char *files_json;  /* touched-file audit "[{path,ops}]" or NULL (malloc'd) */
} coa_sandbox_result;

coa_sandbox *coa_sandbox_new(int timeout_ms);   /* timeout_ms <= 0 = none */
void coa_sandbox_free(coa_sandbox *sb);

/* Deny-list check: returns 1 if `cmd` is forbidden, 0 otherwise. */
int coa_sandbox_forbidden(const char *cmd);

/* Run `cmd` in the sandbox. Returns NULL if the command is forbidden or spawn
 * failed (call coa_sandbox_forbidden() first to distinguish). Caller frees with
 * coa_sandbox_result_free(). */
coa_sandbox_result *coa_sandbox_run(coa_sandbox *sb, const char *cmd);
void coa_sandbox_result_free(coa_sandbox_result *r);

/* Enable file tracking: sandboxed runs scan `dir` before/after and record
 * file accesses (see filetracker.h). NULL/"" disables tracking. */
void coa_sandbox_set_workspace(coa_sandbox *sb, const char *dir);
/* The sandbox's tracker (borrowed; valid until coa_sandbox_free). */
coa_filetracker *coa_sandbox_filetracker(coa_sandbox *sb);

/* --- Wasm seam ---
 * The native sandbox executes untrusted shell. Wasm is the upgrade path for
 * stronger isolation: a host embeds a Wasm runtime (wasm3/wasmtime) and
 * registers a runner callback here. The seam is the stable interface the
 * plugin runtime calls; until a runner is registered, Wasm execution reports
 * "unsupported" without crashing. */
typedef char *(*coa_sandbox_wasm_fn)(const void *wasm, size_t wasm_len,
                                    const char *fn_name, const char *args_json);

/* Register a Wasm runner (set once, before any wasm execution). */
void coa_sandbox_set_wasm_runner(coa_sandbox_wasm_fn fn);
/* 1 if a runner is registered, else 0. */
int coa_sandbox_wasm_supported(void);
/* Invoke `fn_name` in `wasm` with JSON `args_json`. Returns a malloc'd JSON
 * result string (caller frees), or a malloc'd error string if unsupported. */
char *coa_sandbox_run_wasm(const void *wasm, size_t wasm_len,
                          const char *fn_name, const char *args_json);

#ifdef __cplusplus
}
#endif
