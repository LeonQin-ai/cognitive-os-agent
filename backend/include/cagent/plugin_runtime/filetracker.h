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

typedef struct ca_filetracker ca_filetracker;

/* operation bits (OR together) */
#define CA_FT_READ  0x1
#define CA_FT_WRITE 0x2   /* created or modified */
#define CA_FT_DELETE 0x4
#define CA_FT_EXEC  0x8

ca_filetracker *ca_filetracker_new(void);
void ca_filetracker_free(ca_filetracker *ft);
/* Forget everything recorded so far. */
void ca_filetracker_clear(ca_filetracker *ft);

/* Merge ops for `path` (dedup by exact path string; ops OR in).
 * Returns the entry's accumulated op mask, or 0 on bad args. */
int ca_filetracker_record(ca_filetracker *ft, const char *path, int ops);

/* Distinct tracked paths. */
int ca_filetracker_count(ca_filetracker *ft);

/* ops bitmask -> "read,write,delete,exec" (static buffer, do not free). */
const char *ca_filetracker_ops_str(int ops);

/* JSON array [{"path":"...","ops":"read,write"}] (caller frees). */
char *ca_filetracker_json(ca_filetracker *ft);

/* --- workspace scan (before/after diff) --- */
typedef struct ca_ft_snapshot ca_ft_snapshot;

/* Capture {path,size} of every regular file under `dir` (recursive, depth- and
 * entry-bounded). `dir` may be NULL/"" for "nothing to scan". NULL on OOM. */
ca_ft_snapshot *ca_filetracker_dir_snapshot(const char *dir);
void ca_filetracker_snapshot_free(ca_ft_snapshot *s);
/* Diff the current state of `dir` against `before`: new/size-changed files
 * record CA_FT_WRITE, vanished files record CA_FT_DELETE. Returns the number
 * of changes recorded (0 = identical). */
int ca_filetracker_dir_diff(ca_filetracker *ft, const ca_ft_snapshot *before,
                            const char *dir);

/* Parse `cmd` for path tokens (whitespace/quote separated); tokens that exist
 * (as given, or resolved against `workspace` when relative) record CA_FT_READ.
 * Returns reads recorded. */
int ca_filetracker_cmd_reads(ca_filetracker *ft, const char *cmd,
                             const char *workspace);

#ifdef __cplusplus
}
#endif
