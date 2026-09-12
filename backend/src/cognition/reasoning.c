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
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/tx/tx.h"
#include "cognitive-os-agent/execution/executor.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/infra/logging.h"
#include "cognitive-os-agent/infra/metrics.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

/* One chat session: isolated conversation history, compaction summary and
 * session notes. Sessions are created on demand by reasoning_run_ex
 * (id NULL or "" = the default shared session, used by every legacy caller). */
#define SESSION_MAX 64
struct session {
    char *id;
    mutex_t mtx;
    char **hist_q;
    char **hist_a;
    size_t hist_n, hist_cap;
    char *summary;         /* LLM-compacted summary of dropped turns */
    int compact_fails;     /* consecutive compaction LLM failures */
    int compact_disabled;  /* circuit breaker: stop trying after 3 failures */
    char sn_state[256];    /* current state / progress */
    char sn_task[256];     /* current task */
    char sn_files[256];    /* files touched this session */
    char sn_errors[256];   /* recent errors */
    char sn_worklog[1024]; /* append-only per-action log (tail kept) */
    long long last_active_ms;
};

static struct session *session_new(const char *id) {
    struct session *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->id = xstrdup(id && *id ? id : "default");
    if (!s->id) {
        free(s);
        return NULL;
    }
    mutex_init(&s->mtx);
    s->last_active_ms = (long long)time_now_ms();
    return s;
}

static void session_free(struct session *s) {
    if (!s)
        return;
    for (size_t i = 0; i < s->hist_n; i++) {
        free(s->hist_q[i]);
        free(s->hist_a[i]);
    }
    free(s->hist_q);
    free(s->hist_a);
    mutex_destroy(&s->mtx);
    free(s->summary);
    free(s->id);
    free(s);
}

/* Find-or-create the session with this id (NULL/"" = default). Registry cap
 * SESSION_MAX; beyond it the default session is reused (no unbounded
 * growth from hostile clients). Callers hold no run in flight. (Defined
 * after struct reasoning — see below.) */

struct reasoning {
    llm *llm;
    tool_registry *tools;
    memory *mem;
    policy_engine *policy;
    snapshot *snap;
    event_bus *bus;
    metrics *metrics;
    tx_manager *txm;
    evaluator *eval;
    char *workspace;
    int use_transaction;

    /* Context MMU budgets (chars per prompt section, auto-degrading) */
    int budget_hot, budget_warm, budget_cold;
    int hyde;
    const char *exec_backend; /* "local" | "wsl" | "remote" (NULL = local) */
    const char *exec_host;    /* ssh target for "remote" */

    router *router;       /* optional multi-provider routing (NULL = single LLM) */
    attention *attention; /* salience ranking over retrieved context */

    /* multi-turn conversation history (bounded ring of recent turns), kept
     * PER CHAT SESSION. sess_mtx guards the session registry; each session's
     * own mtx guards its ring: the reasoning run itself is serialized by the
     * ctx run-lock, but /v1/chat/history reads a ring from the HTTP thread
     * while a run may be in flight. `cur` is the session selected for the
     * current run (set at run entry, stable for the run's duration). */
    mutex_t sess_mtx;
    struct session **sessions;
    size_t nsessions, scap;
    struct session *cur;

    /* code index: touched files are indexed for term -> file:line recall */
    struct ret_index *index;

    /* missing-capability auto-generation (self-evolution loop) */
    struct plugin_registry *plugin_registry;
    char *state_root;
    int gen_attempted; /* one auto-generation attempt per run */

    state_machine *sm;
    hook_registry *hooks; /* horizontal hook system (borrowed, may be NULL) */

    /* transient per-run state */
    planned_action *actions;
    int n_actions;
    char *last_prompt;
    int all_actions_ok;
    int ok_actions;
    int denied_actions;                /* blocked by policy this run (not a failure) */
    struct skill_registry *skills; /* advertised to planner + skill tool */
    struct mcp_manager *mcp;       /* handed to tool ctx for mcp tool calls */

    /* agent loop: bounded plan->act->observe->replan rounds per run. Results of
     * executed rounds are fed back into the next round's planner context; the
     * loop ends when the LLM stops proposing actions (final text answer). */
    int max_rounds;      /* from config (default AGENT_LOOP_MAX_ROUNDS) */
    int round_idx;       /* 1-based round currently executing */
    int stall_nudged;    /* one-shot stall-recovery nudge already given */
    int intent_nudged;   /* intent-narration nudges given this run (bounded) */
    int had_plan;        /* last REASON produced tool actions (vs final text) */
    char *last_plan_raw; /* this round's raw plan (stall detection) */
    char *prev_plan;     /* previous round's raw plan (stall detection) */
    char *round_log;     /* accumulated action results of previous rounds */
    size_t round_log_len, round_log_cap;
};

static struct session *session_get(reasoning *r, const char *id) {
    const char *want = (id && *id) ? id : "default";
    mutex_lock(&r->sess_mtx);
    for (size_t i = 0; i < r->nsessions; i++)
        if (strcmp(r->sessions[i]->id, want) == 0) {
            struct session *s = r->sessions[i];
            mutex_unlock(&r->sess_mtx);
            return s;
        }
    struct session *s = NULL;
    if (r->nsessions < SESSION_MAX) {
        s = session_new(want);
        if (s) {
            struct session **na = realloc(r->sessions, (r->nsessions + 1) * sizeof(*na));
            if (na) {
                r->sessions = na;
                r->sessions[r->nsessions++] = s;
            } else {
                session_free(s);
                s = NULL;
            }
        }
    }
    if (!s) { /* cap reached or alloc failed: fall back to default */
        for (size_t i = 0; i < r->nsessions; i++)
            if (strcmp(r->sessions[i]->id, "default") == 0) {
                s = r->sessions[i];
                break;
            }
    }
    mutex_unlock(&r->sess_mtx);
    return s;
}

