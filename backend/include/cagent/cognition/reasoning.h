/* reasoning.h — cognitive reasoning engine.
 * Orchestrates the cognitive state machine (RECEIVE..LEARN) over an LLM provider,
 * executing the LLM's planned tool calls through the tool registry under the
 * transaction/snapshot layer, and recording experiences into memory.
 * The LLM is a planner (cognitive accelerator), not the control center. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_reasoning ca_reasoning;
typedef struct ca_llm ca_llm;
typedef struct ca_tool_registry ca_tool_registry;
typedef struct ca_memory ca_memory;
typedef struct ca_policy_engine ca_policy_engine;
typedef struct ca_snapshot ca_snapshot;
typedef struct ca_event_bus ca_event_bus;
typedef struct ca_metrics ca_metrics;
typedef struct ca_state_machine ca_state_machine;

typedef struct ca_reasoning_config {
    ca_llm *llm;                 /* required */
    ca_tool_registry *tools;     /* required */
    ca_memory *memory;           /* may be NULL */
    ca_policy_engine *policy;    /* may be NULL = allow all */
    ca_snapshot *snapshot;       /* may be NULL = no rollback */
    ca_event_bus *bus;           /* may be NULL */
    ca_metrics *metrics;         /* may be NULL */
    const char *workspace;       /* base dir for relative tool paths */
    int use_transaction;         /* wrap actions in a tx when snapshot present */
} ca_reasoning_config;

ca_reasoning *ca_reasoning_new(const ca_reasoning_config *cfg);
void ca_reasoning_free(ca_reasoning *r);

/* Run the full RECEIVE..LEARN pipeline on `prompt`.
 * *answer receives the final output (caller frees). Returns 0 ok, -1 failed. */
int ca_reasoning_run(ca_reasoning *r, const char *prompt, char **answer);

/* The underlying state machine (borrowed; valid until ca_reasoning_free). */
ca_state_machine *ca_reasoning_sm(ca_reasoning *r);

/* Swap the active LLM at runtime. Caller serializes access (the ctx run-lock);
 * the old instance stays owned by the caller to destroy after the swap. */
void ca_reasoning_set_llm(ca_reasoning *r, ca_llm *llm);

#ifdef __cplusplus
}
#endif
