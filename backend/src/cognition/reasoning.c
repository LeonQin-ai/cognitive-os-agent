/* reasoning.c — cognitive reasoning engine.
 * Wires the state machine stages to the LLM, tool registry, transaction/snapshot
 * layer, memory and event bus. The LLM proposes a JSON plan of tool actions;
 * this engine executes them transactionally and records the episode. */
#include "cognitive-os-agent/cognition/reasoning.h"
#include "cognitive-os-agent/cognition/planner.h"
#include "cognitive-os-agent/cognition/evaluator.h"
#include "cognitive-os-agent/cognition/attention.h"
#include "cognitive-os-agent/retrieval/context_builder.h"
#include "cognitive-os-agent/llm/router.h"
#include "cognitive-os-agent/runtime/state_machine.h"
#include "cognitive-os-agent/runtime/policy_engine.h"
#include "cognitive-os-agent/runtime/event_bus.h"
#include "cognitive-os-agent/runtime/hook.h"
#include "cognitive-os-agent/runtime/scheduler.h"
#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/plugin_intelligence/generator.h"
#include "cognitive-os-agent/memory/memory.h"
#include "cognitive-os-agent/retrieval/engine.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/tx/tx.h"
#include "cognitive-os-agent/execution/executor.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/infra/logging.h"
#include "cognitive-os-agent/infra/metrics.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct coa_reasoning {
    coa_llm *llm;
    coa_tool_registry *tools;
    coa_memory *mem;
    coa_policy_engine *policy;
    coa_snapshot *snap;
    coa_event_bus *bus;
    coa_metrics *metrics;
    coa_tx_manager *txm;
    coa_evaluator *eval;
    char *workspace;
    int use_transaction;

    /* Context MMU budgets (chars per prompt section, auto-degrading) */
    int budget_hot, budget_warm, budget_cold;
    int hyde;
    const char *exec_backend; /* "local" | "wsl" | "remote" (NULL = local) */
    const char *exec_host;    /* ssh target for "remote" */

    coa_router *router;       /* optional multi-provider routing (NULL = single LLM) */
    coa_attention *attention; /* salience ranking over retrieved context */

    /* multi-turn conversation history (bounded ring of recent turns).
     * hist_mtx guards all hist_* accesses: the reasoning run itself is
     * serialized by the ctx run-lock, but /v1/chat/history reads the ring
     * from the HTTP thread while a run may be in flight. */
    coa_mutex hist_mtx;
    char **hist_q;
    char **hist_a;
    size_t hist_n, hist_cap;

    /* LLM-compacted summary of turns dropped from the ring (NULL = none) */
    char *summary;
    int compact_fails;    /* consecutive compaction LLM failures */
    int compact_disabled; /* circuit breaker: stop trying after 3 failures */

    /* session notes: fixed-section working notes injected into every prompt
     * (Claude Code-style SessionMemory template, updated each run) */
    char sn_state[256];    /* current state / progress */
    char sn_task[256];     /* current task */
    char sn_files[256];    /* files touched this session */
    char sn_errors[256];   /* recent errors */
    char sn_worklog[1024]; /* append-only per-action log (tail kept) */

    /* code index: touched files are indexed for term -> file:line recall */
    struct coa_index *index;

    /* missing-capability auto-generation (self-evolution loop) */
    struct coa_plugin_registry *plugin_registry;
    char *state_root;
    int gen_attempted;     /* one auto-generation attempt per run */

    coa_state_machine *sm;
    coa_hook_registry *hooks; /* horizontal hook system (borrowed, may be NULL) */

    /* transient per-run state */
    coa_planned_action *actions;
    int n_actions;
    char *last_prompt;
    int all_actions_ok;
    int ok_actions;
    int denied_actions;  /* blocked by policy this run (not a failure) */
    struct coa_skill_registry *skills;  /* advertised to planner + skill tool */
    struct coa_mcp_manager *mcp;        /* handed to tool ctx for mcp tool calls */

    /* agent loop: bounded plan->act->observe->replan rounds per run. Results of
     * executed rounds are fed back into the next round's planner context; the
     * loop ends when the LLM stops proposing actions (final text answer). */
    int max_rounds;          /* from config (default AGENT_LOOP_MAX_ROUNDS) */
    int round_idx;           /* 1-based round currently executing */
    int had_plan;            /* last REASON produced tool actions (vs final text) */
    char *last_plan_raw;     /* this round's raw plan (stall detection) */
    char *prev_plan;         /* previous round's raw plan (stall detection) */
    char *round_log;         /* accumulated action results of previous rounds */
    size_t round_log_len, round_log_cap;
};

static void clear_actions(coa_reasoning *r) {
    for (int i = 0; i < r->n_actions; i++) {
        free(r->actions[i].tool);
        free(r->actions[i].args_json);
    }
    free(r->actions);
    r->actions = NULL;
    r->n_actions = 0;
}

/* Copy at most `cap` bytes of s, cutting back to a UTF-8 boundary and adding
 * an ellipsis when truncated. Caller frees. NULL only on OOM. */
#define HIST_TURN_CAP 500     /* per-turn chars kept in history */
#define HIST_BUDGET 8192      /* total chars of history injected per run */
#define LEARN_RESULT_CAP 300  /* chars of a result kept as a memory episode */
#define COMPACT_SUMMARY_CAP 2000 /* rolling compaction summary cap */
#define AGENT_LOOP_MAX_ROUNDS 32 /* default rounds when config does not set it;
                                    config "reasoning.max_rounds" < 0 = unlimited */
#define ROUND_LOG_CAP 16384       /* tail-keep cap for accumulated round results */

/* Append text to the round log, tail-keeping: once past ROUND_LOG_CAP the
 * oldest half is dropped so recent action results always stay available. */
static void round_log_append(coa_reasoning *r, const char *text) {
    if (!text || !*text) return;
    size_t add = strlen(text);
    size_t need = r->round_log_len + add + 1;
    if (need > r->round_log_cap) {
        size_t ncap = r->round_log_cap ? r->round_log_cap * 2 : 2048;
        while (ncap < need) ncap *= 2;
        char *nb = (char *)realloc(r->round_log, ncap);
        if (!nb) return;
        r->round_log = nb;
        r->round_log_cap = ncap;
    }
    memcpy(r->round_log + r->round_log_len, text, add + 1);
    r->round_log_len += add;
    if (r->round_log_len > ROUND_LOG_CAP) {
        size_t half = r->round_log_len / 2;
        memmove(r->round_log, r->round_log + half, r->round_log_len - half + 1);
        r->round_log_len -= half;
    }
}