static void clear_actions(reasoning *r) {
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
#define HIST_TURN_CAP 500        /* per-turn chars kept in history */
#define HIST_BUDGET 8192         /* total chars of history injected per run */
#define LEARN_RESULT_CAP 300     /* chars of a result kept as a memory episode */
#define COMPACT_SUMMARY_CAP 2000 /* rolling compaction summary cap */
#define AGENT_LOOP_MAX_ROUNDS                                                                                          \
    32                      /* default rounds when config does not set it;                                             \
                               config "reasoning.max_rounds" < 0 = unlimited */
/* Tail-keep cap for accumulated round results. 16K was too small for long
 * agent runs: one file_read of a source file easily produces 5-15K, so a
 * multi-round SWE-style task overflowed within 2-3 reads and the halving
 * eviction silently discarded the code read earlier — the model then
 * re-read files or planned against half-missing observations. */
#define ROUND_LOG_CAP 65536
/* Per-entry cap: an action result larger than this is truncated head+tail
 * (middle elided) BEFORE appending, so one huge output cannot evict many
 * earlier observations — more distinct results survive the same budget. */
#define ROUND_LOG_ENTRY_CAP 6144
#define ROUND_LOG_HEAD_KEEP 4096
#define ROUND_LOG_TAIL_KEEP 1536

/* Append text to the round log, tail-keeping: once past ROUND_LOG_CAP the
 * oldest half is dropped so recent action results always stay available. */
static void round_log_append(reasoning *r, const char *text) {
    if (!text || !*text)
        return;
    const char *add = text;
    char *elided = NULL;
    size_t len = strlen(text);
    if (len > ROUND_LOG_ENTRY_CAP) {
        /* head+tail keep with an explicit elision marker: the model sees
         * the beginning and end of the output, not a silent hole */
        size_t mid = len - ROUND_LOG_HEAD_KEEP - ROUND_LOG_TAIL_KEEP;
        char marker[80];
        int mlen = snprintf(marker, sizeof(marker),
                            "\n...[%zu bytes of output elided]...\n", mid);
        elided = (char *)malloc(ROUND_LOG_HEAD_KEEP + (size_t)mlen +
                                ROUND_LOG_TAIL_KEEP + 1);
        if (elided) {
            memcpy(elided, text, ROUND_LOG_HEAD_KEEP);
            memcpy(elided + ROUND_LOG_HEAD_KEEP, marker, (size_t)mlen);
            memcpy(elided + ROUND_LOG_HEAD_KEEP + mlen,
                   text + len - ROUND_LOG_TAIL_KEEP, ROUND_LOG_TAIL_KEEP);
            elided[ROUND_LOG_HEAD_KEEP + (size_t)mlen + ROUND_LOG_TAIL_KEEP] = '\0';
            add = elided;
            len = strlen(elided);
        }
    }
    size_t need = r->round_log_len + len + 1;
    if (need > r->round_log_cap) {
        size_t ncap = r->round_log_cap ? r->round_log_cap * 2 : 2048;
        while (ncap < need)
            ncap *= 2;
        char *nb = (char *)realloc(r->round_log, ncap);
        if (!nb) {
            free(elided);
            return;
        }
        r->round_log = nb;
        r->round_log_cap = ncap;
    }
    memcpy(r->round_log + r->round_log_len, add, len + 1);
    r->round_log_len += len;
    free(elided);
    if (r->round_log_len > ROUND_LOG_CAP) {
        size_t half = r->round_log_len / 2;
        memmove(r->round_log, r->round_log + half, r->round_log_len - half + 1);
        r->round_log_len -= half;
        /* tell the model the log was folded, so it knows earlier results
         * may be gone and can re-read if truly needed */
        static const char fold_note[] =
            "(earlier action results were folded away to fit the budget)\n";
        size_t nlen = strlen(fold_note);
        if (r->round_log_cap > nlen + r->round_log_len + 1) {
            memmove(r->round_log + nlen, r->round_log, r->round_log_len + 1);
            memcpy(r->round_log, fold_note, nlen);
            r->round_log_len += nlen;
        }
    }
}

static void round_log_reset(reasoning *r) {
    if (r->round_log)
        r->round_log[0] = '\0';
    r->round_log_len = 0;
}

/* session notes: append "line\n" to a fixed-size buffer, keeping the TAIL
 * (oldest lines are dropped from the front when the cap would be exceeded). */
static void sn_append_line(char *dst, size_t cap, const char *line) {
    if (!dst || !line || !*line)
        return;
    size_t cur = strlen(dst);
    size_t add = strlen(line) + 1; /* line chars + '\n' */
    while (cur + add + 1 > cap) {
        char *nl = strchr(dst, '\n');
        if (!nl) {
            dst[0] = '\0';
            cur = 0;
            break;
        }
        size_t cut = (size_t)(nl - dst) + 1;
        memmove(dst, dst + cut, cur - cut + 1);
        cur -= cut;
    }
    size_t room = cap - 1 - cur;
    if (room < 2)
        return;
    if (add > room)
        add = room;
    size_t keep = add - 1;
    while (keep > 0 && ((unsigned char)line[keep] & 0xC0) == 0x80)
        keep--; /* utf-8 boundary */
    memcpy(dst + cur, line, keep);
    dst[cur + keep] = '\n';
    dst[cur + keep + 1] = '\0';
}

/* Track a file touched by a file_* action (extract "path" from its args). */
static void sn_note_file(reasoning *r, const char *args_json) {
    if (!args_json || !*args_json)
        return;
    cJSON *o = cJSON_Parse(args_json);
    if (!o)
        return;
    cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "path");
    if (p && cJSON_IsString(p) && p->valuestring)
        sn_append_line(r->cur->sn_files, sizeof(r->cur->sn_files), p->valuestring);
    cJSON_Delete(o);
}

