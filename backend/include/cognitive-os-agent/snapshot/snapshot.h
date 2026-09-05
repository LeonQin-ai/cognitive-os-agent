/* snapshot.h — file snapshot engine (Copy-On-Write).
 * Captures the original content of files before they are modified, stores it
 * in a content-addressed block store, and can restore originals (rollback).
 * Manifests and blocks live under <state_root>/snapshots. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_snapshot ca_snapshot;

/* Open (load) the snapshot store rooted at state_root. NULL on failure. */
ca_snapshot *ca_snapshot_open(const char *state_root);
void ca_snapshot_close(ca_snapshot *s);

/* Capture size limit in bytes (0 = unlimited). Overrides the built-in default
 * (64MB) and the CA_SNAPSHOT_MAX_FILE env value; persists via config. */
void ca_snapshot_set_max_file(ca_snapshot *s, long long bytes);
long long ca_snapshot_get_max_file(const ca_snapshot *s);

/* Capture the current content of `path` into the pending snapshot.
 * If the file does not exist, records it as "to be created" so rollback deletes it.
 * Returns 0 ok, -1 error. */
int ca_snapshot_capture(ca_snapshot *s, const char *path);
/* Capture a set of paths given as a JSON array of strings. */
int ca_snapshot_capture_json(ca_snapshot *s, const char *paths_json);

/* Commit the pending snapshot. Returns a stable id (borrowed) or NULL. */
const char *ca_snapshot_commit(ca_snapshot *s);
/* Abandon the pending snapshot without persisting it. */
void ca_snapshot_abort(ca_snapshot *s);

/* List committed snapshots as a JSON array of {id, created, files}. Caller frees. */
char *ca_snapshot_list(ca_snapshot *s);

/* Restore the newest committed snapshot (rollback). Returns 0 ok, -1 error. */
int ca_snapshot_restore_latest(ca_snapshot *s);
/* Restore a specific snapshot by id. */
int ca_snapshot_restore(ca_snapshot *s, const char *id);
/* Restore the in-progress (uncommitted) captures and clear them. Used by ROLLBACK. */
int ca_snapshot_restore_pending(ca_snapshot *s);

#ifdef __cplusplus
}
#endif
