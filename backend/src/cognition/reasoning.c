/* reasoning.c — cognitive reasoning engine.
 * Wires the state machine stages to the LLM, tool registry, transaction/snapshot
 * layer, memory and event bus. The LLM proposes a JSON plan of tool actions;
 * this engine executes them transactionally and records the episode. */
#include "cagent/cognition/reasoning.h"
#include "cagent/cognition/planner.h"
#include "cagent/cognition/evaluator.h"
#include "cagent/runtime/state_machine.h"
#include "cagent/runtime/event_bus.h"
#include "cagent/runtime/scheduler.h"
#include "cagent/llm/llm.h"
#include "cagent/action/tools.h"
#include "cagent/memory/memory.h"
#include "cagent/tx/tx.h"
#include "cagent/infra/util.h"
#include "cagent/infra/logging.h"
#include "cagent/infra/metrics.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct ca_reasoning {
    ca_llm *llm;
    ca_tool_registry *tools;
    ca_memory *mem;
    ca_policy_engine *policy;
    ca_snapshot *snap;
    ca_event_bus *bus;
    ca_metrics *metrics;
    ca_tx_manager *txm;
    ca_evaluator *eval;
    char *workspace;
    int use_transaction;

    ca_state_machine *sm;

    /* transient per-run state */
    ca_planned_action *actions;
    int n_actions;
    char *last_prompt;
    int all_actions_ok;
    int ok_actions;
};

static void clear_actions(ca_reasoning *r) {
    for (int i = 0; i < r->n_actions; i++) {
        free(r->actions[i].tool);
        free(r->actions[i].args_json);
    }
    free(r->actions);
    r->actions = NULL;
    r->n_actions = 0;
}

/* REASON: ask the LLM for a plan (JSON array of actions, or plain text). */
static int h_reason(ca_state_machine *sm, void *ud, const char *input, char **out) {
    ca_reasoning *r = ud;
    (void)sm;
    clear_actions(r);
    r->ok_actions = 0;
    if (!r->llm) { *out = ca_strdup("(no LLM provider configured)"); return 0; }
    char *raw = NULL;
    int rc = ca_planner_plan(r->llm, input, &r->actions, &r->n_actions, &raw);
    if (rc != 0 || !raw) {
        free(raw);
        ca_log_error("reasoning: LLM returned no plan");
        return -1; /* move to FAILED */
    }
    ca_log_info("reasoning: LLM plan: %s", raw);
    if (r->bus) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "plan", raw);
        ca_event_bus_publish(r->bus, CA_EV_MODEL, "reasoning", p);
    }
    *out = raw;
    return 0;
}

/* PLAN: the planner already parsed the plan in REASON; this stage passes it on. */
static int h_plan(ca_state_machine *sm, void *ud, const char *input, char **out) {
    ca_reasoning *r = ud;
    (void)sm;
    if (r->n_actions == 0)
        ca_log_info("reasoning: no tool plan, treating LLM output as answer");
    *out = ca_strdup(input);
    return 0;
}

/* ACT: execute the planned actions, wrapped in a transaction. */
static int h_act(ca_state_machine *sm, void *ud, const char *input, char **out) {
    ca_reasoning *r = ud;
    (void)sm;
    r->all_actions_ok = 1;
    ca_strbuf b;
    ca_strbuf_init(&b);

    if (r->n_actions == 0) {
        ca_strbuf_append(&b, input);
        *out = ca_strbuf_detach(&b);
        return 0;
    }

    ca_tool_ctx tctx;
    memset(&tctx, 0, sizeof(tctx));
    tctx.reg = r->tools;
    tctx.policy = r->policy;
    tctx.snapshot = r->snap;
    tctx.bus = r->bus;
    tctx.workspace = r->workspace;
    tctx.metrics = r->metrics;

    ca_tx *tx = NULL;
    if (r->use_transaction && r->snap) tx = ca_tx_begin(r->txm, r->snap, r->tools, &tctx);

    for (int i = 0; i < r->n_actions; i++) {
        ca_scheduler_yield(); /* cooperative checkpoint between tool actions */
        int rc;
        if (tx) {
            rc = ca_tx_run(tx, r->actions[i].tool, r->actions[i].args_json);
        } else {
            ca_tool_result *res = ca_tool_execute(r->tools, r->actions[i].tool,
                                                  r->actions[i].args_json, &tctx);
            rc = (res && res->ok) ? 0 : -1;
            if (res) {
                ca_strbuf_appendf(&b, "[%s] %s\n", r->actions[i].tool,
                                 res->output ? res->output : "");
                ca_tool_result_free(res);
            }
        }
        if (rc != 0) {
            r->all_actions_ok = 0;
            ca_strbuf_appendf(&b, "[%s] FAILED\n", r->actions[i].tool);
            ca_log_warn("reasoning: action '%s' failed", r->actions[i].tool);
        } else {
            r->ok_actions++;
            ca_strbuf_appendf(&b, "[%s] ok\n", r->actions[i].tool);
        }
    }

    if (tx) {
        if (r->all_actions_ok && ca_tx_validate(tx)) {
            ca_tx_commit(tx);
            ca_log_info("reasoning: transaction committed");
        } else {
            ca_tx_rollback(tx);
            ca_log_warn("reasoning: transaction rolled back");
        }
        const char *tout = ca_tx_output(tx);
        if (tout && *tout) ca_strbuf_append(&b, tout);
        ca_tx_free(tx);
    }
    if (r->metrics) ca_metrics_add(r->metrics, "actions.executed", (double)r->n_actions);

    *out = ca_strbuf_detach(&b);
    return 0;
}

