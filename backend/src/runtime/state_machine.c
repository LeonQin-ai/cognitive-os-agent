#include "cognitive-os-agent/runtime/state_machine.h"
#include "cognitive-os-agent/runtime/hook.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    coa_state_handler fn;
    void *ud;
} stage;

struct coa_state_machine {
    stage stages[COA_ST_COUNT];
    coa_state current;
    coa_hook_registry *hooks; /* optional; fires agent.on_state_change */
};

static const char *state_names[COA_ST_COUNT] = {
    "RECEIVE", "UNDERSTAND", "REASON", "PLAN", "ACT", "VERIFY", "LEARN",
    "DONE", "FAILED",
};

coa_state_machine *coa_state_machine_new(void) {
    coa_state_machine *sm = calloc(1, sizeof(coa_state_machine));
    sm->current = COA_ST_RECEIVE;
    return sm;
}

void coa_state_machine_free(coa_state_machine *sm) { free(sm); }

void coa_state_machine_set_hooks(coa_state_machine *sm, coa_hook_registry *hooks) {
    if (!sm) return;
    sm->hooks = hooks;
}

/* Fire the on_state_change hook for a stage entry (best effort, never blocks). */
static void sm_notify(coa_state_machine *sm, coa_state st) {
    if (!sm->hooks) return;
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}", coa_state_name(st));
    coa_hook_dispatch(sm->hooks, "agent.on_state_change", payload);
}

void coa_state_machine_set_handler(coa_state_machine *sm, coa_state st, coa_state_handler fn, void *ud) {
    if (st < COA_ST_RECEIVE || st >= COA_ST_DONE) return;
    sm->stages[st].fn = fn;
    sm->stages[st].ud = ud;
}

coa_state coa_state_machine_current(const coa_state_machine *sm) { return sm->current; }

/* The seven cognitive stages, in order. */
static const coa_state cognitive_order[] = {
    COA_ST_RECEIVE, COA_ST_UNDERSTAND, COA_ST_REASON, COA_ST_PLAN,
    COA_ST_ACT, COA_ST_VERIFY, COA_ST_LEARN,
};

coa_state coa_state_machine_run(coa_state_machine *sm, const char *input, char **result) {
    char *value = input ? coa_strdup(input) : coa_strdup("");
    sm->current = COA_ST_RECEIVE;

    for (size_t i = 0; i < sizeof(cognitive_order) / sizeof(coa_state); i++) {
        coa_state st = cognitive_order[i];
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
                sm->current = COA_ST_FAILED;
                return COA_ST_FAILED;
            }
            if (out) {
                free(value);
                value = out;
            }
        }
    }

    if (result) *result = value;
    else free(value);
    sm->current = COA_ST_DONE;
    return COA_ST_DONE;
}

const char *coa_state_name(coa_state st) {
    if (st < 0 || st >= COA_ST_COUNT) return "?";
    return state_names[st];
}
