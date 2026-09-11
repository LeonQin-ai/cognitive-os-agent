/* state_machine.h — cognitive lifecycle:
 * RECEIVE -> UNDERSTAND -> REASON -> PLAN -> ACT -> VERIFY -> LEARN -> DONE.
 * Each stage may be overridden with a handler; the default transitions run
 * in order. Handlers run synchronously within state_machine_run. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum state {
    ST_RECEIVE = 0,
    ST_UNDERSTAND,
    ST_REASON,
    ST_PLAN,
    ST_ACT,
    ST_VERIFY,
    ST_LEARN,
    ST_DONE,
    ST_FAILED,
    ST_COUNT
} state;

typedef struct state_machine state_machine;
typedef struct hook_registry hook_registry;

/* A stage handler. `input` is the current working value; returns 0 for success
 * and stores a new value into *out (caller frees). Return -1 to move to FAILED. */
typedef int (*state_handler)(state_machine *sm, void *ud, const char *input, char **out);

state_machine *state_machine_new(void);
void state_machine_free(state_machine *sm);

/* Attach the runtime hook registry: every stage entry fires the
 * "agent.on_state_change" hook with {"state": "<STAGE>"} (hook.h). Borrowed. */
void state_machine_set_hooks(state_machine *sm, hook_registry *hooks);

/* Install a handler for a stage (or NULL to use the pass-through default). */
void state_machine_set_handler(state_machine *sm, state st, state_handler fn, void *ud);

/* Run the full pipeline on `input`. Returns final state (DONE or FAILED).
 * *result receives the final working value; caller frees. */
state state_machine_run(state_machine *sm, const char *input, char **result);

const char *state_name(state st);

#ifdef __cplusplus
}
#endif