static void round_log_reset(coa_reasoning *r) {
    if (r->round_log) r->round_log[0] = '\0';
    r->round_log_len = 0;
}

/* session notes: append "line\n" to a fixed-size buffer, keeping the TAIL
 * (oldest lines are dropped from the front when the cap would be exceeded). */
static void sn_append_line(char *dst, size_t cap, const char *line) {
    if (!dst || !line || !*line) return;
    size_t cur = strlen(dst);
    size_t add = strlen(line) + 1; /* line chars + '\n' */
    while (cur + add + 1 > cap) {
        char *nl = strchr(dst, '\n');
        if (!nl) { dst[0] = '\0'; cur = 0; break; }
        size_t cut = (size_t)(nl - dst) + 1;
        memmove(dst, dst + cut, cur - cut + 1);
        cur -= cut;
    }
    size_t room = cap - 1 - cur;
    if (room < 2) return;
    if (add > room) add = room;
    size_t keep = add - 1;
    while (keep > 0 && ((unsigned char)line[keep] & 0xC0) == 0x80) keep--; /* utf-8 boundary */
    memcpy(dst + cur, line, keep);
    dst[cur + keep] = '\n';
    dst[cur + keep + 1] = '\0';
}

/* Track a file touched by a file_* action (extract "path" from its args). */
static void sn_note_file(coa_reasoning *r, const char *args_json) {
    if (!args_json || !*args_json) return;
    cJSON *o = cJSON_Parse(args_json);
    if (!o) return;
    cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "path");
    if (p && cJSON_IsString(p) && p->valuestring)
        sn_append_line(r->sn_files, sizeof(r->sn_files), p->valuestring);
    cJSON_Delete(o);
}

