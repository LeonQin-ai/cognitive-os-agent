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

#ifdef __cplusplus
}
#endif
