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

typedef struct coa_executor coa_executor;

/* Result of one execution. `output` is heap; free with
 * coa_executor_result_free. */
typedef struct coa_executor_result {
    int ok;
    char *output;
} coa_executor_result;

void coa_executor_result_free(coa_executor_result *r);

/* vtable — impl is the executor's private state. */
typedef struct coa_executor_ops {
    const char *name; /* "local", "sandbox", "vm", ... */
    int  (*start)(void *impl);
    /* run one action; returns 0 and fills *result (always non-NULL on rc 0),
     * nonzero on infrastructure failure (tool-level failure stays in result.ok) */
    int  (*execute)(void *impl, const char *tool, const char *args_json,
                    coa_executor_result **result);
    int  (*stop)(void *impl);
    void (*destroy)(void *impl);
    /* optional state capture for executors that own their environment;
     * return -1 when unsupported */
    int  (*snapshot)(void *impl, char **snapshot_id);
    int  (*restore)(void *impl, const char *snapshot_id);
} coa_executor_ops;

struct coa_executor {
    const coa_executor_ops *ops;
    void *impl;
};

/* --- LocalExecutor: delegates to the tool registry (tool registry ctx) --- */
struct coa_tool_registry;
struct coa_tool_ctx;
coa_executor *coa_executor_new_local(struct coa_tool_registry *reg,
                                   struct coa_tool_ctx *tctx,
                                   void *snapshot /* coa_snapshot*, may be NULL */);

/* --- Routing executors (architecture v1.0 §9 Executor family) ---
 * Wrap an inner executor and forward every action to it, rewriting `shell`
 * tool commands to run inside the target environment (WSL distro / remote
 * host over ssh, POSIX-quoted). Non-shell tools pass through unchanged.
 * The wrapper owns `inner` (destroyed with the wrapper). `distro` may be
 * NULL for the WSL default. */
coa_executor *coa_executor_new_wsl(coa_executor *inner, const char *distro);
coa_executor *coa_executor_new_remote(coa_executor *inner, const char *host);

/* Generic lifecycle over any vtable. */
coa_executor *coa_executor_new(const coa_executor_ops *ops, void *impl);
void coa_executor_free(coa_executor *e);
const char *coa_executor_name(const coa_executor *e);

/* Run one action. Returns 0 ok (*result filled, caller frees), -1 infra error. */
int coa_executor_execute(coa_executor *e, const char *tool, const char *args_json,
                        coa_executor_result **result);
int coa_executor_start(coa_executor *e);
int coa_executor_stop(coa_executor *e);
int coa_executor_snapshot(coa_executor *e, char **snapshot_id);
int coa_executor_restore(coa_executor *e, const char *snapshot_id);

#ifdef __cplusplus
}
#endif