static char *str_head(const char *s, size_t cap) {
    if (!s) return NULL;
    size_t n = strlen(s);
    int trunc = n > cap;
    if (trunc) n = cap;
    while (trunc && n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--; /* utf-8 boundary */
    char *out = (char *)malloc(n + 4);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    if (trunc) memcpy(out + n, "…", 4);
    return out;
}

/* Build an augmented prompt for the planner: recent multi-turn history, then
 * retrieved RAG context (ranked by attention), then the current request.
 * Caller frees the returned string. */
static char *build_context(coa_reasoning *r, const char *prompt) {
    coa_strbuf b;
    coa_strbuf_init(&b);

    /* WARM tier: compaction summary + session notes under an explicit budget.
     * Over budget the lowest-value sections shed first:
     * worklog -> errors/files -> task/state only. */
    size_t warm_used = 0;
    if (r->summary && *r->summary) {
        coa_strbuf_append(&b, "## Earlier conversation summary\n");
        coa_strbuf_append(&b, r->summary);
        coa_strbuf_append(&b, "\n\n");
        warm_used += strlen(r->summary) + 34;
    }
    if (r->sn_task[0] || r->sn_state[0] || r->sn_files[0] ||
        r->sn_errors[0] || r->sn_worklog[0]) {
        size_t budget_left = (warm_used < (size_t)r->budget_warm)
            ? (size_t)r->budget_warm - warm_used : 0;
        for (int lv = 0; lv < 3; lv++) { /* 0=full 1=no worklog 2=minimal */
            coa_strbuf nb;
            coa_strbuf_init(&nb);
            coa_strbuf_append(&nb, "## Session notes\n");
            if (r->sn_task[0]) coa_strbuf_appendf(&nb, "- 任务: %s\n", r->sn_task);
            if (r->sn_state[0]) coa_strbuf_appendf(&nb, "- 状态: %s\n", r->sn_state);
            if (lv < 2 && r->sn_files[0])
                coa_strbuf_appendf(&nb, "- 本会话涉及文件:\n%s", r->sn_files);
            if (lv < 2 && r->sn_errors[0])
                coa_strbuf_appendf(&nb, "- 近期错误:\n%s", r->sn_errors);
            if (lv < 1 && r->sn_worklog[0])
                coa_strbuf_appendf(&nb, "- 工作日志:\n%s", r->sn_worklog);
            coa_strbuf_append(&nb, "\n");
            if (nb.len <= budget_left || lv == 2) {
                warm_used += nb.len;
                coa_strbuf_append(&b, nb.buf ? nb.buf : "");
                coa_strbuf_free(&nb);
                break;
            }
            coa_strbuf_free(&nb);
        }
    }

    /* HOT tier: multi-turn history (bounded, most-recent-last). Newest turns
     * are kept whole; older turns beyond the hot budget degrade to one line. */
    size_t hot_mark = b.len;
    coa_mutex_lock(&r->hist_mtx);
    if (r->hist_n > 0) {
        coa_strbuf_append(&b, "## Conversation history\n");
        size_t start = r->hist_n > 6 ? r->hist_n - 6 : 0;
        /* walk newest->oldest; the first turn that pushes the running total
         * over budget (and everything before it) is degraded to one line */
        size_t keep_from = start;
        size_t total = 0;
        int over = 0;
        for (size_t i = r->hist_n; i-- > start; ) {
            size_t cost = (r->hist_q[i] ? strlen(r->hist_q[i]) : 0) +
                          (r->hist_a[i] ? strlen(r->hist_a[i]) : 0) + 24;
            total += cost;
            if (total > (size_t)r->budget_hot) { keep_from = i + 1; over = 1; break; }
        }
        for (size_t i = start; i < r->hist_n; i++) {
            if (over && i < keep_from) {
                char *qh = str_head(r->hist_q[i], 120);
                coa_strbuf_appendf(&b, "User: %s → Assistant: [earlier turn omitted]\n",
                                  qh ? qh : "");
                free(qh);
                continue;
            }
            if (r->hist_q[i]) coa_strbuf_appendf(&b, "User: %s\n", r->hist_q[i]);
            if (r->hist_a[i]) coa_strbuf_appendf(&b, "Assistant: %s\n", r->hist_a[i]);
        }
        coa_strbuf_append(&b, "\n");
    }
    coa_mutex_unlock(&r->hist_mtx);

    /* COLD tier: retrieved long-term knowledge + code index, under budget.
     * Degradation: fewer attention-selected items, then hard char cut.
     * HyDE (optional): one LLM call rewrites the request as a hypothetical
     * answer passage; passage-to-passage similarity beats question-to-passage
     * for recall. */
    size_t cold_mark = b.len;
    if (r->mem && r->attention) {
        char hyde_query_buf[1024];
        const char *retrieval_query = prompt;
        if (r->hyde && r->llm) {
            char *passage = coa_hyde_passage(r->llm, prompt);
            if (passage) {
                snprintf(hyde_query_buf, sizeof(hyde_query_buf), "%.1000s", passage);
                free(passage);
                retrieval_query = hyde_query_buf;
            }
        }
        char *ctx_json = coa_context_build(r->mem, retrieval_query, 12);
        if (ctx_json) {
            cJSON *arr = cJSON_Parse(ctx_json);
            if (arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
                int n = cJSON_GetArraySize(arr);
                coa_attention_candidate *cands = calloc((size_t)n, sizeof(*cands));
                for (int i = 0; i < n; i++) {
                    cJSON *it = cJSON_GetArrayItem(arr, i);
                    cJSON *t = cJSON_GetObjectItem(it, "text");
                    cJSON *res = cJSON_GetObjectItem(it, "result");
                    const char *txt = (t && t->valuestring) ? t->valuestring
                        : (res && res->valuestring) ? res->valuestring : "";
                    cands[i].text = txt;
                    cands[i].tags = "";
                    cands[i].boost = 0.0;
                }
                coa_attention_result ress[6];
                int kmax = r->budget_cold >= 2400 ? 6
                         : (r->budget_cold >= 1000 ? 3 : 1);
                int k = coa_attention_select(r->attention, prompt, cands, (size_t)n,
                                            ress, kmax);
                char *rendered = NULL;
                if (k > 0) {
                    cJSON *sel = cJSON_CreateArray();
                    for (int i = 0; i < k; i++) {
                        cJSON *it = cJSON_GetArrayItem(arr, ress[i].index);
                        if (it) cJSON_AddItemToArray(sel, cJSON_Duplicate(it, 1));
                    }
                    char *sel_json = cJSON_PrintUnformatted(sel);
                    rendered = coa_context_render_text(sel_json);
                    free(sel_json);
                    cJSON_Delete(sel);
                } else {
                    rendered = coa_context_render_text(ctx_json);
                }
                free(cands);
                if (rendered) {
                    coa_strbuf_append(&b, "## Retrieved context\n");
                    if (strlen(rendered) > (size_t)r->budget_cold) {
                        char *cut = str_head(rendered, (size_t)r->budget_cold);
                        coa_strbuf_append(&b, cut ? cut : "");
                        free(cut);
                        coa_strbuf_append(&b, "\n(超出 cold 预算，已截断)\n\n");
                    } else {
                        coa_strbuf_append(&b, rendered);
                        coa_strbuf_append(&b, "\n\n");
                    }
                    free(rendered);
                }
            }
            if (arr) cJSON_Delete(arr);
            free(ctx_json);
        }
    }

    /* code index: where the request's terms live in workspace files */
    if (r->index) {
        char *hits = coa_index_search(r->index, prompt, 5);
        if (hits && strcmp(hits, "[]") != 0) {
            cJSON *hroot = cJSON_Parse(hits);
            if (hroot && cJSON_IsArray(hroot)) {
                coa_strbuf_append(&b, "## Code index\n");
                cJSON *it;
                cJSON_ArrayForEach(it, hroot) {
                    cJSON *f = cJSON_GetObjectItemCaseSensitive(it, "file");
                    cJSON *l = cJSON_GetObjectItemCaseSensitive(it, "line");
                    cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "term");
                    if (f && cJSON_IsString(f))
                        coa_strbuf_appendf(&b, "- %s:%d (term: %s)\n",
                                          f->valuestring,
                                          l && cJSON_IsNumber(l) ? (int)l->valuedouble : 0,
                                          t && cJSON_IsString(t) ? t->valuestring : "");
                }
                coa_strbuf_append(&b, "\n");
            }
            if (hroot) cJSON_Delete(hroot);
        }
        free(hits);
    }

    /* agent-loop feedback: results of the rounds executed so far in this run,
     * so the planner can decide "next actions" vs "task complete" */
    if (r->round_log_len > 0) {
        coa_strbuf_appendf(&b, "## 之前轮次的动作结果 (第 %d/%d 轮)\n", r->round_idx - 1, r->max_rounds);
        coa_strbuf_append(&b, r->round_log);
        if (r->round_idx >= r->max_rounds)
            coa_strbuf_append(&b,
                "\n这是最后一轮。不要再调用任何工具，也不要输出 JSON 动作；"
                "直接基于以上动作结果用纯文本给出最终答案（说明完成了什么、"
                "或还缺什么信息）。\n\n");
        else
            coa_strbuf_append(&b,
                "\n你是多轮 agent 循环：根据以上动作结果，(a) 任务已完成 → 直接用纯文本回答；"
                "(b) 未完成 → 给出下一批 JSON 动作。不要重复已成功的动作。\n\n");
    }

    /* anti-pollution caution: history/notes/retrieved context are reference
     * only — the current request must always be planned and executed fresh */
    coa_strbuf_append(&b,
        "## 重要约束\n"
        "- 上面的会话摘要、Session notes、Conversation history、Retrieved context 都只是背景参考，"
        "不代表当前请求已经完成。\n"
        "- 即使历史记录里出现过相似的任务，也必须针对「Current request」重新规划并实际执行动作，"
        "不允许凭历史记录直接回答\"已完成\"。\n"
        "- 回答中声称对文件做过任何改动，必须以本轮实际出现的 [tool] 动作结果为依据；"
        "没有实际执行过对应动作，就不得声称做过。\n\n");

    /* Context MMU accounting: per-tier bytes of this prompt */
    if (r->metrics) {
        coa_metrics_set(r->metrics, "context.bytes_hot", (double)(b.len - hot_mark));
        coa_metrics_set(r->metrics, "context.bytes_warm", (double)warm_used);
        coa_metrics_set(r->metrics, "context.bytes_cold", (double)(b.len - cold_mark));
    }

    coa_strbuf_append(&b, "## Current request\n");
    coa_strbuf_append(&b, prompt);
    return coa_strbuf_detach(&b);
}

