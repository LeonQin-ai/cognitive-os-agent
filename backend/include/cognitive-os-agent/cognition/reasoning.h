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

typedef struct reasoning reasoning;
typedef struct llm llm;
typedef struct tool_registry tool_registry;
typedef struct memory memory;
typedef struct policy_engine policy_engine;
typedef struct snapshot snapshot;
typedef struct event_bus event_bus;
typedef struct metrics metrics;
typedef struct state_machine state_machine;
typedef struct hook_registry hook_registry;

struct skill_registry;  /* skill.h */
struct ret_index;         /* retrieval/engine.h */
struct plugin_registry; /* plugin_runtime/registry.h */

typedef struct reasoning_config {
    llm *llm;                                /* required */
    tool_registry *tools;                    /* required */
    memory *memory;                          /* may be NULL */
    policy_engine *policy;                   /* may be NULL = allow all */
    snapshot *snapshot;                      /* may be NULL = no rollback */
    event_bus *bus;                          /* may be NULL */
    metrics *metrics;                        /* may be NULL */
    const char *workspace;                       /* base dir for relative tool paths */
    int use_transaction;                         /* wrap actions in a tx when snapshot present */
    struct skill_registry *skills;           /* advertised to the planner + skill tool (may be NULL) */
    struct mcp_manager *mcp;                 /* MCP connections for the mcp tool + sync (may be NULL) */
    struct ret_index *index;                  /* code index; touched files are indexed (may be NULL) */
    struct plugin_registry *plugin_registry; /* for missing-capability auto-generation */
    const char *state_root;                      /* state dir for plugin generation (may be NULL) */
    int max_rounds;                              /* agent-loop rounds per run (0 = default 8; 1 = single-shot) */
    hook_registry *hooks;                    /* horizontal hook system (may be NULL) */
    /* Context MMU budgets (chars per prompt section; 0 = default). Over budget
     * a section degrades automatically: hot drops oldest turns to one line,
     * warm sheds worklog -> errors/files, cold sheds retrieved items. */
    int budget_hot;  /* conversation history (default 8192) */
    int budget_warm; /* summary + session notes (default 3072) */
    int budget_cold; /* retrieved context + code index (default 4096) */
    int hyde;        /* 1 = HyDE retrieval: one LLM call per run
                      * generates a hypothetical answer passage
                      * used as the cold-tier query (default off) */
    /* Execution backend (non-tx actions): "local" (default) | "wsl" | "remote".
     * wsl/remote wrap the local executor and route shell commands through
     * `wsl.exe` / `ssh <exec_host>`; other tools run unchanged on the host. */
    const char *exec_backend; /* NULL = local */
    const char *exec_host;    /* ssh target for "remote" (user@host) */
} reasoning_config;

/* HyDE (Hypothetical Document Embeddings) primitive: ask the LLM for a short
 * hypothetical answer passage to `query`; embed passage-to-passage instead of
 * question-to-passage for better cold-tier recall. Returns a malloc'd passage
 * (caller frees), or NULL (bad args / LLM error). */
char *hyde_passage(llm *llm, const char *query);

reasoning *reasoning_new(const reasoning_config *cfg);
void reasoning_free(reasoning *r);

/* Run the full RECEIVE..LEARN pipeline on `prompt`.
 * *answer receives the final output (caller frees). Returns 0 ok, -1 failed. */
int reasoning_run(reasoning *r, const char *prompt, char **answer);

/* Same, bound to a named chat session: conversation history, compaction
 * summary and session notes are isolated per session_id (NULL/"" = the
 * shared default session used by reasoning_run). Sessions are created
 * on demand (capped); runs must still be serialized by the caller. */
int reasoning_run_ex(reasoning *r, const char *session_id, const char *prompt, char **answer);

/* Swap the active LLM at runtime. Caller serializes access (the ctx run-lock);
 * the old instance stays owned by the caller to destroy after the swap. */
void reasoning_set_llm(reasoning *r, llm *llm);

/* Route each run through `router` (weighted round-robin). Pass NULL to revert
 * to the single configured LLM. The router is borrowed (owned by the caller). */
void reasoning_set_router(reasoning *r, router *router);

/* Session-memory snapshot as JSON: session notes (task/state/files/errors/
 * worklog), the rolling compaction summary and history size. Caller frees. */
char *reasoning_session_json(reasoning *r);

/* Recent conversation turns as a JSON array of {"q","a"} objects, oldest
 * first (the newest max_turns turns; <=0 = default 20). Thread-safe against
 * a concurrent run. Caller frees. */
char *reasoning_history_json(reasoning *r, int max_turns);

/* Per-session variant of the above; NULL session_id = default session. */
char *reasoning_history_json_ex(reasoning *r, const char *session_id, int max_turns);

/* Chat session registry (multi-session support): list sessions as a JSON
 * array of {id, turns, task, last_active_ms}; clear one session's history
 * and notes (returns -1 if the session does not exist). Callers free the
 * JSON string. */
char *reasoning_sessions_json(reasoning *r);
int reasoning_session_clear(reasoning *r, const char *session_id);

#ifdef __cplusplus
}
#endif
