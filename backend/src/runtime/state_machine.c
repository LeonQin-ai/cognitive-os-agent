#include "cagent/runtime/state_machine.h"
#include "cagent/runtime/hook.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    ca_state_handler fn;
    void *ud;
} stage;

struct ca_state_machine {
    stage stages[CA_ST_COUNT];
    ca_state current;
    ca_hook_registry *hooks; /* optional; fires agent.on_state_change */
};

static const char *state_names[CA_ST_COUNT] = {
    "RECEIVE", "UNDERSTAND", "REASON", "PLAN", "ACT", "VERIFY", "LEARN",
    "DONE", "FAILED",
};

ca_state_machine *ca_state_machine_new(void) {
    ca_state_machine *sm = calloc(1, sizeof(ca_state_machine));
    sm->current = CA_ST_RECEIVE;
    return sm;
}

void ca_state_machine_free(ca_state_machine *sm) { free(sm); }

void ca_state_machine_set_hooks(ca_state_machine *sm, ca_hook_registry *hooks) {
    if (!sm) return;
    sm->hooks = hooks;
}

/* Fire the on_state_change hook for a stage entry (best effort, never blocks). */
static void sm_notify(ca_state_machine *sm, ca_state st) {
    if (!sm->hooks) return;
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}", ca_state_name(st));
    ca_hook_dispatch(sm->hooks, "agent.on_state_change", payload);
}

void ca_state_machine_set_handler(ca_state_machine *sm, ca_state st, ca_state_handler fn, void *ud) {
    if (st < CA_ST_RECEIVE || st >= CA_ST_DONE) return;
    sm->stages[st].fn = fn;
    sm->stages[st].ud = ud;
}

ca_state ca_state_machine_current(const ca_state_machine *sm) { return sm->current; }

/* The seven cognitive stages, in order. */
static const ca_state cognitive_order[] = {
    CA_ST_RECEIVE, CA_ST_UNDERSTAND, CA_ST_REASON, CA_ST_PLAN,
    CA_ST_ACT, CA_ST_VERIFY, CA_ST_LEARN,
};

ca_state ca_state_machine_run(ca_state_machine *sm, const char *input, char **result) {
    char *value = input ? ca_strdup(input) : ca_strdup("");
    sm->current = CA_ST_RECEIVE;

    for (size_t i = 0; i < sizeof(cognitive_order) / sizeof(ca_state); i++) {
        ca_state st = cognitive_order[i];
        sm->current = st;
        sm_notify(sm, st);
        stage *sg = &sm->stages[st];
        if (sg->fn) {
            char *out = NULL;
            if (sg->fn(sm, sg->ud, value, &out) != 0) {
                free(value);
                /* surface the failed stage's diagnostic (may be NULL) */
                if (result) *result = out;
                else free(out);
                sm->current = CA_ST_FAILED;
                return CA_ST_FAILED;
            }
            if (out) {
                free(value);
                value = out;
            }
        }
    }

    if (result) *result = value;
    else free(value);
    sm->current = CA_ST_DONE;
    return CA_ST_DONE;
}

const char *ca_state_name(ca_state st) {
    if (st < 0 || st >= CA_ST_COUNT) return "?";
    return state_names[st];
}
