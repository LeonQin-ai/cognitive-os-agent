/* tx.h — action transaction manager.
 * Lifecycle: BEGIN -> snapshot capture -> execute actions -> validate ->
 *            COMMIT (keep changes, checkpoint) | ROLLBACK (restore originals).
 * A transaction wraps the snapshot engine and a tool registry. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_tx_manager coa_tx_manager;
typedef struct coa_tx coa_tx;
typedef struct coa_snapshot coa_snapshot;
typedef struct coa_tool_registry coa_tool_registry;
typedef struct coa_tool_ctx coa_tool_ctx;

coa_tx_manager *coa_tx_manager_new(void);
void coa_tx_manager_free(coa_tx_manager *m);

/* Begin a transaction. snap/tools/ctx may be NULL (capture/execute become no-ops). */
coa_tx *coa_tx_begin(coa_tx_manager *m, coa_snapshot *snap, coa_tool_registry *tools,
                   const coa_tool_ctx *ctx);

/* Capture a path for rollback, then execute a tool. Returns 0 ok, -1 if denied/failed. */
int coa_tx_run(coa_tx *tx, const char *tool_name, const char *args_json);

/* Validate: true if every executed action succeeded. */
int coa_tx_validate(coa_tx *tx);

/* Accumulated tool output of every executed action, as "[tool] output\n" lines
 * (borrowed; valid until coa_tx_free). NULL/"" if no action ran yet. */
const char *coa_tx_output(coa_tx *tx);

/* Commit: persist snapshot checkpoint; keep file changes. Returns 0 ok. */
int coa_tx_commit(coa_tx *tx);
/* Rollback: restore captured originals; discard snapshot. Returns 0 ok. */
int coa_tx_rollback(coa_tx *tx);

void coa_tx_free(coa_tx *tx);

#ifdef __cplusplus
}
#endif
