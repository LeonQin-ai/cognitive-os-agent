/* filetracker.h — file-access tracking for the sandbox (Execution Runtime
 * observability). While a sandboxed command runs, the tracker records which
 * files it touched:
 *   - reads:    path tokens of the command that exist on disk
 *   - writes:   files created or size-changed between the before/after
 *               workspace scan
 *   - deletes:  files present before, gone after
 * The tracker powers auditing ("what did this plugin do?") and future
 * auto-rollback of sandboxed steps. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct filetracker filetracker;

/* operation bits (OR together) */
#define FT_READ 0x1
#define FT_WRITE 0x2 /* created or modified */
#define FT_DELETE 0x4
#define FT_EXEC 0x8

filetracker *filetracker_new(void);
void filetracker_free(filetracker *ft);
/* Forget everything recorded so far. */
void filetracker_clear(filetracker *ft);

/* Merge ops for `path` (dedup by exact path string; ops OR in).
 * Returns the entry's accumulated op mask, or 0 on bad args. */
int filetracker_record(filetracker *ft, const char *path, int ops);

/* Distinct tracked paths. */
int filetracker_count(filetracker *ft);

/* ops bitmask -> "read,write,delete,exec" (static buffer, do not free). */
const char *filetracker_ops_str(int ops);

/* JSON array [{"path":"...","ops":"read,write"}] (caller frees). */
char *filetracker_json(filetracker *ft);

/* --- workspace scan (before/after diff) --- */
typedef struct ft_snapshot ft_snapshot;

/* Capture {path,size} of every regular file under `dir` (recursive, depth- and
 * entry-bounded). `dir` may be NULL/"" for "nothing to scan". NULL on OOM. */
ft_snapshot *filetracker_dir_snapshot(const char *dir);
void filetracker_snapshot_free(ft_snapshot *s);
/* Diff the current state of `dir` against `before`: new/size-changed files
 * record FT_WRITE, vanished files record FT_DELETE. Returns the number
 * of changes recorded (0 = identical). */
int filetracker_dir_diff(filetracker *ft, const ft_snapshot *before, const char *dir);

/* Parse `cmd` for path tokens (whitespace/quote separated); tokens that exist
 * (as given, or resolved against `workspace` when relative) record FT_READ.
 * Returns reads recorded. */
int filetracker_cmd_reads(filetracker *ft, const char *cmd, const char *workspace);

#ifdef __cplusplus
}
#endif