static char *str_head(const char *s, size_t cap) {
    if (!s)
        return NULL;
    size_t n = strlen(s);
    int trunc = n > cap;
    if (trunc)
        n = cap;
    while (trunc && n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--; /* utf-8 boundary */
    char *out = (char *)malloc(n + 4);
    if (!out)
        return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    if (trunc)
        memcpy(out + n, "…", 4);
    return out;
}

/* Build an augmented prompt for the planner: recent multi-turn history, then
 * retrieved RAG context (ranked by attention), then the current request.
 * Caller frees the returned string. */
static char *build_context(reasoning *r, const char *prompt) {
    strbuf b;
    strbuf_init(&b);

    /* WARM tier: compaction summary + session notes under an explicit budget.
     * Over budget the lowest-value sections shed first:
     * worklog -> errors/files -> task/state only. */
    size_t warm_used = 0;
    if (r->cur->summary && *r->cur->summary) {
        strbuf_append(&b, "## Earlier conversation summary\n");
        strbuf_append(&b, r->cur->summary);
        strbuf_append(&b, "\n\n");
        warm_used += strlen(r->cur->summary) + 34;
    }
    if (r->cur->sn_task[0] || r->cur->sn_state[0] || r->cur->sn_files[0] || r->cur->sn_errors[0] ||
        r->cur->sn_worklog[0]) {
        size_t budget_left = (warm_used < (size_t)r->budget_warm) ? (size_t)r->budget_warm - warm_used : 0;
        for (int lv = 0; lv < 3; lv++) { /* 0=full 1=no worklog 2=minimal */
            strbuf nb;
            strbuf_init(&nb);
            strbuf_append(&nb, "## Session notes\n");
            if (r->cur->sn_task[0])
                strbuf_appendf(&nb, "- 任务: %s\n", r->cur->sn_task);
            if (r->cur->sn_state[0])
                strbuf_appendf(&nb, "- 状态: %s\n", r->cur->sn_state);
            if (lv < 2 && r->cur->sn_files[0])
                strbuf_appendf(&nb, "- 本会话涉及文件:\n%s", r->cur->sn_files);
            if (lv < 2 && r->cur->sn_errors[0])
                strbuf_appendf(&nb, "- 近期错误:\n%s", r->cur->sn_errors);
            if (lv < 1 && r->cur->sn_worklog[0])
                strbuf_appendf(&nb, "- 工作日志:\n%s", r->cur->sn_worklog);
            strbuf_append(&nb, "\n");
            if (nb.len <= budget_left || lv == 2) {
                warm_used += nb.len;
                strbuf_append(&b, nb.buf ? nb.buf : "");
                strbuf_free(&nb);
                break;
            }
            strbuf_free(&nb);
        }
    }

    /* HOT tier: multi-turn history (bounded, most-recent-last). Newest turns
     * are kept whole; older turns beyond the hot budget degrade to one line. */
    size_t hot_mark = b.len;
    mutex_lock(&r->cur->mtx);
    if (r->cur->hist_n > 0) {
        strbuf_append(&b, "## Conversation history\n");
        size_t start = r->cur->hist_n > 6 ? r->cur->hist_n - 6 : 0;
        /* walk newest->oldest; the first turn that pushes the running total
         * over budget (and everything before it) is degraded to one line */
        size_t keep_from = start;
        size_t total = 0;
        int over = 0;
        for (size_t i = r->cur->hist_n; i-- > start;) {
            size_t cost = (r->cur->hist_q[i] ? strlen(r->cur->hist_q[i]) : 0) +
                          (r->cur->hist_a[i] ? strlen(r->cur->hist_a[i]) : 0) + 24;
            total += cost;
            if (total > (size_t)r->budget_hot) {
                keep_from = i + 1;
                over = 1;
                break;
            }
        }
        for (size_t i = start; i < r->cur->hist_n; i++) {
            if (over && i < keep_from) {
                char *qh = str_head(r->cur->hist_q[i], 120);
                strbuf_appendf(&b, "User: %s → Assistant: [earlier turn omitted]\n", qh ? qh : "");
                free(qh);
                continue;
            }
            if (r->cur->hist_q[i])
                strbuf_appendf(&b, "User: %s\n", r->cur->hist_q[i]);
            if (r->cur->hist_a[i])
                strbuf_appendf(&b, "Assistant: %s\n", r->cur->hist_a[i]);
        }
        strbuf_append(&b, "\n");
    }
    mutex_unlock(&r->cur->mtx);

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
            char *passage = hyde_passage(r->llm, prompt);
            if (passage) {
                snprintf(hyde_query_buf, sizeof(hyde_query_buf), "%.1000s", passage);
                free(passage);
                retrieval_query = hyde_query_buf;
            }
        }
        char *ctx_json = context_build(r->mem, retrieval_query, 12);
        if (ctx_json) {
            cJSON *arr = cJSON_Parse(ctx_json);
            if (arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
                int n = cJSON_GetArraySize(arr);
                attention_candidate *cands = calloc((size_t)n, sizeof(*cands));
                for (int i = 0; i < n; i++) {
                    cJSON *it = cJSON_GetArrayItem(arr, i);
                    cJSON *t = cJSON_GetObjectItem(it, "text");
                    cJSON *res = cJSON_GetObjectItem(it, "result");
                    const char *txt = (t && t->valuestring)       ? t->valuestring
                                      : (res && res->valuestring) ? res->valuestring
                                                                  : "";
                    cands[i].text = txt;
                    cands[i].tags = "";
                    cands[i].boost = 0.0;
                }
                attention_result ress[6];
                int kmax = r->budget_cold >= 2400 ? 6 : (r->budget_cold >= 1000 ? 3 : 1);
                int k = attention_select(r->attention, prompt, cands, (size_t)n, ress, kmax);
                char *rendered = NULL;
                if (k > 0) {
                    cJSON *sel = cJSON_CreateArray();
                    for (int i = 0; i < k; i++) {
                        cJSON *it = cJSON_GetArrayItem(arr, ress[i].index);
                        if (it)
                            cJSON_AddItemToArray(sel, cJSON_Duplicate(it, 1));
                    }
                    char *sel_json = cJSON_PrintUnformatted(sel);
                    rendered = context_render_text(sel_json);
                    free(sel_json);
                    cJSON_Delete(sel);
                } else {
                    rendered = context_render_text(ctx_json);
                }
                free(cands);
                if (rendered) {
                    strbuf_append(&b, "## Retrieved context\n");
                    if (strlen(rendered) > (size_t)r->budget_cold) {
                        char *cut = str_head(rendered, (size_t)r->budget_cold);
                        strbuf_append(&b, cut ? cut : "");
                        free(cut);
                        strbuf_append(&b, "\n(超出 cold 预算，已截断)\n\n");
                    } else {
                        strbuf_append(&b, rendered);
                        strbuf_append(&b, "\n\n");
                    }
                    free(rendered);
                }
            }
            if (arr)
                cJSON_Delete(arr);
            free(ctx_json);
        }
    }

    /* code index: where the request's terms live in workspace files */
    if (r->index) {
        char *hits = index_search(r->index, prompt, 5);
        if (hits && strcmp(hits, "[]") != 0) {
            cJSON *hroot = cJSON_Parse(hits);
            if (hroot && cJSON_IsArray(hroot)) {
                strbuf_append(&b, "## Code index\n");
                cJSON *it;
                cJSON_ArrayForEach(it, hroot) {
                    cJSON *f = cJSON_GetObjectItemCaseSensitive(it, "file");
                    cJSON *l = cJSON_GetObjectItemCaseSensitive(it, "line");
                    cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "term");
                    if (f && cJSON_IsString(f))
                        strbuf_appendf(&b, "- %s:%d (term: %s)\n", f->valuestring,
                                           l && cJSON_IsNumber(l) ? (int)l->valuedouble : 0,
                                           t && cJSON_IsString(t) ? t->valuestring : "");
                }
                strbuf_append(&b, "\n");
            }
            if (hroot)
                cJSON_Delete(hroot);
        }
        free(hits);
    }

    /* agent-loop feedback: results of the rounds executed so far in this run,
     * so the planner can decide "next actions" vs "task complete" */
    if (r->round_log_len > 0) {
        strbuf_appendf(&b, "## 之前轮次的动作结果 (第 %d/%d 轮)\n", r->round_idx - 1, r->max_rounds);
        strbuf_append(&b, r->round_log);
        if (r->round_idx >= r->max_rounds)
            strbuf_append(&b, "\n这是最后一轮。不要再调用任何工具，也不要输出 JSON 动作；"
                                  "直接基于以上动作结果用纯文本给出最终答案（说明完成了什么、"
                                  "或还缺什么信息）。\n\n");
        else
            strbuf_append(&b, "\n你是多轮 agent 循环：根据以上动作结果，(a) 任务已完成 → 直接用纯文本回答；"
                                  "(b) 未完成 → 给出下一批 JSON 动作。不要重复已成功的动作。\n\n");
    }

    /* anti-pollution caution: history/notes/retrieved context are reference
     * only — the current request must always be planned and executed fresh */
    strbuf_append(&b, "## 重要约束\n"
                          "- 上面的会话摘要、Session notes、Conversation history、Retrieved context 都只是背景参考，"
                          "不代表当前请求已经完成。\n"
                          "- 即使历史记录里出现过相似的任务，也必须针对「Current request」重新规划并实际执行动作，"
                          "不允许凭历史记录直接回答\"已完成\"。\n"
                          "- 回答中声称对文件做过任何改动，必须以本轮实际出现的 [tool] 动作结果为依据；"
                          "没有实际执行过对应动作，就不得声称做过。\n\n");

    /* Context MMU accounting: per-tier bytes of this prompt */
    if (r->metrics) {
        metrics_set(r->metrics, "context.bytes_hot", (double)(b.len - hot_mark));
        metrics_set(r->metrics, "context.bytes_warm", (double)warm_used);
        metrics_set(r->metrics, "context.bytes_cold", (double)(b.len - cold_mark));
    }

    strbuf_append(&b, "## Current request\n");
    strbuf_append(&b, prompt);
    return strbuf_detach(&b);
}

