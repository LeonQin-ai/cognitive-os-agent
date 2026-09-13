/* tx.h — action transaction manager.
 * Lifecycle: BEGIN -> snapshot capture -> execute actions -> validate ->
 *            COMMIT (keep changes, checkpoint) | ROLLBACK (restore originals).
 * A transaction wraps the snapshot engine and a tool registry. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tx_manager tx_manager;
typedef struct tx tx;
typedef struct snapshot snapshot;
typedef struct tool_registry tool_registry;
typedef struct tool_ctx tool_ctx;

tx_manager *tx_manager_new(void);
void tx_manager_free(tx_manager *m);

/* Begin a transaction. snap/tools/ctx may be NULL (capture/execute become no-ops). */
tx *tx_begin(tx_manager *m, snapshot *snap, tool_registry *tools, const tool_ctx *ctx);

/* Capture a path for rollback, then execute a tool. Returns 0 ok, -1 if denied/failed. */
int tx_run(tx *tx, const char *tool_name, const char *args_json);

/* Validate: true if every executed action succeeded. */
int tx_validate(tx *tx);

/* Accumulated tool output of every executed action, as "[tool] output\n" lines
 * (borrowed; valid until tx_free). NULL/"" if no action ran yet. */
const char *tx_output(tx *tx);

/* Commit: persist snapshot checkpoint; keep file changes. Returns 0 ok. */
int tx_commit(tx *tx);
/* Rollback: restore captured originals; discard snapshot. Returns 0 ok. */
int tx_rollback(tx *tx);

void tx_free(tx *tx);

#ifdef __cplusplus
}
#endif
