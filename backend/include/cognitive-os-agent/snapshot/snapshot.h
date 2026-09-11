/* snapshot.h — file snapshot engine (Copy-On-Write).
 * Captures the original content of files before they are modified, stores it
 * in a content-addressed block store, and can restore originals (rollback).
 * Manifests and blocks live under <state_root>/snapshots. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct snapshot snapshot;

/* Open (load) the snapshot store rooted at state_root. NULL on failure. */
snapshot *snapshot_open(const char *state_root);
void snapshot_close(snapshot *s);

/* Capture size limit in bytes (0 = unlimited). Overrides the built-in default
 * (64MB) and the SNAPSHOT_MAX_FILE env value; persists via config. */
void snapshot_set_max_file(snapshot *s, long long bytes);
long long snapshot_get_max_file(const snapshot *s);

/* Capture the current content of `path` into the pending snapshot.
 * If the file does not exist, records it as "to be created" so rollback deletes it.
 * Returns 0 ok, -1 error. */
int snapshot_capture(snapshot *s, const char *path);

/* Commit the pending snapshot. Returns a stable id (borrowed) or NULL. */
const char *snapshot_commit(snapshot *s);
/* Abandon the pending snapshot without persisting it. */
void snapshot_abort(snapshot *s);

/* List committed snapshots as a JSON array of {id, created, files}. Caller frees. */
char *snapshot_list(snapshot *s);

/* Restore the newest committed snapshot (rollback). Returns 0 ok, -1 error. */
int snapshot_restore_latest(snapshot *s);
/* Restore a specific snapshot by id. */
int snapshot_restore(snapshot *s, const char *id);
/* Restore the in-progress (uncommitted) captures and clear them. Used by ROLLBACK. */
int snapshot_restore_pending(snapshot *s);

#ifdef __cplusplus
}
#endif
