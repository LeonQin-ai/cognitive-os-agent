/* tx.h — action transaction manager.
 * Lifecycle: BEGIN -> snapshot capture -> execute actions -> validate ->
 *            COMMIT (keep changes, checkpoint) | ROLLBACK (restore originals).
 * A transaction wraps the snapshot engine and a tool registry. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_tx_manager ca_tx_manager;
typedef struct ca_tx ca_tx;
typedef struct ca_snapshot ca_snapshot;
typedef struct ca_tool_registry ca_tool_registry;
typedef struct ca_tool_ctx ca_tool_ctx;

ca_tx_manager *ca_tx_manager_new(void);
void ca_tx_manager_free(ca_tx_manager *m);

/* Begin a transaction. snap/tools/ctx may be NULL (capture/execute become no-ops). */
ca_tx *ca_tx_begin(ca_tx_manager *m, ca_snapshot *snap, ca_tool_registry *tools,
                   const ca_tool_ctx *ctx);

/* Capture a path for rollback, then execute a tool. Returns 0 ok, -1 if denied/failed. */
int ca_tx_run(ca_tx *tx, const char *tool_name, const char *args_json);

/* Validate: true if every executed action succeeded. */
int ca_tx_validate(ca_tx *tx);

/* Accumulated tool output of every executed action, as "[tool] output\n" lines
 * (borrowed; valid until ca_tx_free). NULL/"" if no action ran yet. */
const char *ca_tx_output(ca_tx *tx);

/* Commit: persist snapshot checkpoint; keep file changes. Returns 0 ok. */
int ca_tx_commit(ca_tx *tx);
/* Rollback: restore captured originals; discard snapshot. Returns 0 ok. */
int ca_tx_rollback(ca_tx *tx);

void ca_tx_free(ca_tx *tx);

#ifdef __cplusplus
}
#endif
