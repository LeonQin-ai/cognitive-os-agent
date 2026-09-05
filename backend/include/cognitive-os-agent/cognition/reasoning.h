/* reasoning.h — cognitive reasoning engine.
 * Orchestrates the cognitive state machine (RECEIVE..LEARN) over an LLM provider,
 * executing the LLM's planned tool calls through the tool registry under the
 * transaction/snapshot layer, and recording experiences into memory.
 * The LLM is a planner (cognitive accelerator), not the control center. */
#pragma once
#include <stddef.h>
#include "cognitive-os-agent/llm/router.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_reasoning coa_reasoning;
typedef struct coa_llm coa_llm;
typedef struct coa_tool_registry coa_tool_registry;
typedef struct coa_memory coa_memory;
typedef struct coa_policy_engine coa_policy_engine;
typedef struct coa_snapshot coa_snapshot;
typedef struct coa_event_bus coa_event_bus;
typedef struct coa_metrics coa_metrics;
typedef struct coa_state_machine coa_state_machine;
typedef struct coa_hook_registry coa_hook_registry;

struct coa_skill_registry;    /* skill.h */
struct coa_index;             /* retrieval/engine.h */
struct coa_plugin_registry;   /* plugin_runtime/registry.h */

typedef struct coa_reasoning_config {
    coa_llm *llm;                 /* required */
    coa_tool_registry *tools;     /* required */
    coa_memory *memory;           /* may be NULL */
    coa_policy_engine *policy;    /* may be NULL = allow all */
    coa_snapshot *snapshot;       /* may be NULL = no rollback */
    coa_event_bus *bus;           /* may be NULL */
    coa_metrics *metrics;         /* may be NULL */
    const char *workspace;       /* base dir for relative tool paths */
    int use_transaction;         /* wrap actions in a tx when snapshot present */
    struct coa_skill_registry *skills;  /* advertised to the planner + skill tool (may be NULL) */
    struct coa_mcp_manager *mcp;        /* MCP connections for the mcp tool + sync (may be NULL) */
    struct coa_index *index;            /* code index; touched files are indexed (may be NULL) */
    struct coa_plugin_registry *plugin_registry; /* for missing-capability auto-generation */
    const char *state_root;            /* state dir for plugin generation (may be NULL) */
    int max_rounds;                    /* agent-loop rounds per run (0 = default 8; 1 = single-shot) */
    coa_hook_registry *hooks;           /* horizontal hook system (may be NULL) */
    /* Context MMU budgets (chars per prompt section; 0 = default). Over budget
     * a section degrades automatically: hot drops oldest turns to one line,
     * warm sheds worklog -> errors/files, cold sheds retrieved items. */
    int budget_hot;                    /* conversation history (default 8192) */
    int budget_warm;                   /* summary + session notes (default 3072) */
    int budget_cold;                   /* retrieved context + code index (default 4096) */
    int hyde;                          /* 1 = HyDE retrieval: one LLM call per run
                                        * generates a hypothetical answer passage
                                        * used as the cold-tier query (default off) */
    /* Execution backend (non-tx actions): "local" (default) | "wsl" | "remote".
     * wsl/remote wrap the local executor and route shell commands through
     * `wsl.exe` / `ssh <exec_host>`; other tools run unchanged on the host. */
    const char *exec_backend;          /* NULL = local */
    const char *exec_host;             /* ssh target for "remote" (user@host) */
} coa_reasoning_config;

/* HyDE (Hypothetical Document Embeddings) primitive: ask the LLM for a short
 * hypothetical answer passage to `query`; embed passage-to-passage instead of
 * question-to-passage for better cold-tier recall. Returns a malloc'd passage
 * (caller frees), or NULL (bad args / LLM error). */
char *coa_hyde_passage(coa_llm *llm, const char *query);

coa_reasoning *coa_reasoning_new(const coa_reasoning_config *cfg);
void coa_reasoning_free(coa_reasoning *r);

/* Run the full RECEIVE..LEARN pipeline on `prompt`.
 * *answer receives the final output (caller frees). Returns 0 ok, -1 failed. */
int coa_reasoning_run(coa_reasoning *r, const char *prompt, char **answer);

/* The underlying state machine (borrowed; valid until coa_reasoning_free). */
coa_state_machine *coa_reasoning_sm(coa_reasoning *r);

/* Swap the active LLM at runtime. Caller serializes access (the ctx run-lock);
 * the old instance stays owned by the caller to destroy after the swap. */
void coa_reasoning_set_llm(coa_reasoning *r, coa_llm *llm);

/* Route each run through `router` (weighted round-robin). Pass NULL to revert
 * to the single configured LLM. The router is borrowed (owned by the caller). */
void coa_reasoning_set_router(coa_reasoning *r, coa_router *router);

/* Session-memory snapshot as JSON: session notes (task/state/files/errors/
 * worklog), the rolling compaction summary and history size. Caller frees. */
char *coa_reasoning_session_json(coa_reasoning *r);

/* Recent conversation turns as a JSON array of {"q","a"} objects, oldest
 * first (the newest max_turns turns; <=0 = default 20). Thread-safe against
 * a concurrent run. Caller frees. */
char *coa_reasoning_history_json(coa_reasoning *r, int max_turns);

#ifdef __cplusplus
}
#endif
