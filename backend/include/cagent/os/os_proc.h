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

/* Persistent child process with piped stdin/stdout (for stdio MCP servers).
 * NOTE: unlike ca_proc_run*, this spawns `cmd` DIRECTLY (no shell): it takes
 * an argv-style NULL-terminated array. The child's stderr goes to the parent's
 * stderr. */
typedef struct ca_proc_popen ca_proc_popen;

/* Spawn argv[0] with argv. Returns NULL on spawn failure. */
ca_proc_popen *ca_proc_popen_new(char *const argv[]);

/* Write bytes to the child's stdin. Returns 0 ok, -1 error/pipe closed. */
int ca_proc_popen_write(ca_proc_popen *p, const char *data, size_t len);

/* Poll the child's stdout for up to timeout_ms, appending whatever arrives to
 * the internal buffer. Returns the number of NEW bytes read (0 = nothing yet;
 * process death also yields whatever remains, check alive). */
size_t ca_proc_popen_read(ca_proc_popen *p, int timeout_ms);

/* Borrowed view of everything read so far (NUL-terminated). */
const char *ca_proc_popen_buffer(ca_proc_popen *p);

/* Discard everything read so far. */
void ca_proc_popen_reset(ca_proc_popen *p);

/* Discard the first `n` bytes of the read buffer, keeping the remainder
 * (used by line-oriented consumers that consume only complete lines). */
void ca_proc_popen_trim(ca_proc_popen *p, size_t n);

/* 1 if the child has exited. */
int ca_proc_popen_alive(ca_proc_popen *p);

/* Kill the child and free. */
void ca_proc_popen_free(ca_proc_popen *p);

#ifdef __cplusplus
}
#endif