/* REASON: ask the LLM for a plan (JSON array of actions, or plain text). */
static int h_reason(state_machine *sm, void *ud, const char *input, char **out) {
    reasoning *r = ud;
    (void)sm;
    clear_actions(r);
    r->ok_actions = 0;
    r->denied_actions = 0;
    char *aug = build_context(r, input);
    if (!r->llm) {
        free(aug);
        *out = xstrdup("(no LLM provider configured)");
        return 0;
    }
    char *raw = NULL;
    char *plan_err = NULL;
    int rc = planner_plan_ex(r->llm, r->tools, r->skills, r->policy, aug ? aug : input, &r->actions, &r->n_actions,
                                 &raw, &plan_err);
    free(aug);
    if (rc != 0 || !raw) {
        free(raw);
        char *msg = plan_err ? xstrdup(plan_err)
                             : xstrdup("(LLM 调用失败：请先用「测试」按钮验证模型配置 provider/key/base_url/model)");
        free(plan_err);
        log_error("reasoning: LLM returned no plan: %s", msg);
        *out = msg; /* surfaced as the task result on FAILED */
        return -1;  /* move to FAILED */
    }
    log_info("reasoning: LLM plan: %s", raw);
    r->had_plan = r->n_actions > 0;
    /* remember this round's plan for stall detection (identical plan twice in
     * a row means the loop is not making progress); copy — raw flows out */
    free(r->prev_plan);
    r->prev_plan = r->last_plan_raw;
    r->last_plan_raw = xstrdup(raw);
    if (r->bus) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "plan", raw);
        event_bus_publish(r->bus, EV_MODEL, "reasoning", p);
    }
    *out = raw;
    return 0;
}

/* PLAN: the planner already parsed the plan in REASON; this stage passes it on. */
static int h_plan(state_machine *sm, void *ud, const char *input, char **out) {
    reasoning *r = ud;
    (void)sm;
    if (r->n_actions == 0)
        log_info("reasoning: no tool plan, treating LLM output as answer");
    *out = xstrdup(input);
    return 0;
}

/* ACT: execute the planned actions, wrapped in a transaction. */
static int h_act(state_machine *sm, void *ud, const char *input, char **out) {
    reasoning *r = ud;
    (void)sm;
    r->all_actions_ok = 1;
    strbuf b;
    strbuf_init(&b);

    if (r->n_actions == 0) {
        strbuf_append(&b, input);
        *out = strbuf_detach(&b);
        return 0;
    }

    tool_ctx tctx;
    memset(&tctx, 0, sizeof(tctx));
    tctx.reg = r->tools;
    tctx.policy = r->policy;
    tctx.snapshot = r->snap;
    tctx.bus = r->bus;
    tctx.workspace = r->workspace;
    tctx.metrics = r->metrics;
    tctx.skills = r->skills;
    tctx.mcp = r->mcp;

    tx *tx = NULL;
    if (r->use_transaction && r->snap)
        tx = tx_begin(r->txm, r->snap, r->tools, &tctx);

    /* Execution Runtime: all non-tx actions run behind the executor interface.
     * exec_backend routes shell commands through WSL / ssh by wrapping the
     * local executor (non-shell tools pass through unchanged). */
    executor *exec = tx ? NULL : executor_new_local(r->tools, &tctx, r->snap);
    if (exec && r->exec_backend && strcmp(r->exec_backend, "wsl") == 0) {
        executor *w = executor_new_wsl(exec, r->exec_host);
        if (w)
            exec = w;
    } else if (exec && r->exec_backend && strcmp(r->exec_backend, "remote") == 0 && r->exec_host && *r->exec_host) {
        executor *w = executor_new_remote(exec, r->exec_host);
        if (w)
            exec = w;
    }

    for (int i = 0; i < r->n_actions; i++) {
        scheduler_yield(); /* cooperative checkpoint between tool actions */
        /* policy hard-block: a denied action is intentionally NOT executed.
         * Record the refusal and keep going — a policy refusal is a legitimate
         * outcome, not an infrastructure failure, so it must not fail the
         * pipeline (the planner sees the refusal in the next round). */
        if (r->policy) {
            const char *preason = NULL;
            if (policy_check(r->policy, r->actions[i].tool, r->actions[i].args_json, &preason) == POLICY_DENY) {
                r->denied_actions++;
                strbuf_appendf(&b, "[%s] denied by policy (%s)\n", r->actions[i].tool, preason ? preason : "rule");
                char el[128];
                snprintf(el, sizeof(el), "%s 被策略拒绝", r->actions[i].tool);
                sn_append_line(r->cur->sn_errors, sizeof(r->cur->sn_errors), el);
                snprintf(el, sizeof(el), "[%s] DENIED", r->actions[i].tool);
                sn_append_line(r->cur->sn_worklog, sizeof(r->cur->sn_worklog), el);
                if (r->metrics)
                    metrics_inc(r->metrics, "tools.denied");
                continue;
            }
        }
        /* missing-capability self-evolution: the planner referenced a tool
         * that does not exist — try generating a plugin for it (once per run)
         * and bind the generated skill under the planned tool name. */
        if (!r->gen_attempted && r->plugin_registry && r->llm && r->skills &&
            !tool_find(r->tools, r->actions[i].tool)) {
            r->gen_attempted = 1;
            char *desc = str_head(r->last_prompt ? r->last_prompt : r->actions[i].tool, 300);
            plugin_gen_deps gd;
            memset(&gd, 0, sizeof(gd));
            gd.llm = r->llm;
            gd.registry = r->plugin_registry;
            gd.skills = r->skills;
            gd.state_root = r->state_root;
            char *gjson = plugin_generate_deps(&gd, desc ? desc : r->actions[i].tool);
            free(desc);
            if (gjson) {
                cJSON *g = cJSON_Parse(gjson);
                cJSON *okj = g ? cJSON_GetObjectItemCaseSensitive(g, "ok") : NULL;
                cJSON *pj = g ? cJSON_GetObjectItemCaseSensitive(g, "plugin") : NULL;
                cJSON *nj = pj ? cJSON_GetObjectItemCaseSensitive(pj, "name") : NULL;
                if (okj && cJSON_IsTrue(okj) && nj && cJSON_IsString(nj) &&
                    tool_register_generated(r->tools, r->skills, r->actions[i].tool, nj->valuestring) == 0) {
                    log_info("reasoning: auto-generated plugin '%s' bound as tool '%s'", nj->valuestring,
                                 r->actions[i].tool);
                    /* persist the tool -> skill binding so the capability
                     * re-binds at startup instead of regenerating */
                    if (r->state_root)
                        tool_generated_save_mapping(r->state_root, r->actions[i].tool, nj->valuestring);
                }
                if (g)
                    cJSON_Delete(g);
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
                cJSON *ho = r->actions[i].args_json ? cJSON_Parse(r->actions[i].args_json) : NULL;
                if (ho)
                    cJSON_AddItemToObject(wrap, "args", ho);
                hp = cJSON_PrintUnformatted(wrap);
                cJSON_Delete(wrap);
            }
            if (hook_dispatch(r->hooks, "exec.before_execute", hp) == 1) {
                free(hp);
                r->denied_actions++;
                strbuf_appendf(&b, "[%s] blocked by hook\n", r->actions[i].tool);
                char el[128];
                snprintf(el, sizeof(el), "%s 被 hook 拦截", r->actions[i].tool);
                sn_append_line(r->cur->sn_errors, sizeof(r->cur->sn_errors), el);
                if (r->metrics)
                    metrics_inc(r->metrics, "tools.hook_blocked");
                continue;
            }
            free(hp);
        }
        int rc;
        if (tx) {
            rc = tx_run(tx, r->actions[i].tool, r->actions[i].args_json);
        } else if (exec) {
            executor_result *er = NULL;
            int erc = executor_execute(exec, r->actions[i].tool, r->actions[i].args_json, &er);
            rc = (erc == 0 && er && er->ok) ? 0 : -1;
            if (er) {
                /* executor output is already UTF-8 sanitized */
                strbuf_appendf(&b, "[%s] %s\n", r->actions[i].tool, er->output ? er->output : "");
                executor_result_free(er);
            }
        } else {
            rc = -1;
        }
        if (rc != 0) {
            r->all_actions_ok = 0;
            strbuf_appendf(&b, "[%s] FAILED\n", r->actions[i].tool);
            log_warn("reasoning: action '%s' failed", r->actions[i].tool);
            if (r->hooks)
                hook_dispatch(r->hooks, "exec.on_failure", r->actions[i].tool);
        } else {
            r->ok_actions++;
            strbuf_appendf(&b, "[%s] ok\n", r->actions[i].tool);
            if (r->hooks)
                hook_dispatch(r->hooks, "exec.after_execute", r->actions[i].tool);
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
                    path_resolve(full, sizeof(full), r->workspace, pj->valuestring);
                    char *content = fs_read_file(full);
                    if (content) {
                        index_add_file(r->index, full, content);
                        free(content);
                    }
                }
                if (ao)
                    cJSON_Delete(ao);
            }
        }
        if (rc != 0) {
            char el[96];
            snprintf(el, sizeof(el), "%s 执行失败", r->actions[i].tool);
            sn_append_line(r->cur->sn_errors, sizeof(r->cur->sn_errors), el);
        }
        char wl[128];
        snprintf(wl, sizeof(wl), "[%s] %s", r->actions[i].tool, rc == 0 ? "ok" : "FAILED");
        sn_append_line(r->cur->sn_worklog, sizeof(r->cur->sn_worklog), wl);
    }
    if (r->n_actions > 0) {
        snprintf(r->cur->sn_state, sizeof(r->cur->sn_state), "%d/%d 个动作已执行%s", r->ok_actions, r->n_actions,
                 r->all_actions_ok ? "" : "，部分失败");
    }
    executor_free(exec);

    if (tx) {
        if (r->all_actions_ok && tx_validate(tx)) {
            tx_commit(tx);
            log_info("reasoning: transaction committed");
        } else {
            tx_rollback(tx);
            log_warn("reasoning: transaction rolled back");
        }
        const char *tout = tx_output(tx);
        if (tout && *tout) {
            char *safe = str_utf8_sanitize(tout);
            strbuf_append(&b, safe ? safe : tout);
            free(safe);
        }
        tx_free(tx);
    }
    if (r->metrics)
        metrics_add(r->metrics, "actions.executed", (double)r->n_actions);

    *out = strbuf_detach(&b);
    return 0;
}