/* REASON: ask the LLM for a plan (JSON array of actions, or plain text). */
static int h_reason(coa_state_machine *sm, void *ud, const char *input, char **out) {
    coa_reasoning *r = ud;
    (void)sm;
    clear_actions(r);
    r->ok_actions = 0;
    r->denied_actions = 0;
    char *aug = build_context(r, input);
    if (!r->llm) { free(aug); *out = coa_strdup("(no LLM provider configured)"); return 0; }
    char *raw = NULL;
    char *plan_err = NULL;
    int rc = coa_planner_plan_ex(r->llm, r->tools, r->skills, r->policy,
                                aug ? aug : input,
                                &r->actions, &r->n_actions, &raw, &plan_err);
    free(aug);
    if (rc != 0 || !raw) {
        free(raw);
        char *msg = plan_err
            ? coa_strdup(plan_err)
            : coa_strdup("(LLM 调用失败：请先用「测试」按钮验证模型配置 provider/key/base_url/model)");
        free(plan_err);
        coa_log_error("reasoning: LLM returned no plan: %s", msg);
        *out = msg;           /* surfaced as the task result on FAILED */
        return -1; /* move to FAILED */
    }
    coa_log_info("reasoning: LLM plan: %s", raw);
    r->had_plan = r->n_actions > 0;
    /* remember this round's plan for stall detection (identical plan twice in
     * a row means the loop is not making progress); copy — raw flows out */
    free(r->prev_plan);
    r->prev_plan = r->last_plan_raw;
    r->last_plan_raw = coa_strdup(raw);
    if (r->bus) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "plan", raw);
        coa_event_bus_publish(r->bus, COA_EV_MODEL, "reasoning", p);
    }
    *out = raw;
    return 0;
}

/* PLAN: the planner already parsed the plan in REASON; this stage passes it on. */
static int h_plan(coa_state_machine *sm, void *ud, const char *input, char **out) {
    coa_reasoning *r = ud;
    (void)sm;
    if (r->n_actions == 0)
        coa_log_info("reasoning: no tool plan, treating LLM output as answer");
    *out = coa_strdup(input);
    return 0;
}

/* ACT: execute the planned actions, wrapped in a transaction. */
static int h_act(coa_state_machine *sm, void *ud, const char *input, char **out) {
    coa_reasoning *r = ud;
    (void)sm;
    r->all_actions_ok = 1;
    coa_strbuf b;
    coa_strbuf_init(&b);

    if (r->n_actions == 0) {
        coa_strbuf_append(&b, input);
        *out = coa_strbuf_detach(&b);
        return 0;
    }

    coa_tool_ctx tctx;
    memset(&tctx, 0, sizeof(tctx));
    tctx.reg = r->tools;
    tctx.policy = r->policy;
    tctx.snapshot = r->snap;
    tctx.bus = r->bus;
    tctx.workspace = r->workspace;
    tctx.metrics = r->metrics;
    tctx.skills = r->skills;
    tctx.mcp = r->mcp;

    coa_tx *tx = NULL;
    if (r->use_transaction && r->snap) tx = coa_tx_begin(r->txm, r->snap, r->tools, &tctx);

    /* Execution Runtime: all non-tx actions run behind the executor interface.
     * exec_backend routes shell commands through WSL / ssh by wrapping the
     * local executor (non-shell tools pass through unchanged). */
    coa_executor *exec = tx ? NULL : coa_executor_new_local(r->tools, &tctx, r->snap);
    if (exec && r->exec_backend && strcmp(r->exec_backend, "wsl") == 0) {
        coa_executor *w = coa_executor_new_wsl(exec, r->exec_host);
        if (w) exec = w;
    } else if (exec && r->exec_backend && strcmp(r->exec_backend, "remote") == 0 &&
               r->exec_host && *r->exec_host) {
        coa_executor *w = coa_executor_new_remote(exec, r->exec_host);
        if (w) exec = w;
    }

    for (int i = 0; i < r->n_actions; i++) {
        coa_scheduler_yield(); /* cooperative checkpoint between tool actions */
        /* policy hard-block: a denied action is intentionally NOT executed.
         * Record the refusal and keep going — a policy refusal is a legitimate
         * outcome, not an infrastructure failure, so it must not fail the
         * pipeline (the planner sees the refusal in the next round). */
        if (r->policy) {
            const char *preason = NULL;
            if (coa_policy_check(r->policy, r->actions[i].tool,
                                r->actions[i].args_json, &preason) == COA_POLICY_DENY) {
                r->denied_actions++;
                coa_strbuf_appendf(&b, "[%s] denied by policy (%s)\n",
                                  r->actions[i].tool, preason ? preason : "rule");
                char el[128];
                snprintf(el, sizeof(el), "%s 被策略拒绝", r->actions[i].tool);
                sn_append_line(r->sn_errors, sizeof(r->sn_errors), el);
                snprintf(el, sizeof(el), "[%s] DENIED", r->actions[i].tool);
                sn_append_line(r->sn_worklog, sizeof(r->sn_worklog), el);
                if (r->metrics) coa_metrics_inc(r->metrics, "tools.denied");
                continue;
            }
        }
        /* missing-capability self-evolution: the planner referenced a tool
         * that does not exist — try generating a plugin for it (once per run)
         * and bind the generated skill under the planned tool name. */
        if (!r->gen_attempted && r->plugin_registry && r->llm && r->skills &&
            !coa_tool_find(r->tools, r->actions[i].tool)) {
            r->gen_attempted = 1;
            char *desc = str_head(r->last_prompt ? r->last_prompt : r->actions[i].tool, 300);
            coa_plugin_gen_deps gd;
            memset(&gd, 0, sizeof(gd));
            gd.llm = r->llm;
            gd.registry = r->plugin_registry;
            gd.skills = r->skills;
            gd.state_root = r->state_root;
            char *gjson = coa_plugin_generate_deps(&gd, desc ? desc : r->actions[i].tool);
            free(desc);
            if (gjson) {
                cJSON *g = cJSON_Parse(gjson);
                cJSON *okj = g ? cJSON_GetObjectItemCaseSensitive(g, "ok") : NULL;
                cJSON *pj = g ? cJSON_GetObjectItemCaseSensitive(g, "plugin") : NULL;
                cJSON *nj = pj ? cJSON_GetObjectItemCaseSensitive(pj, "name") : NULL;
                if (okj && cJSON_IsTrue(okj) && nj && cJSON_IsString(nj) &&
                    coa_tool_register_generated(r->tools, r->skills,
                                               r->actions[i].tool, nj->valuestring) == 0) {
                    coa_log_info("reasoning: auto-generated plugin '%s' bound as tool '%s'",
                                nj->valuestring, r->actions[i].tool);
                    /* persist the tool -> skill binding so the capability
                     * re-binds at startup instead of regenerating */
                    if (r->state_root)
                        coa_tool_generated_save_mapping(r->state_root,
                                                       r->actions[i].tool,
                                                       nj->valuestring);
                }
                if (g) cJSON_Delete(g);
                free(gjson);
            }
        }
        /* execution hooks (horizontal): before_execute may block the action
         * the same way policy does — recorded, not fatal. */
        if (r->hooks) {
            char *hp = NULL;
            cJSON *wrap = cJSON_CreateObject();
            if (wrap) {
                cJSON_AddStringToObject(wrap, "tool", r->actions[i].tool);
                cJSON *ho = r->actions[i].args_json
                    ? cJSON_Parse(r->actions[i].args_json) : NULL;
                if (ho) cJSON_AddItemToObject(wrap, "args", ho);
                hp = cJSON_PrintUnformatted(wrap);
                cJSON_Delete(wrap);
            }
            if (coa_hook_dispatch(r->hooks, "exec.before_execute", hp) == 1) {
                free(hp);
                r->denied_actions++;
                coa_strbuf_appendf(&b, "[%s] blocked by hook\n", r->actions[i].tool);
                char el[128];
                snprintf(el, sizeof(el), "%s 被 hook 拦截", r->actions[i].tool);
                sn_append_line(r->sn_errors, sizeof(r->sn_errors), el);
                if (r->metrics) coa_metrics_inc(r->metrics, "tools.hook_blocked");
                continue;
            }
            free(hp);
        }
        int rc;
        if (tx) {
            rc = coa_tx_run(tx, r->actions[i].tool, r->actions[i].args_json);
        } else if (exec) {
            coa_executor_result *er = NULL;
            int erc = coa_executor_execute(exec, r->actions[i].tool,
                                          r->actions[i].args_json, &er);
            rc = (erc == 0 && er && er->ok) ? 0 : -1;
            if (er) {
                /* executor output is already UTF-8 sanitized */
                coa_strbuf_appendf(&b, "[%s] %s\n", r->actions[i].tool,
                                 er->output ? er->output : "");
                coa_executor_result_free(er);
            }
        } else {
            rc = -1;
        }
        if (rc != 0) {
            r->all_actions_ok = 0;
            coa_strbuf_appendf(&b, "[%s] FAILED\n", r->actions[i].tool);
            coa_log_warn("reasoning: action '%s' failed", r->actions[i].tool);
            if (r->hooks)
                coa_hook_dispatch(r->hooks, "exec.on_failure", r->actions[i].tool);
        } else {
            r->ok_actions++;
            coa_strbuf_appendf(&b, "[%s] ok\n", r->actions[i].tool);
            if (r->hooks)
                coa_hook_dispatch(r->hooks, "exec.after_execute", r->actions[i].tool);
        }
        /* session notes: files touched / errors / per-action worklog */
        if (strncmp(r->actions[i].tool, "file_", 5) == 0) {
            sn_note_file(r, r->actions[i].args_json);
            /* keep the code index current with files the session touches */
            if (rc == 0 && r->index && r->actions[i].args_json) {
                cJSON *ao = cJSON_Parse(r->actions[i].args_json);
                cJSON *pj = ao ? cJSON_GetObjectItemCaseSensitive(ao, "path") : NULL;
                if (pj && cJSON_IsString(pj) && pj->valuestring) {
                    char full[2048];
                    coa_path_resolve(full, sizeof(full), r->workspace, pj->valuestring);
                    char *content = coa_fs_read_file(full);
                    if (content) {
                        coa_index_add_file(r->index, full, content);
                        free(content);
                    }
                }
                if (ao) cJSON_Delete(ao);
            }
        }
        if (rc != 0) {
            char el[96];
            snprintf(el, sizeof(el), "%s 执行失败", r->actions[i].tool);
            sn_append_line(r->sn_errors, sizeof(r->sn_errors), el);
        }
        char wl[128];
        snprintf(wl, sizeof(wl), "[%s] %s", r->actions[i].tool,
                 rc == 0 ? "ok" : "FAILED");
        sn_append_line(r->sn_worklog, sizeof(r->sn_worklog), wl);
    }
    if (r->n_actions > 0) {
        snprintf(r->sn_state, sizeof(r->sn_state), "%d/%d 个动作已执行%s",
                 r->ok_actions, r->n_actions,
                 r->all_actions_ok ? "" : "，部分失败");
    }
    coa_executor_free(exec);

    if (tx) {
        if (r->all_actions_ok && coa_tx_validate(tx)) {
            coa_tx_commit(tx);
            coa_log_info("reasoning: transaction committed");
        } else {
            coa_tx_rollback(tx);
            coa_log_warn("reasoning: transaction rolled back");
        }
        const char *tout = coa_tx_output(tx);
        if (tout && *tout) {
            char *safe = coa_str_utf8_sanitize(tout);
            coa_strbuf_append(&b, safe ? safe : tout);
            free(safe);
        }
        coa_tx_free(tx);
    }
    if (r->metrics) coa_metrics_add(r->metrics, "actions.executed", (double)r->n_actions);

    *out = coa_strbuf_detach(&b);
    return 0;
}

