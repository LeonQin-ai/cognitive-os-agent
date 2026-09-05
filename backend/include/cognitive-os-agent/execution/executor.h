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

typedef struct ca_executor ca_executor;

/* Result of one execution. `output` is heap; free with
 * ca_executor_result_free. */
typedef struct ca_executor_result {
    int ok;
    char *output;
} ca_executor_result;

void ca_executor_result_free(ca_executor_result *r);

/* vtable — impl is the executor's private state. */
typedef struct ca_executor_ops {
    const char *name; /* "local", "sandbox", "vm", ... */
    int  (*start)(void *impl);
    /* run one action; returns 0 and fills *result (always non-NULL on rc 0),
     * nonzero on infrastructure failure (tool-level failure stays in result.ok) */
    int  (*execute)(void *impl, const char *tool, const char *args_json,
                    ca_executor_result **result);
    int  (*stop)(void *impl);
    void (*destroy)(void *impl);
    /* optional state capture for executors that own their environment;
     * return -1 when unsupported */
    int  (*snapshot)(void *impl, char **snapshot_id);
    int  (*restore)(void *impl, const char *snapshot_id);
} ca_executor_ops;

struct ca_executor {
    const ca_executor_ops *ops;
    void *impl;
};

/* --- LocalExecutor: delegates to the tool registry (tool registry ctx) --- */
struct ca_tool_registry;
struct ca_tool_ctx;
ca_executor *ca_executor_new_local(struct ca_tool_registry *reg,
                                   struct ca_tool_ctx *tctx,
                                   void *snapshot /* ca_snapshot*, may be NULL */);

/* --- Routing executors (architecture v1.0 §9 Executor family) ---
 * Wrap an inner executor and forward every action to it, rewriting `shell`
 * tool commands to run inside the target environment (WSL distro / remote
 * host over ssh, POSIX-quoted). Non-shell tools pass through unchanged.
 * The wrapper owns `inner` (destroyed with the wrapper). `distro` may be
 * NULL for the WSL default. */
ca_executor *ca_executor_new_wsl(ca_executor *inner, const char *distro);
ca_executor *ca_executor_new_remote(ca_executor *inner, const char *host);

/* Generic lifecycle over any vtable. */
ca_executor *ca_executor_new(const ca_executor_ops *ops, void *impl);
void ca_executor_free(ca_executor *e);
const char *ca_executor_name(const ca_executor *e);

/* Run one action. Returns 0 ok (*result filled, caller frees), -1 infra error. */
int ca_executor_execute(ca_executor *e, const char *tool, const char *args_json,
                        ca_executor_result **result);
int ca_executor_start(ca_executor *e);
int ca_executor_stop(ca_executor *e);
int ca_executor_snapshot(ca_executor *e, char **snapshot_id);
int ca_executor_restore(ca_executor *e, const char *snapshot_id);

#ifdef __cplusplus
}
#endif