/* VERIFY: any action failure fails the whole pipeline — but the surfaced
 * result must carry the real per-action outputs (which tool failed and why),
 * not an opaque message. Policy-denied actions are NOT failures: a plan whose
 * actions were all legitimately refused is a correct outcome (the run
 * completes with the refusal text). `input` is the ACT stage's report. */
static int h_verify(state_machine *sm, void *ud, const char *input, char **out) {
    reasoning *r = ud;
    (void)sm;
    int eff_total = r->n_actions - r->denied_actions;
    if (!evaluator_verify(r->eval, r->all_actions_ok, eff_total, r->ok_actions)) {
        strbuf b;
        strbuf_init(&b);
        strbuf_appendf(&b, "%d/%d 个动作执行失败，各动作结果：\n", eff_total - r->ok_actions, eff_total);
        strbuf_append(&b, (input && *input) ? input : "(无工具输出)");
        *out = strbuf_detach(&b);
        return -1;
    }
    *out = xstrdup(input);
    return 0;
}

/* LEARN: record the episode into memory. The raw result (which can embed
 * kilobytes of tool output) is distilled to its first ~300 chars first —
 * storing everything verbatim permanently bloats retrieval. */
static int h_learn(state_machine *sm, void *ud, const char *input, char **out) {
    reasoning *r = ud;
    (void)sm;
    /* session notes: current task + end-of-run state */
    if (r->last_prompt && *r->last_prompt) {
        char *t = str_head(r->last_prompt, 200);
        if (t) {
            snprintf(r->cur->sn_task, sizeof(r->cur->sn_task), "%s", t);
            free(t);
        }
    }
    snprintf(r->cur->sn_state, sizeof(r->cur->sn_state), "%s",
             r->all_actions_ok ? "上一任务已完成" : "上一任务部分失败");
    if (r->mem) {
        /* episode only on the final round: intermediate rounds would record
         * raw tool output into episodic memory; KG edges stay per-round */
        if (!r->had_plan || r->round_idx >= r->max_rounds) {
            char *shaped = str_head(input, LEARN_RESULT_CAP);
            memory_record_experience(r->mem, r->last_prompt ? r->last_prompt : "(task)", shaped ? shaped : input);
            free(shaped);
            /* consolidation automation: threshold+interval gated pass over the
             * episodes (semantic themes + procedural tool facts) */
            if (memory_maybe_consolidate(r->mem, 10, 60000) == 1 && r->metrics)
                metrics_inc(r->metrics, "memory.consolidations");
        }
        /* knowledge-graph edges: task -used-> tool -touched-> file */
        if (r->last_prompt && r->n_actions > 0) {
            char *th = str_head(r->last_prompt, 80);
            for (int i = 0; i < r->n_actions; i++) {
                memory_record_edge(r->mem, th ? th : "(task)", r->actions[i].tool, "used_tool");
                if (strncmp(r->actions[i].tool, "file_", 5) == 0 && r->actions[i].args_json) {
                    cJSON *ao = cJSON_Parse(r->actions[i].args_json);
                    cJSON *pj = ao ? cJSON_GetObjectItemCaseSensitive(ao, "path") : NULL;
                    if (pj && cJSON_IsString(pj) && pj->valuestring)
                        memory_record_edge(r->mem, r->actions[i].tool, pj->valuestring, "touched");
                    cJSON_Delete(ao);
                }
            }
            free(th);
        }
        memory_flush(r->mem);
    }
    if (r->metrics) {
        double q = evaluator_score(r->eval, r->n_actions, r->ok_actions, r->all_actions_ok, input);
        metrics_set(r->metrics, "reasoning.quality", q);
    }
    if (r->bus) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "result", input);
        event_bus_publish(r->bus, EV_MEMORY, "reasoning", p);
    }
    *out = xstrdup(input);
    return 0;
}

