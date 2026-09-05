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

typedef struct coa_filetracker coa_filetracker;

/* operation bits (OR together) */
#define COA_FT_READ  0x1
#define COA_FT_WRITE 0x2   /* created or modified */
#define COA_FT_DELETE 0x4
#define COA_FT_EXEC  0x8

coa_filetracker *coa_filetracker_new(void);
void coa_filetracker_free(coa_filetracker *ft);
/* Forget everything recorded so far. */
void coa_filetracker_clear(coa_filetracker *ft);

/* Merge ops for `path` (dedup by exact path string; ops OR in).
 * Returns the entry's accumulated op mask, or 0 on bad args. */
int coa_filetracker_record(coa_filetracker *ft, const char *path, int ops);

/* Distinct tracked paths. */
int coa_filetracker_count(coa_filetracker *ft);

/* ops bitmask -> "read,write,delete,exec" (static buffer, do not free). */
const char *coa_filetracker_ops_str(int ops);

/* JSON array [{"path":"...","ops":"read,write"}] (caller frees). */
char *coa_filetracker_json(coa_filetracker *ft);

/* --- workspace scan (before/after diff) --- */
typedef struct coa_ft_snapshot coa_ft_snapshot;

/* Capture {path,size} of every regular file under `dir` (recursive, depth- and
 * entry-bounded). `dir` may be NULL/"" for "nothing to scan". NULL on OOM. */
coa_ft_snapshot *coa_filetracker_dir_snapshot(const char *dir);
void coa_filetracker_snapshot_free(coa_ft_snapshot *s);
/* Diff the current state of `dir` against `before`: new/size-changed files
 * record COA_FT_WRITE, vanished files record COA_FT_DELETE. Returns the number
 * of changes recorded (0 = identical). */
int coa_filetracker_dir_diff(coa_filetracker *ft, const coa_ft_snapshot *before,
                            const char *dir);

/* Parse `cmd` for path tokens (whitespace/quote separated); tokens that exist
 * (as given, or resolved against `workspace` when relative) record COA_FT_READ.
 * Returns reads recorded. */
int coa_filetracker_cmd_reads(coa_filetracker *ft, const char *cmd,
                             const char *workspace);

#ifdef __cplusplus
}
#endif
