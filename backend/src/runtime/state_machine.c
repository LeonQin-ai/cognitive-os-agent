#include "cognitive-os-agent/runtime/state_machine.h"
#include "cognitive-os-agent/runtime/hook.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    state_handler fn;
    void *ud;
} stage;

struct state_machine {
    stage stages[ST_COUNT];
    state current;
    hook_registry *hooks; /* optional; fires agent.on_state_change */
};

static const char *state_names[ST_COUNT] = {
    "RECEIVE", "UNDERSTAND", "REASON", "PLAN", "ACT", "VERIFY", "LEARN", "DONE", "FAILED",
};

state_machine *state_machine_new(void) {
    state_machine *sm = calloc(1, sizeof(state_machine));
    sm->current = ST_RECEIVE;
    return sm;
}

void state_machine_free(state_machine *sm) {
    free(sm);
}

void state_machine_set_hooks(state_machine *sm, hook_registry *hooks) {
    if (!sm)
        return;
    sm->hooks = hooks;
}

/* Fire the on_state_change hook for a stage entry (best effort, never blocks). */
static void sm_notify(state_machine *sm, state st) {
    char payload[64];

    if (!sm->hooks)
        return;
    snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}", state_name(st));
    hook_dispatch(sm->hooks, "agent.on_state_change", payload);
}

void state_machine_set_handler(state_machine *sm, state st, state_handler fn, void *ud) {
    if (st < ST_RECEIVE || st >= ST_DONE)
        return;
    sm->stages[st].fn = fn;
    sm->stages[st].ud = ud;
}

/* The seven cognitive stages, in order. */
static const state cognitive_order[] = {
    ST_RECEIVE, ST_UNDERSTAND, ST_REASON, ST_PLAN, ST_ACT, ST_VERIFY, ST_LEARN,
};

state state_machine_run(state_machine *sm, const char *input, char **result) {
    char *value = input ? xstrdup(input) : xstrdup("");
    sm->current = ST_RECEIVE;

    for (size_t i = 0; i < sizeof(cognitive_order) / sizeof(state); i++) {
        state st = cognitive_order[i];
        sm->current = st;
        sm_notify(sm, st);
        stage *sg = &sm->stages[st];
        if (sg->fn) {
            char *out = NULL;
            if (sg->fn(sm, sg->ud, value, &out) != 0) {
                free(value);
                /* surface the failed stage's diagnostic (may be NULL) */
                if (result)
                    *result = out;
                else
                    free(out);
                sm->current = ST_FAILED;
                return ST_FAILED;
            }
            if (out) {
                free(value);
                value = out;
            }
        }
    }

    if (result)
        *result = value;
    else
        free(value);
    sm->current = ST_DONE;
    return ST_DONE;
}

const char *state_name(state st) {
    if (st < 0 || st >= ST_COUNT)
        return "?";
    return state_names[st];
}