reasoning *reasoning_new(const reasoning_config *cfg) {
    if (!cfg || !cfg->llm || !cfg->tools)
        return NULL;
    reasoning *r = calloc(1, sizeof(reasoning));
    if (!r)
        return NULL;
    mutex_init(&r->sess_mtx);
    r->cur = session_get(r, NULL); /* default session, always present */
    r->llm = cfg->llm;
    r->tools = cfg->tools;
    r->mem = cfg->memory;
    r->policy = cfg->policy;
    r->snap = cfg->snapshot;
    r->bus = cfg->bus;
    r->metrics = cfg->metrics;
    r->workspace = cfg->workspace ? xstrdup(cfg->workspace) : NULL;
    r->use_transaction = cfg->use_transaction;
    r->budget_hot = cfg->budget_hot > 0 ? cfg->budget_hot : HIST_BUDGET;
    r->budget_warm = cfg->budget_warm > 0 ? cfg->budget_warm : 3072;
    r->budget_cold = cfg->budget_cold > 0 ? cfg->budget_cold : 4096;
    r->hyde = cfg->hyde ? 1 : 0;
    r->exec_backend = (cfg->exec_backend && *cfg->exec_backend) ? cfg->exec_backend : NULL;
    r->exec_host = (cfg->exec_host && *cfg->exec_host) ? cfg->exec_host : NULL;
    r->skills = cfg->skills;
    r->mcp = cfg->mcp;
    r->index = cfg->index;
    r->plugin_registry = cfg->plugin_registry;
    r->state_root = cfg->state_root ? xstrdup(cfg->state_root) : NULL;
    /* 0 = use default; negative = unlimited (loop guards on round_idx only
     * hitting INT_MAX, so clamp to a practical upper bound) */
    r->max_rounds = cfg->max_rounds != 0 ? cfg->max_rounds : AGENT_LOOP_MAX_ROUNDS;
    if (r->max_rounds < 0)
        r->max_rounds = 1000000;
    r->hooks = cfg->hooks;
    if (r->hooks)
        state_machine_set_hooks(r->sm, r->hooks);
    r->txm = tx_manager_new();
    r->eval = evaluator_new();
    r->attention = attention_new();
    r->sm = state_machine_new();

    state_machine_set_handler(r->sm, ST_REASON, h_reason, r);
    state_machine_set_handler(r->sm, ST_PLAN, h_plan, r);
    state_machine_set_handler(r->sm, ST_ACT, h_act, r);
    state_machine_set_handler(r->sm, ST_VERIFY, h_verify, r);
    state_machine_set_handler(r->sm, ST_LEARN, h_learn, r);
    return r;
}

void reasoning_free(reasoning *r) {
    if (!r)
        return;
    clear_actions(r);
    free(r->last_prompt);
    free(r->workspace);
    free(r->state_root);
    for (size_t i = 0; i < r->nsessions; i++)
        session_free(r->sessions[i]);
    free(r->sessions);
    mutex_destroy(&r->sess_mtx);
    free(r->last_plan_raw);
    free(r->prev_plan);
    free(r->round_log);
    attention_free(r->attention);
    state_machine_free(r->sm);
    evaluator_free(r->eval);
    tx_manager_free(r->txm);
    free(r);
}

void reasoning_set_llm(reasoning *r, llm *llm) {
    if (!r || !llm)
        return;
    r->llm = llm;
}

/* Optional: route each run through the multi-provider router (weighted
 * round-robin). Pass NULL to revert to the single configured LLM. */
void reasoning_set_router(reasoning *r, router *router) {
    if (!r)
        return;
    r->router = router;
}

/* Threshold-triggered compaction: summarize the oldest `n_drop` turns with a
 * structured 9-section LLM prompt (Claude Code-style), store the rolling
 * summary, then drop those turns. On LLM failure the turns are kept and the
 * attempt is retried next threshold; 3 consecutive failures trip the breaker. */
static void compact_history(reasoning *r, size_t n_drop) {
    if (!r || r->cur->hist_n == 0)
        return;
    if (n_drop > r->cur->hist_n)
        n_drop = r->cur->hist_n;
    if (n_drop == 0)
        return;

    strbuf tb;
    strbuf_init(&tb);
    for (size_t i = 0; i < n_drop; i++) {
        strbuf_appendf(&tb, "User: %s\nAssistant: %s\n\n", r->cur->hist_q[i] ? r->cur->hist_q[i] : "",
                           r->cur->hist_a[i] ? r->cur->hist_a[i] : "");
    }
    char *turns = strbuf_detach(&tb);

    strbuf pb;
    strbuf_init(&pb);
    strbuf_append(&pb, "将以下早期对话压缩为结构化纪要，严格按以下 9 个小节输出（Markdown，"
                           "每节 1-4 行，没有内容的写「无」）：\n"
                           "1. 用户核心意图\n2. 技术概念与术语\n3. 涉及文件与代码\n4. 错误与修复\n"
                           "5. 用户全部消息要点\n6. 已完成事项\n7. 未完成待办\n8. 当前工作状态\n"
                           "9. 下一步建议\n"
                           "总长度不超过 2000 字，只输出纪要本身，不要任何前言。\n\n## 待压缩对话\n");
    strbuf_append(&pb, turns ? turns : "");
    free(turns);
    char *user_prompt = strbuf_detach(&pb);

    char *sum = llm_chat_simple(r->llm, "你是会话压缩器。输出简体中文 Markdown 纪要。", user_prompt);
    free(user_prompt);
    if (sum && *sum) {
        free(r->cur->summary);
        r->cur->summary = str_head(sum, COMPACT_SUMMARY_CAP);
        if (!r->cur->summary)
            r->cur->summary = xstrdup(sum);
        r->cur->compact_fails = 0;
        log_info("reasoning: compacted %zu turns into a %zu-char summary", n_drop, strlen(r->cur->summary));
    } else {
        free(sum);
        r->cur->compact_fails++;
        if (r->cur->compact_fails >= 3)
            r->cur->compact_disabled = 1; /* circuit breaker */
        log_warn("reasoning: compaction LLM call failed (%d consecutive)", r->cur->compact_fails);
        return; /* keep the turns; retry at the next threshold */
    }
    for (size_t i = 0; i < n_drop; i++) {
        free(r->cur->hist_q[i]);
        free(r->cur->hist_a[i]);
    }
    memmove(r->cur->hist_q, r->cur->hist_q + n_drop, (r->cur->hist_n - n_drop) * sizeof(char *));
    memmove(r->cur->hist_a, r->cur->hist_a + n_drop, (r->cur->hist_n - n_drop) * sizeof(char *));
    r->cur->hist_n -= n_drop;
}

/* Append a completed turn to the bounded multi-turn history.
 * Called from the run path; also read from the HTTP thread by
 * reasoning_history_json, hence the hist_mtx guard. compact_history is
 * only ever invoked from here with the lock held (do not lock inside it). */
/* Heuristic: the model narrated what it is about to do ("Let me check…",
 * "我需要先…") instead of emitting a JSON action array or a real answer.
 * Short intent-sounding text on the first round is a premature loop stop —
 * the model meant to act. NOTE: no length cap — chatty reasoning models
 * (e.g. GLM) emit multi-KB deliberation prose that is still narration, not
 * an answer; a nudge that wrongly fires on a real answer costs one round
 * (the bound re-accepts the text), while missing narration ends the run. */
static int looks_like_intent(const char *text) {
    if (!text)
        return 0;
    static const char *const marks[] = {
        "I need to",
        "Let me",
        "I will",
        "I'll",
        "First,",
        "First ",
        "I'm going to",
        "Let's",
        "I should",
        "Hmm",
        "I recall",
        "Wait,",
        "\xe6\x88\x91\xe9\x9c\x80\xe8\xa6\x81", /* 我需要 */
        "\xe8\xae\xa9\xe6\x88\x91",             /* 让我   */
        "\xe6\x88\x91\xe5\xb0\x86",             /* 我将   */
        "\xe6\x88\x91\xe5\x85\x88",             /* 我先   */
        "\xe7\xac\xac\xe4\xb8\x80\xe6\xad\xa5", /* 第一步 */
    };
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
        if (strstr(text, marks[i]))
            return 1;
    return 0;
}

