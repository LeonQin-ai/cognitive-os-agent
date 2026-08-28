/* os_proc.h — run a shell command and capture its combined output. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_proc_result {
    char *output;      /* combined stdout+stderr (malloc'd, may be empty) */
    int exit_code;     /* process exit code; -1 if killed/timed out */
    int timed_out;     /* 1 if killed due to timeout */
} ca_proc_result;

/* Run `cmd` through the platform shell, capture combined output.
 * timeout_ms <= 0 = no timeout. Returns NULL only on spawn failure.
 * Caller frees with ca_proc_result_free. */
ca_proc_result *ca_proc_run(const char *cmd, int timeout_ms);

/* Like ca_proc_run but sets the child working directory to `cwd`
 * (NULL or empty = inherit). Relative paths/redirects resolve against cwd. */
ca_proc_result *ca_proc_run_in(const char *cmd, int timeout_ms, const char *cwd);
void ca_proc_result_free(ca_proc_result *r);

/* Fire-and-forget: launch `cmd` through the platform shell, detached from the
 * caller (no console window, no inherited stdio, survives after return). The
 * child runs in its own process group so long-running servers (e.g. `ollama
 * serve`) keep running after this call returns. Returns 0 on spawn success,
 * -1 on failure. Does NOT wait or capture output. */
int ca_proc_spawn_detached(const char *cmd);

#ifdef __cplusplus
}
#endif
