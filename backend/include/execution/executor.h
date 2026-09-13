/* executor.h — Execution Runtime stable interface (architecture v1.0 §9).
 *
 * Tool execution sits behind an executor vtable so the Agent Runtime never
 * hard-depends on HOW actions run. Today: LocalExecutor (delegates to the
 * tool registry). Tomorrow: Sandbox / VM / WSL / Remote / Cluster executors
 * implement the same ops without touching the reasoning engine.
 *
 * Baseline→Execute→Verify→Commit/Rollback stays in the tx/snapshot layer;
 * executor.snapshot/restore expose the capability to executors that own
 * their own state (VMs, sandboxes). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct executor executor;

/* Result of one execution. `output` is heap; free with
 * executor_result_free. */
typedef struct executor_result {
    int ok;
    char *output;
} executor_result;

void executor_result_free(executor_result *r);

/* vtable — impl is the executor's private state. */
typedef struct executor_ops {
    const char *name; /* "local", "sandbox", "vm", ... */
    int (*start)(void *impl);
    /* run one action; returns 0 and fills *result (always non-NULL on rc 0),
     * nonzero on infrastructure failure (tool-level failure stays in result.ok) */
    int (*execute)(void *impl, const char *tool, const char *args_json, executor_result **result);
    int (*stop)(void *impl);
    void (*destroy)(void *impl);
    /* optional state capture for executors that own their environment;
     * return -1 when unsupported */
    int (*snapshot)(void *impl, char **snapshot_id);
    int (*restore)(void *impl, const char *snapshot_id);
} executor_ops;

struct executor {
    const executor_ops *ops;
    void *impl;
};

/* --- LocalExecutor: delegates to the tool registry (tool registry ctx) --- */
struct tool_registry;
struct tool_ctx;
executor *executor_new_local(struct tool_registry *reg, struct tool_ctx *tctx,
                                     void *snapshot /* snapshot*, may be NULL */);

/* --- Routing executors (architecture v1.0 §9 Executor family) ---
 * Wrap an inner executor and forward every action to it, rewriting `shell`
 * tool commands to run inside the target environment (WSL distro / remote
 * host over ssh, POSIX-quoted). Non-shell tools pass through unchanged.
 * The wrapper owns `inner` (destroyed with the wrapper). `distro` may be
 * NULL for the WSL default. */
executor *executor_new_wsl(executor *inner, const char *distro);
executor *executor_new_remote(executor *inner, const char *host);

/* Generic lifecycle over any vtable. */
executor *executor_new(const executor_ops *ops, void *impl);
void executor_free(executor *e);
const char *executor_name(const executor *e);

/* Run one action. Returns 0 ok (*result filled, caller frees), -1 infra error. */
int executor_execute(executor *e, const char *tool, const char *args_json, executor_result **result);
int executor_start(executor *e);
int executor_stop(executor *e);
int executor_snapshot(executor *e, char **snapshot_id);
int executor_restore(executor *e, const char *snapshot_id);

#ifdef __cplusplus
}
#endif