static void record_turn(reasoning *r, const char *q, const char *a) {
    if (!r || !q || !a)
        return;
    mutex_lock(&r->cur->mtx);
    if (r->cur->hist_cap == 0) {
        r->cur->hist_cap = 16;
        r->cur->hist_q = calloc(r->cur->hist_cap, sizeof(char *));
        r->cur->hist_a = calloc(r->cur->hist_cap, sizeof(char *));
    }
    if (r->cur->hist_n >= r->cur->hist_cap) {
        free(r->cur->hist_q[0]);
        free(r->cur->hist_a[0]);
        memmove(r->cur->hist_q, r->cur->hist_q + 1, (r->cur->hist_cap - 1) * sizeof(char *));
        memmove(r->cur->hist_a, r->cur->hist_a + 1, (r->cur->hist_cap - 1) * sizeof(char *));
        r->cur->hist_n--;
    }
    r->cur->hist_q[r->cur->hist_n] = str_head(q, HIST_TURN_CAP);
    r->cur->hist_a[r->cur->hist_n] = str_head(a, HIST_TURN_CAP);
    if (!r->cur->hist_q[r->cur->hist_n])
        r->cur->hist_q[r->cur->hist_n] = xstrdup(q);
    if (!r->cur->hist_a[r->cur->hist_n])
        r->cur->hist_a[r->cur->hist_n] = xstrdup(a);
    r->cur->hist_n++;
    /* ring full → compact the oldest half via LLM instead of silent loss */
    if (r->cur->hist_n >= r->cur->hist_cap && !r->cur->compact_disabled && r->llm)
        compact_history(r, r->cur->hist_cap / 2);
    mutex_unlock(&r->cur->mtx);
}

char *reasoning_history_json_ex(reasoning *r, const char *session_id, int max_turns) {
    if (!r)
        return xstrdup("[]");
    if (max_turns <= 0)
        max_turns = 20;
    struct session *s = session_get(r, session_id);
    if (!s)
        return xstrdup("[]");
    mutex_lock(&s->mtx);
    size_t start = (s->hist_n > (size_t)max_turns) ? s->hist_n - (size_t)max_turns : 0;
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = start; i < s->hist_n; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "q", s->hist_q[i] ? s->hist_q[i] : "");
        cJSON_AddStringToObject(t, "a", s->hist_a[i] ? s->hist_a[i] : "");
        cJSON_AddItemToArray(arr, t);
    }
    mutex_unlock(&s->mtx);
    char *sjson = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return sjson ? sjson : xstrdup("[]");
}

char *reasoning_history_json(reasoning *r, int max_turns) {
    return reasoning_history_json_ex(r, NULL, max_turns);
}

/* Sessions listing for the UI: [{id, turns, last_active_ms, task}]. */
char *reasoning_sessions_json(reasoning *r) {
    if (!r)
        return xstrdup("[]");
    mutex_lock(&r->sess_mtx);
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < r->nsessions; i++) {
        struct session *s = r->sessions[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s->id);
        mutex_lock(&s->mtx);
        cJSON_AddNumberToObject(o, "turns", (double)s->hist_n);
        cJSON_AddStringToObject(o, "task", s->sn_task);
        mutex_unlock(&s->mtx);
        cJSON_AddNumberToObject(o, "last_active_ms", (double)s->last_active_ms);
        cJSON_AddItemToArray(arr, o);
    }
    mutex_unlock(&r->sess_mtx);
    char *sjson = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return sjson ? sjson : xstrdup("[]");
}

/* Clear one session's conversation (keeps the session itself). */
int reasoning_session_clear(reasoning *r, const char *session_id) {
    if (!r)
        return -1;
    const char *want = (session_id && *session_id) ? session_id : "default";
    mutex_lock(&r->sess_mtx);
    struct session *s = NULL;
    for (size_t i = 0; i < r->nsessions; i++)
        if (strcmp(r->sessions[i]->id, want) == 0) {
            s = r->sessions[i];
            break;
        }
    mutex_unlock(&r->sess_mtx);
    if (!s)
        return -1;
    mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->hist_n; i++) {
        free(s->hist_q[i]);
        free(s->hist_a[i]);
    }
    s->hist_n = 0;
    free(s->summary);
    s->summary = NULL;
    s->sn_state[0] = s->sn_task[0] = s->sn_files[0] = 0;
    s->sn_errors[0] = s->sn_worklog[0] = 0;
    mutex_unlock(&s->mtx);
    return 0;
}

int reasoning_run(reasoning *r, const char *prompt, char **answer) {
    return reasoning_run_ex(r, NULL, prompt, answer);
}

