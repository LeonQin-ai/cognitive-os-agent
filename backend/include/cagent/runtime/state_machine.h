/* state_machine.h — cognitive lifecycle:
 * RECEIVE -> UNDERSTAND -> REASON -> PLAN -> ACT -> VERIFY -> LEARN -> DONE.
 * Each stage may be overridden with a handler; the default transitions run
 * in order. Handlers run synchronously within ca_state_machine_run. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ca_state {
    CA_ST_RECEIVE = 0,
    CA_ST_UNDERSTAND,
    CA_ST_REASON,
    CA_ST_PLAN,
    CA_ST_ACT,
    CA_ST_VERIFY,
    CA_ST_LEARN,
    CA_ST_DONE,
    CA_ST_FAILED,
    CA_ST_COUNT
} ca_state;

typedef struct ca_state_machine ca_state_machine;
typedef struct ca_hook_registry ca_hook_registry;

/* A stage handler. `input` is the current working value; returns 0 for success
 * and stores a new value into *out (caller frees). Return -1 to move to FAILED. */
typedef int (*ca_state_handler)(ca_state_machine *sm, void *ud, const char *input, char **out);

ca_state_machine *ca_state_machine_new(void);
void ca_state_machine_free(ca_state_machine *sm);

/* Attach the runtime hook registry: every stage entry fires the
 * "agent.on_state_change" hook with {"state": "<STAGE>"} (hook.h). Borrowed. */
void ca_state_machine_set_hooks(ca_state_machine *sm, ca_hook_registry *hooks);

/* Install a handler for a stage (or NULL to use the pass-through default). */
void ca_state_machine_set_handler(ca_state_machine *sm, ca_state st, ca_state_handler fn, void *ud);

ca_state ca_state_machine_current(const ca_state_machine *sm);

/* Run the full pipeline on `input`. Returns final state (DONE or FAILED).
 * *result receives the final working value; caller frees. */
ca_state ca_state_machine_run(ca_state_machine *sm, const char *input, char **result);

const char *ca_state_name(ca_state st);

#ifdef __cplusplus
}
#endif