/* VERIFY: any action failure fails the whole pipeline — but the surfaced
 * result must carry the real per-action outputs (which tool failed and why),
 * not an opaque message. Policy-denied actions are NOT failures: a plan whose
 * actions were all legitimately refused is a correct outcome (the run
 * completes with the refusal text). `input` is the ACT stage's report. */
static int h_verify(coa_state_machine *sm, void *ud, const char *input, char **out) {
    coa_reasoning *r = ud;
    (void)sm;
    int eff_total = r->n_actions - r->denied_actions;
    if (!coa_evaluator_verify(r->eval, r->all_actions_ok, eff_total, r->ok_actions)) {
        coa_strbuf b;
        coa_strbuf_init(&b);
        coa_strbuf_appendf(&b, "%d/%d 个动作执行失败，各动作结果：\n",
                          eff_total - r->ok_actions, eff_total);
        coa_strbuf_append(&b, (input && *input) ? input : "(无工具输出)");
        *out = coa_strbuf_detach(&b);
        return -1;
    }
    *out = coa_strdup(input);
    return 0;
}

/* LEARN: record the episode into memory. The raw result (which can embed
 * kilobytes of tool output) is distilled to its first ~300 chars first —
 * storing everything verbatim permanently bloats retrieval. */
static int h_learn(coa_state_machine *sm, void *ud, const char *input, char **out) {
    coa_reasoning *r = ud;
    (void)sm;
    /* session notes: current task + end-of-run state */
    if (r->last_prompt && *r->last_prompt) {
        char *t = str_head(r->last_prompt, 200);
        if (t) { snprintf(r->sn_task, sizeof(r->sn_task), "%s", t); free(t); }
    }
    snprintf(r->sn_state, sizeof(r->sn_state), "%s",
             r->all_actions_ok ? "上一任务已完成" : "上一任务部分失败");
    if (r->mem) {
        /* episode only on the final round: intermediate rounds would record
         * raw tool output into episodic memory; KG edges stay per-round */
        if (!r->had_plan || r->round_idx >= r->max_rounds) {
            char *shaped = str_head(input, LEARN_RESULT_CAP);
            coa_memory_record_experience(r->mem,
                                        r->last_prompt ? r->last_prompt : "(task)",
                                        shaped ? shaped : input);
            free(shaped);
            /* consolidation automation: threshold+interval gated pass over the
             * episodes (semantic themes + procedural tool facts) */
            if (coa_memory_maybe_consolidate(r->mem, 10, 60000) == 1 && r->metrics)
                coa_metrics_inc(r->metrics, "memory.consolidations");
        }
        /* knowledge-graph edges: task -used-> tool -touched-> file */
        if (r->last_prompt && r->n_actions > 0) {
            char *th = str_head(r->last_prompt, 80);
            for (int i = 0; i < r->n_actions; i++) {
                coa_memory_record_edge(r->mem, th ? th : "(task)",
                                      r->actions[i].tool, "used_tool");
                if (strncmp(r->actions[i].tool, "file_", 5) == 0 &&
                    r->actions[i].args_json) {
                    cJSON *ao = cJSON_Parse(r->actions[i].args_json);
                    cJSON *pj = ao ? cJSON_GetObjectItemCaseSensitive(ao, "path") : NULL;
                    if (pj && cJSON_IsString(pj) && pj->valuestring)
                        coa_memory_record_edge(r->mem, r->actions[i].tool,
                                              pj->valuestring, "touched");
                    cJSON_Delete(ao);
                }
            }
            free(th);
        }
        coa_memory_flush(r->mem);
    }
    if (r->metrics) {
        double q = coa_evaluator_score(r->eval, r->n_actions, r->ok_actions,
                                      r->all_actions_ok, input);
        coa_metrics_set(r->metrics, "reasoning.quality", q);
    }
    if (r->bus) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "result", input);
        coa_event_bus_publish(r->bus, COA_EV_MEMORY, "reasoning", p);
    }
    *out = coa_strdup(input);
    return 0;
}