/* VERIFY: any action failure fails the whole pipeline. */
static int h_verify(ca_state_machine *sm, void *ud, const char *input, char **out) {
    ca_reasoning *r = ud;
    (void)sm;
    if (!ca_evaluator_verify(r->eval, r->all_actions_ok, r->n_actions)) return -1;
    *out = ca_strdup(input);
    return 0;
}

/* LEARN: record the episode into memory. */
static int h_learn(ca_state_machine *sm, void *ud, const char *input, char **out) {
    ca_reasoning *r = ud;
    (void)sm;
    if (r->mem) {
        ca_memory_record_experience(r->mem,
                                    r->last_prompt ? r->last_prompt : "(task)", input);
        ca_memory_flush(r->mem);
    }
    if (r->metrics) {
        double q = ca_evaluator_score(r->eval, r->n_actions, r->ok_actions,
                                      r->all_actions_ok, input);
        ca_metrics_set(r->metrics, "reasoning.quality", q);
    }
    if (r->bus) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "result", input);
        ca_event_bus_publish(r->bus, CA_EV_MEMORY, "reasoning", p);
    }
    *out = ca_strdup(input);
    return 0;
}

ca_reasoning *ca_reasoning_new(const ca_reasoning_config *cfg) {
    if (!cfg || !cfg->llm || !cfg->tools) return NULL;
    ca_reasoning *r = calloc(1, sizeof(ca_reasoning));
    if (!r) return NULL;
    r->llm = cfg->llm;
    r->tools = cfg->tools;
    r->mem = cfg->memory;
    r->policy = cfg->policy;
    r->snap = cfg->snapshot;
    r->bus = cfg->bus;
    r->metrics = cfg->metrics;
    r->workspace = cfg->workspace ? ca_strdup(cfg->workspace) : NULL;
    r->use_transaction = cfg->use_transaction;
    r->txm = ca_tx_manager_new();
    r->eval = ca_evaluator_new();
    r->sm = ca_state_machine_new();

    ca_state_machine_set_handler(r->sm, CA_ST_REASON, h_reason, r);
    ca_state_machine_set_handler(r->sm, CA_ST_PLAN, h_plan, r);
    ca_state_machine_set_handler(r->sm, CA_ST_ACT, h_act, r);
    ca_state_machine_set_handler(r->sm, CA_ST_VERIFY, h_verify, r);
    ca_state_machine_set_handler(r->sm, CA_ST_LEARN, h_learn, r);
    return r;
}

void ca_reasoning_free(ca_reasoning *r) {
    if (!r) return;
    clear_actions(r);
    free(r->last_prompt);
    free(r->workspace);
    ca_state_machine_free(r->sm);
    ca_evaluator_free(r->eval);
    ca_tx_manager_free(r->txm);
    free(r);
}

ca_state_machine *ca_reasoning_sm(ca_reasoning *r) { return r ? r->sm : NULL; }

void ca_reasoning_set_llm(ca_reasoning *r, ca_llm *llm) {
    if (!r || !llm) return;
    r->llm = llm;
}

int ca_reasoning_run(ca_reasoning *r, const char *prompt, char **answer) {
    if (!r || !prompt) return -1;
    free(r->last_prompt);
    r->last_prompt = ca_strdup(prompt);
    if (r->mem) ca_memory_working_push(r->mem, prompt);

    char *result = NULL;
    ca_state st = ca_state_machine_run(r->sm, prompt, &result);
    if (r->metrics) ca_metrics_inc(r->metrics, st == CA_ST_DONE ? "tasks.done" : "tasks.failed");

    if (st != CA_ST_DONE) {
        if (answer) *answer = result ? result : ca_strdup("(pipeline failed)");
        else free(result);
        return -1;
    }
    if (answer) *answer = result;
    else free(result);
    return 0;
}
