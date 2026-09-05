/* state_machine.h — cognitive lifecycle:
 * RECEIVE -> UNDERSTAND -> REASON -> PLAN -> ACT -> VERIFY -> LEARN -> DONE.
 * Each stage may be overridden with a handler; the default transitions run
 * in order. Handlers run synchronously within coa_state_machine_run. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum coa_state {
    COA_ST_RECEIVE = 0,
    COA_ST_UNDERSTAND,
    COA_ST_REASON,
    COA_ST_PLAN,
    COA_ST_ACT,
    COA_ST_VERIFY,
    COA_ST_LEARN,
    COA_ST_DONE,
    COA_ST_FAILED,
    COA_ST_COUNT
} coa_state;

typedef struct coa_state_machine coa_state_machine;
typedef struct coa_hook_registry coa_hook_registry;

/* A stage handler. `input` is the current working value; returns 0 for success
 * and stores a new value into *out (caller frees). Return -1 to move to FAILED. */
typedef int (*coa_state_handler)(coa_state_machine *sm, void *ud, const char *input, char **out);

coa_state_machine *coa_state_machine_new(void);
void coa_state_machine_free(coa_state_machine *sm);

/* Attach the runtime hook registry: every stage entry fires the
 * "agent.on_state_change" hook with {"state": "<STAGE>"} (hook.h). Borrowed. */
void coa_state_machine_set_hooks(coa_state_machine *sm, coa_hook_registry *hooks);

/* Install a handler for a stage (or NULL to use the pass-through default). */
void coa_state_machine_set_handler(coa_state_machine *sm, coa_state st, coa_state_handler fn, void *ud);

coa_state coa_state_machine_current(const coa_state_machine *sm);

/* Run the full pipeline on `input`. Returns final state (DONE or FAILED).
 * *result receives the final working value; caller frees. */
coa_state coa_state_machine_run(coa_state_machine *sm, const char *input, char **result);

const char *coa_state_name(coa_state st);

#ifdef __cplusplus
}
#endif