coa_reasoning *coa_reasoning_new(const coa_reasoning_config *cfg) {
    if (!cfg || !cfg->llm || !cfg->tools) return NULL;
    coa_reasoning *r = calloc(1, sizeof(coa_reasoning));
    if (!r) return NULL;
    coa_mutex_init(&r->hist_mtx);
    r->llm = cfg->llm;
    r->tools = cfg->tools;
    r->mem = cfg->memory;
    r->policy = cfg->policy;
    r->snap = cfg->snapshot;
    r->bus = cfg->bus;
    r->metrics = cfg->metrics;
    r->workspace = cfg->workspace ? coa_strdup(cfg->workspace) : NULL;
    r->use_transaction = cfg->use_transaction;
    r->budget_hot   = cfg->budget_hot   > 0 ? cfg->budget_hot   : HIST_BUDGET;
    r->budget_warm  = cfg->budget_warm  > 0 ? cfg->budget_warm  : 3072;
    r->budget_cold  = cfg->budget_cold  > 0 ? cfg->budget_cold  : 4096;
    r->hyde = cfg->hyde ? 1 : 0;
    r->exec_backend = (cfg->exec_backend && *cfg->exec_backend) ? cfg->exec_backend : NULL;
    r->exec_host = (cfg->exec_host && *cfg->exec_host) ? cfg->exec_host : NULL;
    r->skills = cfg->skills;
    r->mcp = cfg->mcp;
    r->index = cfg->index;
    r->plugin_registry = cfg->plugin_registry;
    r->state_root = cfg->state_root ? coa_strdup(cfg->state_root) : NULL;
    /* 0 = use default; negative = unlimited (loop guards on round_idx only
     * hitting INT_MAX, so clamp to a practical upper bound) */
    r->max_rounds = cfg->max_rounds != 0 ? cfg->max_rounds : AGENT_LOOP_MAX_ROUNDS;
    if (r->max_rounds < 0) r->max_rounds = 1000000;
    r->hooks = cfg->hooks;
    if (r->hooks) coa_state_machine_set_hooks(r->sm, r->hooks);
    r->txm = coa_tx_manager_new();
    r->eval = coa_evaluator_new();
    r->attention = coa_attention_new();
    r->sm = coa_state_machine_new();

    coa_state_machine_set_handler(r->sm, COA_ST_REASON, h_reason, r);
    coa_state_machine_set_handler(r->sm, COA_ST_PLAN, h_plan, r);
    coa_state_machine_set_handler(r->sm, COA_ST_ACT, h_act, r);
    coa_state_machine_set_handler(r->sm, COA_ST_VERIFY, h_verify, r);
    coa_state_machine_set_handler(r->sm, COA_ST_LEARN, h_learn, r);
    return r;
}

void coa_reasoning_free(coa_reasoning *r) {
    if (!r) return;
    clear_actions(r);
    free(r->last_prompt);
    free(r->workspace);
    free(r->state_root);
    coa_mutex_lock(&r->hist_mtx);
    for (size_t i = 0; i < r->hist_n; i++) {
        free(r->hist_q[i]);
        free(r->hist_a[i]);
    }
    free(r->hist_q);
    free(r->hist_a);
    coa_mutex_unlock(&r->hist_mtx);
    coa_mutex_destroy(&r->hist_mtx);
    free(r->summary);
    free(r->last_plan_raw);
    free(r->prev_plan);
    free(r->round_log);
    coa_attention_free(r->attention);
    coa_state_machine_free(r->sm);
    coa_evaluator_free(r->eval);
    coa_tx_manager_free(r->txm);
    free(r);
}

coa_state_machine *coa_reasoning_sm(coa_reasoning *r) { return r ? r->sm : NULL; }

void coa_reasoning_set_llm(coa_reasoning *r, coa_llm *llm) {
    if (!r || !llm) return;
    r->llm = llm;
}

/* Optional: route each run through the multi-provider router (weighted
 * round-robin). Pass NULL to revert to the single configured LLM. */
void coa_reasoning_set_router(coa_reasoning *r, coa_router *router) {
    if (!r) return;
    r->router = router;
}

/* Threshold-triggered compaction: summarize the oldest `n_drop` turns with a
 * structured 9-section LLM prompt (Claude Code-style), store the rolling
 * summary, then drop those turns. On LLM failure the turns are kept and the
 * attempt is retried next threshold; 3 consecutive failures trip the breaker. */