int reasoning_run_ex(reasoning *r, const char *session_id, const char *prompt, char **answer) {
    if (!r || !prompt)
        return -1;

    /* select (or create) the chat session this run belongs to; the ctx
     * run-lock serializes runs, so swapping r->cur here is race-free */
    r->cur = session_get(r, session_id);
    if (r->cur)
        r->cur->last_active_ms = (long long)time_now_ms();

    /* Ingestion guard: a prompt with invalid UTF-8 (e.g. a non-UTF-8 API
     * client) would poison memory/history and break every later LLM call. */
    char *safe_prompt = str_utf8_sanitize(prompt);
    if (safe_prompt)
        prompt = safe_prompt;

    /* Router: pick a provider for this run (weighted round-robin). */
    llm *picked = NULL;
    llm *saved = NULL;
    if (r->router) {
        const route *rt = router_pick(r->router);
        if (rt) {
            picked = llm_create(rt->provider, rt->base_url, rt->api_key, rt->model);
            if (picked) {
                saved = r->llm;
                r->llm = picked;
            }
        }
    }

    free(r->last_prompt);
    r->last_prompt = xstrdup(prompt);
    r->gen_attempted = 0; /* one auto-generation attempt per run */
    if (r->mem)
        memory_working_push(r->mem, prompt);

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
        int hb = hook_dispatch(r->hooks, "agent.before_run", pj);
        free(pj);
        if (hb == 1) {
            if (r->metrics)
                metrics_inc(r->metrics, "tasks.hook_blocked");
            if (answer)
                *answer = xstrdup("(run blocked by hook)");
            free(safe_prompt);
            return 0;
        }
    }

    /* Bounded agent loop: each round runs the full state machine once. Round
     * results are fed back into the next round's planner context; the loop
     * ends when the LLM stops proposing actions (final text answer) or the
     * round budget is exhausted. */
    round_log_reset(r);
    free(r->last_plan_raw);
    r->last_plan_raw = NULL;
    free(r->prev_plan);
    r->prev_plan = NULL;
    r->stall_nudged = 0;
    r->intent_nudged = 0;

    char *final_text = NULL; /* LLM's plain-text answer (had_plan == 0) */
    char *result = NULL;     /* per-round pipeline output */
    state st = ST_FAILED;
    int stalled = 0;
    for (r->round_idx = 1; r->round_idx <= r->max_rounds; r->round_idx++) {
        free(result);
        result = NULL;
        st = state_machine_run(r->sm, prompt, &result);
        if (st != ST_DONE) {
            /* Stage failure (planner LLM error, VERIFY gate, …): the failed
             * stage's diagnostic is an observation the agent must see, not a
             * reason to die silently. Feed it back and keep looping while
             * round budget remains — the next round can self-correct (e.g.
             * retry with a fixed path). Terminal only when the budget is
             * exhausted, and the report stays in the answer either way. */
            round_log_append(r, result && *result ? result : "(run failed)");
            if (r->round_idx >= r->max_rounds)
                break;
            continue;
        }
        if (!r->had_plan) { /* no actions planned → this is the final answer */
            /* Intent narration ("Let me check the files…") without a single
             * action is a premature stop: the model announced its plan
             * instead of emitting the JSON action array. Narration is NOT
             * confined to round 1, and chatty models narrate repeatedly, so
             * nudge on ANY round — bounded by CONSECUTIVE narration rounds
             * (reset whenever a round executes a plan), not a per-run total:
             * a narration → plan → narration pattern is normal thinking
             * aloud, while the model stuck narrating 4 rounds in a row will
             * not recover. After the bound, the text is accepted as the
             * final answer. */
            if (r->max_rounds > 1 && r->intent_nudged < 4 &&
                r->round_idx < r->max_rounds && looks_like_intent(result)) {
                r->intent_nudged++;
                round_log_append(r, result && *result ? result : "");
                round_log_append(r, "[system] 上一轮只输出了意向说明，没有执行任何工具动作。"
                                    "如果任务还需要操作（读写文件、执行命令、生成文件等），"
                                    "请输出 JSON 动作数组并实际执行；"
                                    "如果任务已经完成或确实无需任何工具，"
                                    "请直接给出最终答案文本。");
                continue;
            }
            final_text = xstrdup(result ? result : "");
            break;
        }
        /* executed a planned round: keep the observation for the next round */
        round_log_append(r, result ? result : "");
        r->intent_nudged = 0; /* narration recovered: reset the consecutive bound */
        /* stall detection: the LLM proposed the exact same plan twice — no
         * progress is possible. Give ONE recovery nudge ("the actions already
         * succeeded; answer from the observations instead of repeating them")
         * before giving up — models often re-emit a successful plan because
         * they lost track of the feedback, not because they are stuck. */
        if (r->prev_plan && r->last_plan_raw && strcmp(r->prev_plan, r->last_plan_raw) == 0) {
            if (!r->stall_nudged && r->round_idx < r->max_rounds) {
                r->stall_nudged = 1;
                free(r->prev_plan);
                r->prev_plan = NULL;
                round_log_append(r, "[system] 连续两轮计划完全相同，但这些动作都已成功执行，结果就在上面。"
                                    "不要重复已执行的动作：如果观察结果足以回答任务，直接用纯文本给出最终答案；"
                                    "否则给出与之前不同的下一步动作。");
                continue;
            }
            stalled = 1;
            break;
        }
    }

    /* Budget exhausted (or stalled, or the last round hit a stage failure —
     * LLM error, failed action on the final round) without a plain-text
     * answer: force one final tool-free LLM call to synthesize the gathered
     * observations, so a big task ends with a real answer instead of a bare
     * transcript. Without this, a stage failure on the LAST round skipped
     * synthesis entirely (the old `st == ST_DONE` guard) and the caller
     * received the raw round log with no answer at all. */
    if (!final_text && r->round_log_len > 0 && r->llm) {
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
            snprintf(user, strlen(prompt) + sizeof(tail) + 64, "任务: %s\n\n已执行动作的观察记录（末段）:\n%s", prompt,
                     tail);
            char *ans = llm_chat_simple(r->llm, sys, user);
            if (ans && *ans) {
                final_text = ans;
                /* a synthesized final answer completes the run even when the
                 * last stage failed: the caller gets a usable result instead
                 * of a failure status with no answer text */
                if (st != ST_DONE)
                    st = ST_DONE;
            } else {
                free(ans);
            }
            free(user);
        }
    }

    /* compose the answer: everything that happened + the final reply */
    strbuf out;
    strbuf_init(&out);
    if (r->round_log_len > 0)
        strbuf_append(&out, r->round_log);
    if (final_text && *final_text) {
        if (r->round_log_len > 0)
            strbuf_append(&out, "\n回答: ");
        strbuf_append(&out, final_text);
    } else if (r->round_log_len > 0) {
        if (stalled)
            strbuf_appendf(&out, "\n(连续两轮计划相同，已停止；任务可能未完全完成，第 %d/%d 轮)", r->round_idx,
                               r->max_rounds);
        else
            strbuf_appendf(&out, "\n(已达到最大轮数 %d，任务可能未完全完成)", r->max_rounds);
    }
    free(final_text);
    free(result);
    free(r->last_plan_raw);
    r->last_plan_raw = NULL;
    free(r->prev_plan);
    r->prev_plan = NULL;
    char *combined = strbuf_detach(&out);

    if (r->metrics)
        metrics_inc(r->metrics, st == ST_DONE ? "tasks.done" : "tasks.failed");

    int ret = -1;
    if (st == ST_DONE) {
        if (r->mem)
            memory_working_push(r->mem, combined);
        record_turn(r, prompt, combined);
        if (r->hooks) {
            cJSON *o = cJSON_CreateObject();
            if (o) {
                cJSON_AddStringToObject(o, "prompt", prompt);
                cJSON_AddStringToObject(o, "status", "done");
                cJSON_AddStringToObject(o, "answer", combined ? combined : "");
                char *pj = cJSON_PrintUnformatted(o);
                hook_dispatch(r->hooks, "agent.after_run", pj);
                free(pj);
                cJSON_Delete(o);
            }
        }
        if (answer)
            *answer = combined;
        else
            free(combined);
        ret = 0;
    } else {
        if (r->hooks) {
            cJSON *o = cJSON_CreateObject();
            if (o) {
                cJSON_AddStringToObject(o, "prompt", prompt);
                cJSON_AddStringToObject(o, "status", "failed");
                char *pj = cJSON_PrintUnformatted(o);
                hook_dispatch(r->hooks, "agent.on_error", pj);
                free(pj);
                cJSON_Delete(o);
            }
        }
        if (answer)
            *answer = combined ? combined : xstrdup("(pipeline failed)");
        else
            free(combined);
        ret = -1;
    }

    if (picked) {
        r->llm = saved;
        llm_destroy(picked);
    }
    free(safe_prompt);
    return ret;
}

/* Session-memory snapshot: fixed-section notes + compaction state (JSON). */
char *reasoning_session_json(reasoning *r) {
    if (!r)
        return xstrdup("{}");
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return xstrdup("{}");
    cJSON_AddStringToObject(o, "task", r->cur->sn_task);
    cJSON_AddStringToObject(o, "state", r->cur->sn_state);
    cJSON_AddStringToObject(o, "files", r->cur->sn_files);
    cJSON_AddStringToObject(o, "errors", r->cur->sn_errors);
    cJSON_AddStringToObject(o, "worklog", r->cur->sn_worklog);
    cJSON_AddStringToObject(o, "summary", r->cur->summary ? r->cur->summary : "");
    cJSON_AddNumberToObject(o, "history_turns", (double)r->cur->hist_n);
    cJSON_AddBoolToObject(o, "compaction_disabled", r->cur->compact_disabled ? 1 : 0);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s ? s : xstrdup("{}");
}

/* HyDE primitive: LLM writes a hypothetical answer passage for `query`; the
 * caller embeds passage-to-passage for retrieval (see reasoning.h). */
char *hyde_passage(llm *llm, const char *query) {
    if (!llm || !query || !*query)
        return NULL;
    char prompt[1200];
    snprintf(prompt, sizeof(prompt),
             "Write a short passage (3-5 sentences) that directly answers the "
             "question. Output only the passage, no preamble.\n\nQuestion: %.900s",
             query);
    return llm_chat_simple(llm,
                               "You generate concise hypothetical answer passages "
                               "used for semantic retrieval.",
                               prompt);
}
