/* snapshot.h — file snapshot engine (Copy-On-Write).
 * Captures the original content of files before they are modified, stores it
 * in a content-addressed block store, and can restore originals (rollback).
 * Manifests and blocks live under <state_root>/snapshots. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_snapshot coa_snapshot;

/* Open (load) the snapshot store rooted at state_root. NULL on failure. */
coa_snapshot *coa_snapshot_open(const char *state_root);
void coa_snapshot_close(coa_snapshot *s);

/* Capture size limit in bytes (0 = unlimited). Overrides the built-in default
 * (64MB) and the COA_SNAPSHOT_MAX_FILE env value; persists via config. */
void coa_snapshot_set_max_file(coa_snapshot *s, long long bytes);
long long coa_snapshot_get_max_file(const coa_snapshot *s);

/* Capture the current content of `path` into the pending snapshot.
 * If the file does not exist, records it as "to be created" so rollback deletes it.
 * Returns 0 ok, -1 error. */
int coa_snapshot_capture(coa_snapshot *s, const char *path);
/* Capture a set of paths given as a JSON array of strings. */
int coa_snapshot_capture_json(coa_snapshot *s, const char *paths_json);

/* Commit the pending snapshot. Returns a stable id (borrowed) or NULL. */
const char *coa_snapshot_commit(coa_snapshot *s);
/* Abandon the pending snapshot without persisting it. */
void coa_snapshot_abort(coa_snapshot *s);

/* List committed snapshots as a JSON array of {id, created, files}. Caller frees. */
char *coa_snapshot_list(coa_snapshot *s);

/* Restore the newest committed snapshot (rollback). Returns 0 ok, -1 error. */
int coa_snapshot_restore_latest(coa_snapshot *s);
/* Restore a specific snapshot by id. */
int coa_snapshot_restore(coa_snapshot *s, const char *id);
/* Restore the in-progress (uncommitted) captures and clear them. Used by ROLLBACK. */
int coa_snapshot_restore_pending(coa_snapshot *s);

#ifdef __cplusplus
}
#endif