static void compact_history(coa_reasoning *r, size_t n_drop) {
    if (!r || r->hist_n == 0) return;
    if (n_drop > r->hist_n) n_drop = r->hist_n;
    if (n_drop == 0) return;

    coa_strbuf tb;
    coa_strbuf_init(&tb);
    for (size_t i = 0; i < n_drop; i++) {
        coa_strbuf_appendf(&tb, "User: %s\nAssistant: %s\n\n",
                          r->hist_q[i] ? r->hist_q[i] : "",
                          r->hist_a[i] ? r->hist_a[i] : "");
    }
    char *turns = coa_strbuf_detach(&tb);

    coa_strbuf pb;
    coa_strbuf_init(&pb);
    coa_strbuf_append(&pb,
        "将以下早期对话压缩为结构化纪要，严格按以下 9 个小节输出（Markdown，"
        "每节 1-4 行，没有内容的写「无」）：\n"
        "1. 用户核心意图\n2. 技术概念与术语\n3. 涉及文件与代码\n4. 错误与修复\n"
        "5. 用户全部消息要点\n6. 已完成事项\n7. 未完成待办\n8. 当前工作状态\n"
        "9. 下一步建议\n"
        "总长度不超过 2000 字，只输出纪要本身，不要任何前言。\n\n## 待压缩对话\n");
    coa_strbuf_append(&pb, turns ? turns : "");
    free(turns);
    char *user_prompt = coa_strbuf_detach(&pb);

    char *sum = coa_llm_chat_simple(r->llm,
                                   "你是会话压缩器。输出简体中文 Markdown 纪要。",
                                   user_prompt);
    free(user_prompt);
    if (sum && *sum) {
        free(r->summary);
        r->summary = str_head(sum, COMPACT_SUMMARY_CAP);
        if (!r->summary) r->summary = coa_strdup(sum);
        r->compact_fails = 0;
        coa_log_info("reasoning: compacted %zu turns into a %zu-char summary",
                    n_drop, strlen(r->summary));
    } else {
        free(sum);
        r->compact_fails++;
        if (r->compact_fails >= 3) r->compact_disabled = 1; /* circuit breaker */
        coa_log_warn("reasoning: compaction LLM call failed (%d consecutive)",
                    r->compact_fails);
        return; /* keep the turns; retry at the next threshold */
    }
    for (size_t i = 0; i < n_drop; i++) {
        free(r->hist_q[i]);
        free(r->hist_a[i]);
    }
    memmove(r->hist_q, r->hist_q + n_drop, (r->hist_n - n_drop) * sizeof(char *));
    memmove(r->hist_a, r->hist_a + n_drop, (r->hist_n - n_drop) * sizeof(char *));
    r->hist_n -= n_drop;
}

/* Append a completed turn to the bounded multi-turn history.
 * Called from the run path; also read from the HTTP thread by
 * coa_reasoning_history_json, hence the hist_mtx guard. compact_history is
 * only ever invoked from here with the lock held (do not lock inside it). */
static void record_turn(coa_reasoning *r, const char *q, const char *a) {
    if (!r || !q || !a) return;
    coa_mutex_lock(&r->hist_mtx);
    if (r->hist_cap == 0) {
        r->hist_cap = 16;
        r->hist_q = calloc(r->hist_cap, sizeof(char *));
        r->hist_a = calloc(r->hist_cap, sizeof(char *));
    }
    if (r->hist_n >= r->hist_cap) {
        free(r->hist_q[0]);
        free(r->hist_a[0]);
        memmove(r->hist_q, r->hist_q + 1, (r->hist_cap - 1) * sizeof(char *));
        memmove(r->hist_a, r->hist_a + 1, (r->hist_cap - 1) * sizeof(char *));
        r->hist_n--;
    }
    r->hist_q[r->hist_n] = str_head(q, HIST_TURN_CAP);
    r->hist_a[r->hist_n] = str_head(a, HIST_TURN_CAP);
    if (!r->hist_q[r->hist_n]) r->hist_q[r->hist_n] = coa_strdup(q);
    if (!r->hist_a[r->hist_n]) r->hist_a[r->hist_n] = coa_strdup(a);
    r->hist_n++;
    /* ring full → compact the oldest half via LLM instead of silent loss */
    if (r->hist_n >= r->hist_cap && !r->compact_disabled && r->llm)
        compact_history(r, r->hist_cap / 2);
    coa_mutex_unlock(&r->hist_mtx);
}

char *coa_reasoning_history_json(coa_reasoning *r, int max_turns) {
    if (!r) return coa_strdup("[]");
    if (max_turns <= 0) max_turns = 20;
    coa_mutex_lock(&r->hist_mtx);
    size_t start = (r->hist_n > (size_t)max_turns) ? r->hist_n - (size_t)max_turns : 0;
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = start; i < r->hist_n; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "q", r->hist_q[i] ? r->hist_q[i] : "");
        cJSON_AddStringToObject(t, "a", r->hist_a[i] ? r->hist_a[i] : "");
        cJSON_AddItemToArray(arr, t);
    }
    coa_mutex_unlock(&r->hist_mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : coa_strdup("[]");
}

int coa_reasoning_run(coa_reasoning *r, const char *prompt, char **answer) {
    if (!r || !prompt) return -1;

    /* Ingestion guard: a prompt with invalid UTF-8 (e.g. a non-UTF-8 API
     * client) would poison memory/history and break every later LLM call. */
    char *safe_prompt = coa_str_utf8_sanitize(prompt);
    if (safe_prompt) prompt = safe_prompt;

    /* Router: pick a provider for this run (weighted round-robin). */
    coa_llm *picked = NULL;
    coa_llm *saved = NULL;
    if (r->router) {
        const coa_route *rt = coa_router_pick(r->router);
        if (rt) {
            picked = coa_llm_create(rt->provider, rt->base_url, rt->api_key, rt->model);
            if (picked) { saved = r->llm; r->llm = picked; }
        }
    }

    free(r->last_prompt);
    r->last_prompt = coa_strdup(prompt);
    r->gen_attempted = 0; /* one auto-generation attempt per run */
    if (r->mem) coa_memory_working_push(r->mem, prompt);

    /* lifecycle hook: a blocking before_run skips the whole run (a legitimate
     * refusal, like a policy denial — surfaced as the answer, not a failure) */
    if (r->hooks) {
        char *pj = NULL;
        cJSON *o = cJSON_CreateObject();
        if (o) {
            cJSON_AddStringToObject(o, "prompt", prompt);
            pj = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
        }
        int hb = coa_hook_dispatch(r->hooks, "agent.before_run", pj);
        free(pj);
        if (hb == 1) {
            if (r->metrics) coa_metrics_inc(r->metrics, "tasks.hook_blocked");
            if (answer) *answer = coa_strdup("(run blocked by hook)");
            free(safe_prompt);
            return 0;
        }
    }

    /* Bounded agent loop: each round runs the full state machine once. Round
     * results are fed back into the next round's planner context; the loop
     * ends when the LLM stops proposing actions (final text answer) or the
     * round budget is exhausted. */
    round_log_reset(r);
    free(r->last_plan_raw); r->last_plan_raw = NULL;
    free(r->prev_plan);     r->prev_plan = NULL;

    char *final_text = NULL;   /* LLM's plain-text answer (had_plan == 0) */
    char *result = NULL;       /* per-round pipeline output */
    coa_state st = COA_ST_FAILED;
    int stalled = 0;
    for (r->round_idx = 1; r->round_idx <= r->max_rounds; r->round_idx++) {
        free(result);
        result = NULL;
        st = coa_state_machine_run(r->sm, prompt, &result);
        if (st != COA_ST_DONE) break;
        if (!r->had_plan) { /* no actions planned → this is the final answer */
            final_text = coa_strdup(result ? result : "");
            break;
        }
        /* executed a planned round: keep the observation for the next round */
        round_log_append(r, result ? result : "");
        /* stall detection: the LLM proposed the exact same plan twice — no
         * progress is possible, stop instead of burning the round budget */
        if (r->prev_plan && r->last_plan_raw &&
            strcmp(r->prev_plan, r->last_plan_raw) == 0) {
            stalled = 1;
            break;
        }
    }

    /* Budget exhausted (or stalled) without a plain-text answer: force one
     * final tool-free LLM call to synthesize the gathered observations, so a
     * big task ends with a real answer instead of "(已达到最大轮数…)". */
    if (st == COA_ST_DONE && !final_text && r->round_log_len > 0 && r->llm) {
        char sys[320];
        snprintf(sys, sizeof(sys),
                 "You are finalizing an agent run. Based on the original request and the "
                 "action observations gathered below, produce the final answer in the "
                 "user's language. Do not propose any further tool calls. If the task is "
                 "incomplete, summarize what was accomplished and what remains.");
        char tail[12288];
        size_t loglen = r->round_log ? strlen(r->round_log) : 0;
        size_t start = loglen >= sizeof(tail) - 1 ? loglen - (sizeof(tail) - 1) : 0;
        snprintf(tail, sizeof(tail), "%s", r->round_log + start);
        char *user = (char *)malloc(strlen(prompt) + sizeof(tail) + 64);
        if (user) {
            snprintf(user, strlen(prompt) + sizeof(tail) + 64,
                     "任务: %s\n\n已执行动作的观察记录（末段）:\n%s", prompt, tail);
            char *ans = coa_llm_chat_simple(r->llm, sys, user);
            if (ans && *ans) final_text = ans;
            else free(ans);
            free(user);
        }
    }

    /* compose the answer: everything that happened + the final reply */
    coa_strbuf out;
    coa_strbuf_init(&out);
    if (r->round_log_len > 0) coa_strbuf_append(&out, r->round_log);
    if (final_text && *final_text) {
        if (r->round_log_len > 0) coa_strbuf_append(&out, "\n回答: ");
        coa_strbuf_append(&out, final_text);
    } else if (r->round_log_len > 0) {
        if (stalled)
            coa_strbuf_appendf(&out, "\n(连续两轮计划相同，已停止；任务可能未完全完成，第 %d/%d 轮)",
                              r->round_idx, r->max_rounds);
        else
            coa_strbuf_appendf(&out, "\n(已达到最大轮数 %d，任务可能未完全完成)", r->max_rounds);
    }
    free(final_text);
    free(result);
    free(r->last_plan_raw); r->last_plan_raw = NULL;
    free(r->prev_plan);     r->prev_plan = NULL;
    char *combined = coa_strbuf_detach(&out);

    if (r->metrics) coa_metrics_inc(r->metrics, st == COA_ST_DONE ? "tasks.done" : "tasks.failed");

    int ret = -1;
    if (st == COA_ST_DONE) {
        if (r->mem) coa_memory_working_push(r->mem, combined);
        record_turn(r, prompt, combined);
        if (r->hooks) {
            cJSON *o = cJSON_CreateObject();
            if (o) {
                cJSON_AddStringToObject(o, "prompt", prompt);
                cJSON_AddStringToObject(o, "status", "done");
                cJSON_AddStringToObject(o, "answer", combined ? combined : "");
                char *pj = cJSON_PrintUnformatted(o);
                coa_hook_dispatch(r->hooks, "agent.after_run", pj);
                free(pj);
                cJSON_Delete(o);
            }
        }
        if (answer) *answer = combined;
        else free(combined);
        ret = 0;
    } else {
        if (r->hooks) {
            cJSON *o = cJSON_CreateObject();
            if (o) {
                cJSON_AddStringToObject(o, "prompt", prompt);
                cJSON_AddStringToObject(o, "status", "failed");
                char *pj = cJSON_PrintUnformatted(o);
                coa_hook_dispatch(r->hooks, "agent.on_error", pj);
                free(pj);
                cJSON_Delete(o);
            }
        }
        if (answer) *answer = combined ? combined : coa_strdup("(pipeline failed)");
        else free(combined);
        ret = -1;
    }

    if (picked) { r->llm = saved; coa_llm_destroy(picked); }
    free(safe_prompt);
    return ret;
}

/* Session-memory snapshot: fixed-section notes + compaction state (JSON). */
char *coa_reasoning_session_json(coa_reasoning *r) {
    if (!r) return coa_strdup("{}");
    cJSON *o = cJSON_CreateObject();
    if (!o) return coa_strdup("{}");
    cJSON_AddStringToObject(o, "task", r->sn_task);
    cJSON_AddStringToObject(o, "state", r->sn_state);
    cJSON_AddStringToObject(o, "files", r->sn_files);
    cJSON_AddStringToObject(o, "errors", r->sn_errors);
    cJSON_AddStringToObject(o, "worklog", r->sn_worklog);
    cJSON_AddStringToObject(o, "summary", r->summary ? r->summary : "");
    cJSON_AddNumberToObject(o, "history_turns", (double)r->hist_n);
    cJSON_AddBoolToObject(o, "compaction_disabled", r->compact_disabled ? 1 : 0);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s ? s : coa_strdup("{}");
}

/* HyDE primitive: LLM writes a hypothetical answer passage for `query`; the
 * caller embeds passage-to-passage for retrieval (see reasoning.h). */
char *coa_hyde_passage(coa_llm *llm, const char *query) {
    if (!llm || !query || !*query) return NULL;
    char prompt[1200];
    snprintf(prompt, sizeof(prompt),
             "Write a short passage (3-5 sentences) that directly answers the "
             "question. Output only the passage, no preamble.\n\nQuestion: %.900s",
             query);
    return coa_llm_chat_simple(llm,
                              "You generate concise hypothetical answer passages "
                              "used for semantic retrieval.",
                              prompt);
}
