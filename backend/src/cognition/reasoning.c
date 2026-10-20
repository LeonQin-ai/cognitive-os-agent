/* reasoning.c — cognitive reasoning engine.
 * Wires the state machine stages to the LLM, tool registry, transaction/snapshot
 * layer, memory and event bus. The LLM proposes a JSON plan of tool actions;
 * this engine executes them transactionally and records the episode. */
#include "cognition/reasoning.h"
#include "cognition/planner.h"
#include "cognition/evaluator.h"
#include "cognition/attention.h"
#include "retrieval/context_builder.h"
#include "llm/router.h"
#include "runtime/state_machine.h"
#include "runtime/policy_engine.h"
#include "runtime/event_bus.h"
#include "runtime/hook.h"
#include "runtime/scheduler.h"
#include "llm/llm.h"
#include "action/tools.h"
#include "action/skill.h"
#include "plugin_intelligence/generator.h"
#include "memory/memory.h"
#include "memory/service.h"
#include "retrieval/engine.h"
#include "os/os_fs.h"
#include "os/os_thread.h"
#include "os/os_time.h"
#include "tx/tx.h"
#include "execution/executor.h"
#include "infra/util.h"
#include "infra/logging.h"
#include "infra/trace.h"
#include "infra/metrics.h"
#include "security/secret.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdatomic.h>
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
    int loaded; /* persisted chat/<id>.jsonl already merged into the ring */
    /* session metadata (会话状态): persisted via chat/sessions.json index */
    char title[128];      /* display title = head of the first user message */
    long long created_ms; /* session creation time */
    long long total_ms;   /* accumulated execution duration across runs */
    int shared_memory;    /* 1 = share the Memory OS across sessions (default) */
    volatile int in_run;  /* >0 while a lane run executes on this session
                           * (reasoning_session_delete refuses to free it) */
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
    s->created_ms = s->last_active_ms;
    s->shared_memory = 1;
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

/* ---- shared session registry (sess_store) ----
 * One store holds the chat-session registry + the persisted metadata index.
 * A standalone reasoning instance uses its embedded store; parallel lanes
 * attach ONE shared store so every lane sees the same sessions/transcripts
 * (并发聊天：每条 lane 一个 reasoning 实例，会话注册表共享). */
struct sess_meta {
    char id[64];
    char title[128];
    long long created_ms;
    long long total_ms;
    long long last_active_ms;
    int shared_memory;
};

struct sess_store {
    mutex_t sess_mtx;          /* guards sessions + meta below */
    atomic_int shared_memory_global; /* one Memory OS policy across all chat sessions */
    struct session **sessions;
    size_t nsessions, scap;
    struct sess_meta *meta;
    size_t nmeta, metacap;
};

/* One executed action of the current/most recent run (step registry). */
struct run_step {
    unsigned id;
    char tool[48];
    char *args;
    char *out;
    char target[240]; /* file path only; never persist file contents in the task timeline */
    int ok; /* 1 ok, 0 failed, -1 skipped (policy/hook) */
    int ms;
};

/* Find-or-create the session with this id (NULL/"" = default). Registry cap
 * SESSION_MAX; beyond it the default session is reused (no unbounded
 * growth from hostile clients). Callers hold no run in flight. (Defined
 * after struct reasoning — see below.) */

struct reasoning {
    mutex_t progress_mtx;
    char *progress_json;
    unsigned long progress_seq;
    reasoning_observer observer;
    void *observer_ud;
    task *abort_parent; /* cancellation only; never consumes parent messages */
    task *run_task;
    const char *completion_prompt; /* borrowed node instruction, excluding upstream context */
    int64_t trace_task_id; /* Flow nodes have no run_task pointer */
    unsigned applied_updates;
    llm *llm;
    tool_registry *tools;
    memory *mem;
    memory_service *memsvc;
    policy_engine *policy;
    snapshot *snap;
    event_bus *bus;
    metrics *metrics;
    trace *trace;
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
     * PER CHAT SESSION. The session registry + metadata index live in a
     * sess_store (`ss`): embedded by default, or an externally attached
     * shared store so parallel lanes see the same sessions. Each session's
     * own mtx guards its ring; /v1/chat/history reads a ring from the HTTP
     * thread while a run may be in flight. `cur` is the session selected
     * for the current run (set at run entry, stable for the run's duration). */
    sess_store own_store_; /* embedded store (used unless a shared one is attached) */
    sess_store *ss;        /* active store: &own_store_ or an attached shared store */
    struct session *cur;

    /* Claude-Code style step registry for the current/most recent run:
     * executed actions as {tool,args,out,ok,ms}, rendered by the UI as
     * "● Tool(args) ⎿ out-head". ok: 1 ok, 0 failed, -1 skipped. */
    struct run_step *steps;
    int n_steps, steps_cap;
    unsigned step_sequence;

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
    uint64_t last_plan_action_sig, prev_plan_action_sig; /* JSON-semantic plan identity */
    char *round_log;     /* accumulated action results of previous rounds */
    size_t round_log_len, round_log_cap;
    char *obs_log;       /* user-facing executed-action log (no narrations/nudges) */
    size_t obs_log_len, obs_log_cap;
    uint64_t last_failed_action_sig;
    int same_action_failures;
    struct { uint64_t sig; int attempts; char tool[48]; } invalid_args[32];
    int invalid_args_n;
    int run_had_action_failure;
    char unresolved_failed_tools[32][48];
    char unresolved_failed_scopes[32][2048];
    char unresolved_discovered_reads[32][2048];
    int unresolved_failed_tool_count;
    int unresolved_failed_tools_overflow;
    int tool_fail_aborted;
    int run_mutated; /* successful effects retained; independent of UI step cap */
    uint64_t search_seen[512]; /* successful read-only discoveries in this run */
    int search_seen_n;
    uint64_t versioned_seen[512]; /* file pages and directory contents survive opaque tools */
    int versioned_seen_n;
    int round_search_skips;
    /* Independent of the bounded prompt log/cache: folding observations must
     * not make alternating old reads look like newly discovered evidence. */
    uint64_t observation_seen[2048];
    int observation_seen_n, round_new_observations, round_repeat_reads;
    int progress_cycle_warning_active;
    struct { char *path; unsigned long long next; uint64_t version; } pending_reads[128];
    int pending_reads_n;
    struct { char *path; uint64_t version; } completed_reads[128];
    int completed_reads_n;
    int read_tracking_overflow;
    int run_file_reads;
    int run_successful_tools;
    int thinking_mode; /* task-local preference; set by the chat lane */

    /* live run progress for status display (polled via reasoning_progress):
     * run start time, executed tool-call count, tool currently running.
     * Polled from the HTTP thread without a lock — display-grade accuracy. */
    long long prog_started_ms;
    int prog_tool_calls;
    char prog_tool[64];
    long long prog_llm_ms;   /* cumulative planner LLM latency (issue #6) */
    int prog_llm_calls;
    long long prog_tool_ms;  /* cumulative tool execution latency */
    int prog_model_failures; /* consecutive/total request failures this run */
    int visual_request_failed; /* avoid silently repeating expensive image requests */
    int planner_response_truncated;
    int planner_native_end_turn;
    int planner_output_escalated; /* one bounded 32K -> 64K retry after truncation */
    char last_model_error[1024];

    /* per-model token ledger (borrowed; may be NULL). Deltas are computed
     * against the run-start snapshot of llm usage counters so per-round
     * usage is attributed correctly even across llm swaps. */
    usage *usage_acc;
    long long usage_base_in, usage_base_out, usage_base_reason, usage_base_reports;
    long long run_usage_in, run_usage_out, run_usage_reports;
};

static int is_exploratory_tool(const char *name) {
    return !strcmp(name, "grep") || !strcmp(name, "glob") || !strcmp(name, "list_dir");
}

static void normalized_evidence_path(reasoning *r, const char *path, char *out, size_t cap) {
    char full[2048]; path_resolve(full, sizeof(full), r->workspace, path && *path ? path : ".");
    char *absolute = fs_absolute_path(full);
    snprintf(out, cap, "%s", absolute ? absolute : full);
    free(absolute);
    for (char *p = out; *p; ++p) {
        if (*p == '\\') *p = '/';
#ifdef _WIN32
        if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
#endif
    }
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
}

static void failure_scope(reasoning *r, const char *name, const char *args_json, char *out, size_t cap) {
    out[0] = '\0';
    if (!is_exploratory_tool(name) && strcmp(name, "file_read")) return;
    cJSON *args = cJSON_Parse(args_json ? args_json : "{}");
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));
    normalized_evidence_path(r, path, out, cap);
    cJSON_Delete(args);
}

/* A guessed repository root can be wrong. Keep the discovery evidence that
 * relates a missing workspace-relative path to a real nested source path.
 * The entire relative suffix must match, not merely a shared basename, and
 * discovery alone never resolves the failed read: that file must be read. */
static void record_discovered_read_paths(reasoning *r, const char *tool_name, const char *output) {
    if (strcmp(tool_name, "glob") || !output || !r->unresolved_failed_tool_count) return;
    char root[2048]; normalized_evidence_path(r, ".", root, sizeof(root));
    size_t root_len = strlen(root);
    const char *line = output;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *begin = line;
        if (!strncmp(begin, "[glob] ", 7)) begin += 7;
        size_t length = (size_t)(end - begin);
        while (length && (begin[length - 1] == '\r' || begin[length - 1] == ' ')) --length;
        if (length && length < 2048 && (begin[0] == '/' || (length > 2 && begin[1] == ':'))) {
            char path[2048], candidate[2048];
            memcpy(path, begin, length); path[length] = '\0';
            normalized_evidence_path(r, path, candidate, sizeof(candidate));
            size_t candidate_len = strlen(candidate);
            for (int i = 0; i < r->unresolved_failed_tool_count; ++i) {
                const char *failed = r->unresolved_failed_scopes[i];
                if (strcmp(r->unresolved_failed_tools[i], "file_read") ||
                    strncmp(failed, root, root_len) || failed[root_len] != '/') continue;
                const char *relative = failed + root_len + 1;
                size_t suffix_len = strlen(relative);
                if (suffix_len && candidate_len > suffix_len &&
                    candidate[candidate_len - suffix_len - 1] == '/' &&
                    !strcmp(candidate + candidate_len - suffix_len, relative))
                    snprintf(r->unresolved_discovered_reads[i], sizeof(r->unresolved_discovered_reads[i]), "%s", candidate);
            }
        }
        line = *end ? end + 1 : end;
    }
}

/* Track the affected path as well as the tool. A verified replacement read
 * can supersede a failed exploratory lookup in that scope (#161), while a
 * read of another directory cannot verify a failure or a failed command. */
static void note_failed_tool(reasoning *r, const char *tool_name, const char *args_json) {
    if (!r || !tool_name || !*tool_name) return;
    char scope[2048]; failure_scope(r, tool_name, args_json, scope, sizeof(scope));
    for (int i = 0; i < r->unresolved_failed_tool_count; ++i)
        if (!strcmp(r->unresolved_failed_tools[i], tool_name) && !strcmp(r->unresolved_failed_scopes[i], scope)) return;
    if (r->unresolved_failed_tool_count >=
        (int)(sizeof(r->unresolved_failed_tools) / sizeof(r->unresolved_failed_tools[0]))) {
        r->unresolved_failed_tools_overflow = 1;
        return;
    }
    snprintf(r->unresolved_failed_scopes[r->unresolved_failed_tool_count], sizeof(r->unresolved_failed_scopes[0]), "%s", scope);
    r->unresolved_discovered_reads[r->unresolved_failed_tool_count][0] = '\0';
    snprintf(r->unresolved_failed_tools[r->unresolved_failed_tool_count++],
             sizeof(r->unresolved_failed_tools[0]), "%s", tool_name);
}

/* A successful read of include/<suffix>.h verifies a mistaken src/<suffix>.h
 * probe in the same repository. A basename-only match is never evidence. */
static int same_repository_header(const char *failed, const char *actual) {
    const char *src = strstr(failed,"/src/");
    const char *inc = strstr(actual,"/include/");
    if (!src || !inc || src-failed != inc-actual || strncmp(failed,actual,(size_t)(src-failed))) return 0;
    const char *tail=src+5;
    size_t n=strlen(tail);
    if (!((n>2 && !strcmp(tail+n-2,".h")) || (n>4 && !strcmp(tail+n-4,".hpp")))) return 0;
    return !strcmp(tail,inc+9);
}

static void resolve_failed_tool(reasoning *r, const char *tool_name, const char *args_json) {
    if (!r || !tool_name || !*tool_name) return;
    char scope[2048]; failure_scope(r, tool_name, args_json, scope, sizeof(scope));
    for (int i = 0; i < r->unresolved_failed_tool_count;) {
        const char *failed_scope = r->unresolved_failed_scopes[i];
        size_t n = strlen(failed_scope);
        int retry = !strcmp(r->unresolved_failed_tools[i], tool_name) && !strcmp(failed_scope, scope);
        int replacement = !strcmp(tool_name, "file_read") && is_exploratory_tool(r->unresolved_failed_tools[i]) &&
            n > 0 && !strncmp(scope, failed_scope, n) && (!scope[n] || scope[n] == '/' || failed_scope[n-1] == '/');
        int relocated_read = !strcmp(tool_name, "file_read") && !strcmp(r->unresolved_failed_tools[i], "file_read") &&
            r->unresolved_discovered_reads[i][0] && !strcmp(r->unresolved_discovered_reads[i], scope);
        int header_relocated = !strcmp(tool_name,"file_read") && !strcmp(r->unresolved_failed_tools[i],"file_read") &&
            same_repository_header(failed_scope,scope);
        if (retry || replacement || relocated_read || header_relocated) {
            --r->unresolved_failed_tool_count;
            if (i < r->unresolved_failed_tool_count) {
                memmove(r->unresolved_failed_tools[i], r->unresolved_failed_tools[i + 1],
                        (size_t)(r->unresolved_failed_tool_count - i) * sizeof(r->unresolved_failed_tools[0]));
                memmove(r->unresolved_failed_scopes[i], r->unresolved_failed_scopes[i + 1],
                        (size_t)(r->unresolved_failed_tool_count - i) * sizeof(r->unresolved_failed_scopes[0]));
                memmove(r->unresolved_discovered_reads[i], r->unresolved_discovered_reads[i + 1],
                        (size_t)(r->unresolved_failed_tool_count - i) * sizeof(r->unresolved_discovered_reads[0]));
            }
            memset(r->unresolved_failed_tools[r->unresolved_failed_tool_count], 0,
                   sizeof(r->unresolved_failed_tools[0]));
        } else {
            ++i;
        }
    }
}

/* Only the worker constructs snapshots. Readers copy immutable JSON. */
static void usage_book_round(reasoning *r) {
        /* per-round token accounting into the global per-model ledger
         * (deltas vs the previous snapshot; a mid-run llm swap would produce
         * a bogus negative delta, hence the guard) */
        if (r->usage_acc && r->llm) {
            long long ti = 0, to = 0, tr = 0;
            long long reports = 0;
            llm_usage_thread_totals(&ti, &to, &tr, &reports);
            if (reports > r->usage_base_reports) {
                long long rd = tr - r->usage_base_reason;
                if (rd < 0)
                    rd = 0;
                /* visible completion = total completion minus invisible
                 * thinking tokens (issue #7) */
                long long vd = (to - r->usage_base_out) - rd;
                usage_add_ex(r->usage_acc, r->llm->model ? r->llm->model : "?",
                             (long)(ti - r->usage_base_in), (long)(vd > 0 ? vd : 0),
                             (long)rd);
            }
            r->usage_base_in = ti;
            r->usage_base_out = to;
            r->usage_base_reason = tr;
            r->usage_base_reports = reports;
        }
}

static void progress_emit(reasoning *r, const char *stage) {
    char activity[256];
    const char *model = (r && r->llm && r->llm->model && *r->llm->model) ? r->llm->model : "未配置模型";
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    if (strcmp(stage, "analyzing") == 0)
        snprintf(activity, sizeof(activity), "正在分析任务目标和执行范围");
    else if (strcmp(stage, "planning") == 0)
        snprintf(activity, sizeof(activity), "正在等待模型 %s 返回第 %d 次规划结果", model, r->prog_llm_calls + 1);
    else if (strcmp(stage, "plan_ready") == 0)
        snprintf(activity, sizeof(activity), "模型已返回计划，准备执行 %d 个动作", r->n_actions);
    else if (strcmp(stage, "response_ready") == 0)
        snprintf(activity, sizeof(activity), "模型已返回答复，正在整理结果");
    else if (strcmp(stage, "reconnecting") == 0)
        snprintf(activity, sizeof(activity), "模型请求第 %d 次失败，正在重试", r->prog_model_failures);
    else if (strcmp(stage, "recovering") == 0)
        snprintf(activity, sizeof(activity), "工具执行失败，正在分析错误并调整下一步");
    else if (strcmp(stage, "rolled_back") == 0)
        snprintf(activity, sizeof(activity), "本轮操作失败，事务内的文件修改已回滚");
    else if (strcmp(stage, "preparing_context") == 0)
        snprintf(activity, sizeof(activity), "正在整理会话上下文、已完成工作和可用工具");
    else if (strcmp(stage, "executing") == 0 && r->prog_tool[0])
        snprintf(activity, sizeof(activity), "正在执行工具 %s", r->prog_tool);
    else if (strcmp(stage, "summarizing") == 0)
        snprintf(activity, sizeof(activity), "正在由模型 %s 整理最终结果", model);
    else if (strcmp(stage, "completed") == 0)
        snprintf(activity, sizeof(activity), "执行已完成");
    else if (strcmp(stage, "failed") == 0)
        snprintf(activity, sizeof(activity), "执行失败，正在保留记录");
    else
        snprintf(activity, sizeof(activity), "正在推进任务阶段：%s", stage ? stage : "unknown");
    cJSON_AddStringToObject(o, "stage", stage);
    cJSON_AddStringToObject(o, "activity", activity);
    cJSON_AddStringToObject(o, "model", model);
    if (r->last_model_error[0]) cJSON_AddStringToObject(o,"model_error",r->last_model_error);
    cJSON_AddBoolToObject(o, "thinking", r->thinking_mode);
    cJSON_AddNumberToObject(o, "seq", ++r->progress_seq);
    cJSON_AddNumberToObject(o, "applied_updates", r->applied_updates);
    cJSON_AddNumberToObject(o, "started_ms", (double)r->prog_started_ms);
    cJSON_AddNumberToObject(o, "elapsed_ms", (double)(time_now_ms() - r->prog_started_ms));
    cJSON_AddNumberToObject(o, "round", r->round_idx);
    cJSON_AddNumberToObject(o, "tool_calls", r->prog_tool_calls);
    cJSON_AddStringToObject(o, "cur_tool", r->prog_tool);
    cJSON *plan_tools = cJSON_AddArrayToObject(o, "planned_tools");
    for (int i = 0; i < r->n_actions; i++) {
        int repeated = 0;
        for (int j = 0; j < i; j++)
            if (strcmp(r->actions[j].tool, r->actions[i].tool) == 0) { repeated = 1; break; }
        if (!repeated) cJSON_AddItemToArray(plan_tools, cJSON_CreateString(r->actions[i].tool));
    }
    cJSON_AddNumberToObject(o, "llm_ms", (double)r->prog_llm_ms);
    cJSON_AddNumberToObject(o, "tool_ms", (double)r->prog_tool_ms);
    cJSON_AddNumberToObject(o, "llm_calls", r->prog_llm_calls);
    cJSON_AddNumberToObject(o, "model_call", r->prog_llm_calls + (strcmp(stage, "planning") == 0 ? 1 : 0));
    cJSON_AddNumberToObject(o, "model_failures", r->prog_model_failures);
    long long tin = 0, tout = 0;
    long long reports = 0;
    llm_usage_thread_totals(&tin, &tout, NULL, &reports);
    tin -= r->run_usage_in; tout -= r->run_usage_out;
    cJSON_AddBoolToObject(o, "usage_available", reports > r->run_usage_reports);
    cJSON_AddNumberToObject(o, "tokens_in", (double)tin);
    cJSON_AddNumberToObject(o, "tokens_out", (double)tout);
    char *steps = reasoning_steps_json(r);
    cJSON *arr = steps ? cJSON_Parse(steps) : NULL;
    free(steps);
    if (arr) cJSON_AddItemToObject(o, "steps", arr);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return;
    mutex_lock(&r->progress_mtx);
    free(r->progress_json);
    r->progress_json = xstrdup(json);
    mutex_unlock(&r->progress_mtx);
    if (r->observer) r->observer(json, r->observer_ud);
    free(json);
}
void reasoning_set_observer(reasoning *r, reasoning_observer cb, void *ud) {
    if (r) { r->observer = cb; r->observer_ud = ud; }
}
void reasoning_set_thinking_mode(reasoning *r, int enabled) { if (r) r->thinking_mode = enabled != 0; }
void reasoning_set_abort_parent(reasoning *r, task *t) { if (r) r->abort_parent=t; }
void reasoning_set_task(reasoning *r, task *t) { if (r) r->run_task = t; }
void reasoning_set_completion_prompt(reasoning *r, const char *prompt) { if(r) r->completion_prompt=prompt; }
static const char *completion_instruction(reasoning *r, const char *prompt) {
    return r->completion_prompt ? r->completion_prompt:prompt;
}
void reasoning_set_trace_task_id(reasoning *r, int64_t task_id) { if (r) r->trace_task_id = task_id; }
static int run_aborted(reasoning *r) {
    if (r->abort_parent && task_should_abort(r->abort_parent)) return 1;
    if (!r->run_task || !task_should_abort(r->run_task)) return 0;
    if (!r->run_task->cancel_flag) r->run_task->timed_out = 1;
    return 1;
}

/* Find-or-create the session with this id. Caller holds sess_mtx. */
static int tool_abort_requested(void *ud) { return run_aborted((reasoning *)ud); }

static struct session *session_get_locked(reasoning *r, const char *id) {
    const char *want = (id && *id) ? id : "default";
    struct session *s = NULL;

    for (size_t i = 0; i < r->ss->nsessions; i++)
        if (strcmp(r->ss->sessions[i]->id, want) == 0)
            return r->ss->sessions[i];

    if (r->ss->nsessions < SESSION_MAX) {
        s = session_new(want);
        if (s) {
            s->shared_memory = atomic_load(&r->ss->shared_memory_global);
            for (size_t i = 0; i < r->ss->nmeta; i++) {
                const struct sess_meta *m = &r->ss->meta[i];
                if (strcmp(m->id, want) != 0)
                    continue;
                snprintf(s->title, sizeof(s->title), "%s", m->title);
                s->created_ms = m->created_ms;
                s->total_ms = m->total_ms;
                s->last_active_ms = m->last_active_ms;
                break;
            }
            struct session **na = realloc(r->ss->sessions, (r->ss->nsessions + 1) * sizeof(*na));
            if (na) {
                r->ss->sessions = na;
                r->ss->sessions[r->ss->nsessions++] = s;
            } else {
                session_free(s);
                s = NULL;
            }
        }
    }

    if (!s) { /* cap reached or alloc failed: fall back to default */
        for (size_t i = 0; i < r->ss->nsessions; i++)
            if (strcmp(r->ss->sessions[i]->id, "default") == 0) {
                s = r->ss->sessions[i];
                break;
            }
    }

    return s;
}

static struct session *session_get(reasoning *r, const char *id) {
    struct session *s;

    mutex_lock(&r->ss->sess_mtx);
    s = session_get_locked(r, id);
    mutex_unlock(&r->ss->sess_mtx);
    return s;
}

/* ---- per-session chat persistence (survives restarts) ----
 * Each session's turns append to <state_root>/chat/<id>.jsonl; on first
 * access after process start the file is replayed into the in-memory ring,
 * so history survives restarts and switching sessions/panels always has a
 * durable source of truth. */

static void chat_file_path(char *out, size_t n, const char *state_root, const char *session_id) {
    char safe[128];
    char dir[600];
    char fname[160];
    size_t o = 0;

    for (size_t i = 0; session_id && session_id[i] && o < sizeof(safe) - 1; i++) {
        char ch = session_id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
              ch == '_' || ch == '.'))
            ch = '_';
        safe[o++] = ch;
    }
    safe[o] = '\0';
    snprintf(fname, sizeof(fname), "%s.jsonl", safe);
    path_join(dir, sizeof(dir), state_root ? state_root : "state", "chat");
    fs_mkdirs(dir);
    path_join(out, n, dir, fname);
}

/* ---- session metadata index (chat/sessions.json) ---- */

/* 32-bit random value: rand() is only 15 bits on some platforms (Windows),
 * so chain three calls. */
static unsigned session_rnd32(void) {
    unsigned v = (unsigned)rand() & 0x7fffu;

    v = (v << 15) | ((unsigned)rand() & 0x7fffu);
    return (v << 2) | ((unsigned)rand() & 0x3u);
}

/* Random UUID-v4-shaped session id (36 chars). Seeded once per process. */
static void session_uuid(char out[37]) {
    static int seeded = 0;
    unsigned a, b, c, d;

    if (!seeded) {
        srand((unsigned)time(NULL) ^ (unsigned)time_now_ms());
        seeded = 1;
    }
    a = session_rnd32();
    b = session_rnd32();
    c = session_rnd32();
    d = session_rnd32();
    /* UUID-v4 shape: version nibble fixed to 4, variant bits 10x */
    snprintf(out, 37, "%08x-%04x-4%03x-%04x-%04x%08x", a, b & 0xffffu, c & 0xfffu,
             ((c >> 12) & 0x3fffu) | 0x8000u, d & 0xffffu, (a ^ d) & 0xffffffffu);
}

static void meta_index_path(char *out, size_t n, const char *state_root) {
    char dir[600];

    path_join(dir, sizeof(dir), state_root ? state_root : "state", "chat");
    fs_mkdirs(dir);
    path_join(out, n, dir, "sessions.json");
}

/* Caller holds sess_mtx. */
static void meta_save_locked(reasoning *r) {
    cJSON *arr;
    char *js;
    FILE *f;
    char path[700];

    if (!r->state_root)
        return;
    arr = cJSON_CreateArray();
    if (!arr)
        return;
    for (size_t i = 0; i < r->ss->nmeta; i++) {
        struct sess_meta *m = &r->ss->meta[i];
        cJSON *o = cJSON_CreateObject();
        if (!o)
            break;
        cJSON_AddStringToObject(o, "id", m->id);
        cJSON_AddStringToObject(o, "title", m->title);
        cJSON_AddNumberToObject(o, "created_ms", (double)m->created_ms);
        cJSON_AddNumberToObject(o, "total_ms", (double)m->total_ms);
        cJSON_AddNumberToObject(o, "last_active_ms", (double)m->last_active_ms);
        cJSON_AddBoolToObject(o, "shared_memory", m->shared_memory ? 1 : 0);
        cJSON_AddItemToArray(arr, o);
    }

    js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!js)
        return;
    meta_index_path(path, sizeof(path), r->state_root);
    f = fopen(path, "wb");
    if (f) {
        fputs(js, f);
        fclose(f);
    }
    free(js);
}

/* Copy a live session's metadata into the index (find-or-create).
 * Caller holds sess_mtx. */
static void meta_upsert_locked(reasoning *r, struct session *s) {
    struct sess_meta *m = NULL;

    for (size_t i = 0; i < r->ss->nmeta; i++)
        if (strcmp(r->ss->meta[i].id, s->id) == 0) {
            m = &r->ss->meta[i];
            break;
        }
    if (!m) {
        if (r->ss->nmeta == r->ss->metacap) {
            size_t ncap = r->ss->metacap ? r->ss->metacap * 2 : 16;
            struct sess_meta *nm = realloc(r->ss->meta, ncap * sizeof(*nm));
            if (!nm)
                return;
            r->ss->meta = nm;
            r->ss->metacap = ncap;
        }
        m = &r->ss->meta[r->ss->nmeta++];
        memset(m, 0, sizeof(*m));
        snprintf(m->id, sizeof(m->id), "%s", s->id);
    }
    /* live values win; read s fields under its own lock (try-lock-free: the
     * run path calls this with only sess_mtx held) */
    mutex_lock(&s->mtx);
    snprintf(m->title, sizeof(m->title), "%s", s->title);
    m->created_ms = s->created_ms;
    m->total_ms = s->total_ms;
    m->last_active_ms = s->last_active_ms;
    m->shared_memory = s->shared_memory;
    mutex_unlock(&s->mtx);
}

/* Recover a session title from the first user message ("q") of its persisted
 * transcript. Returns 1 when a title was written into out, 0 otherwise. */
static char *str_head(const char *s, size_t n); /* forward: defined below */

static int title_recover_from_transcript(const char *state_root, const char *id, char *out, size_t cap) {
    char fpath[700];
    FILE *f;
    char line[16384];
    size_t n;
    char *nl;
    cJSON *t;
    cJSON *q;
    char *h;
    int ok = 0;

    if (!state_root || !id || !out || cap == 0)
        return 0;
    out[0] = '\0';
    chat_file_path(fpath, sizeof(fpath), state_root, id);
    f = fopen(fpath, "rb");
    if (!f)
        return 0;
    n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    nl = memchr(line, '\n', n);
    line[nl ? (size_t)(nl - line) : n] = '\0';
    t = cJSON_Parse(line);
    if (!t)
        return 0;
    q = cJSON_GetObjectItemCaseSensitive(t, "q");
    if (cJSON_IsString(q) && q->valuestring && *q->valuestring) {
        h = str_head(q->valuestring, 60); /* same head rule as record_turn */
        if (h) {
            snprintf(out, cap, "%s", h);
            ok = out[0] != '\0';
            free(h);
        }
    }
    cJSON_Delete(t);
    return ok;
}

/* Load the persisted index at startup (call before any run). */
static void meta_load(reasoning *r) {
    char path[700];
    FILE *f;
    char *buf;
    long len;
    cJSON *root, *it;

    if (!r->state_root)
        return;
    {
        char setting_path[700];
        path_join(setting_path, sizeof(setting_path), r->state_root, "chat/memory-sharing.json");
        char *setting = fs_read_file(setting_path);
        cJSON *value = setting ? cJSON_Parse(setting) : NULL;
        cJSON *shared = value ? cJSON_GetObjectItemCaseSensitive(value, "shared") : NULL;
        if (cJSON_IsBool(shared))
            atomic_store(&r->ss->shared_memory_global, cJSON_IsTrue(shared));
        cJSON_Delete(value);
        free(setting);
    }
    meta_index_path(path, sizeof(path), r->state_root);
    f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 512 * 1024) {
        fclose(f);
        return;
    }
    buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';
    root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsArray(root)) {
        if (root)
            cJSON_Delete(root);
        return;
    }
    mutex_lock(&r->ss->sess_mtx);
    cJSON_ArrayForEach(it, root) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *ti = cJSON_GetObjectItemCaseSensitive(it, "title");
        cJSON *cr = cJSON_GetObjectItemCaseSensitive(it, "created_ms");
        cJSON *to = cJSON_GetObjectItemCaseSensitive(it, "total_ms");
        cJSON *la = cJSON_GetObjectItemCaseSensitive(it, "last_active_ms");
        cJSON *sh = cJSON_GetObjectItemCaseSensitive(it, "shared_memory");
        struct sess_meta *m;

        if (!jid || !cJSON_IsString(jid) || !jid->valuestring || !*jid->valuestring)
            continue;
        if (r->ss->nmeta == r->ss->metacap) {
            size_t ncap = r->ss->metacap ? r->ss->metacap * 2 : 16;
            struct sess_meta *nm = realloc(r->ss->meta, ncap * sizeof(*nm));
            if (!nm)
                break;
            r->ss->meta = nm;
            r->ss->metacap = ncap;
        }
        m = &r->ss->meta[r->ss->nmeta++];
        memset(m, 0, sizeof(*m));
        snprintf(m->id, sizeof(m->id), "%s", jid->valuestring);
        if (ti && cJSON_IsString(ti) && ti->valuestring)
            snprintf(m->title, sizeof(m->title), "%s", ti->valuestring);
        if (cr && cJSON_IsNumber(cr))
            m->created_ms = (long long)cr->valuedouble;
        if (to && cJSON_IsNumber(to))
            m->total_ms = (long long)to->valuedouble;
        if (la && cJSON_IsNumber(la))
            m->last_active_ms = (long long)la->valuedouble;
        m->shared_memory = sh ? cJSON_IsTrue(sh) : 1;
    }
    /* self-heal 1: entries with an empty title get it back from the first
     * user message of their persisted transcript (a degraded in-memory index
     * could be saved out with titles wiped — regression guard) */
    int healed = 0;
    for (size_t i = 0; i < r->ss->nmeta; i++) {
        struct sess_meta *m = &r->ss->meta[i];
        if (!m->title[0] && title_recover_from_transcript(r->state_root, m->id, m->title, sizeof(m->title)))
            healed = 1;
    }
    /* self-heal 2: transcripts on disk missing from the index (e.g. the
     * degraded save dropped them) are re-admitted with recovered titles */
    {
        char dir[600];
        path_join(dir, sizeof(dir), r->state_root ? r->state_root : "state", "chat");
        dir_list dl;
        if (fs_list_dir(dir, &dl) == 0) {
            for (size_t i = 0; i < dl.count; i++) {
                if (dl.items[i].is_dir)
                    continue;
                const char *name = dl.items[i].name;
                size_t nl = strlen(name);
                if (nl < 7 || strcmp(name + nl - 6, ".jsonl") != 0)
                    continue;
                char sid[128];
                snprintf(sid, sizeof(sid), "%.*s", (int)(nl - 6), name);
                int seen = 0;
                for (size_t k = 0; k < r->ss->nmeta; k++)
                    if (strcmp(r->ss->meta[k].id, sid) == 0) {
                        seen = 1;
                        break;
                    }
                if (seen)
                    continue;
                if (r->ss->nmeta == r->ss->metacap) {
                    size_t ncap = r->ss->metacap ? r->ss->metacap * 2 : 16;
                    struct sess_meta *nm = realloc(r->ss->meta, ncap * sizeof(*nm));
                    if (!nm)
                        break;
                    r->ss->meta = nm;
                    r->ss->metacap = ncap;
                }
                struct sess_meta *m = &r->ss->meta[r->ss->nmeta++];
                memset(m, 0, sizeof(*m));
                snprintf(m->id, sizeof(m->id), "%s", sid);
                title_recover_from_transcript(r->state_root, m->id, m->title, sizeof(m->title));
                m->shared_memory = 1; /* same default as session_new */
                healed = 1;
            }
        }
    }
    if (healed)
        meta_save_locked(r);
    mutex_unlock(&r->ss->sess_mtx);
    cJSON_Delete(root);
}

/* ---- shared session registry: public sess_store API ---- */

sess_store *sess_store_new(void) {
    sess_store *ss = calloc(1, sizeof(*ss));
    if (!ss)
        return NULL;
    mutex_init(&ss->sess_mtx);
    atomic_init(&ss->shared_memory_global, 1);
    return ss;
}

void sess_store_free(sess_store *ss) {
    if (!ss)
        return;
    for (size_t i = 0; i < ss->nsessions; i++)
        session_free(ss->sessions[i]);
    free(ss->sessions);
    free(ss->meta);
    mutex_destroy(&ss->sess_mtx);
    free(ss);
}

void reasoning_attach_sess_store(reasoning *r, sess_store *ss) {
    if (!r || !ss || ss == &r->own_store_ || r->ss == ss)
        return;
    /* drop any lazily created sessions of the embedded store (attach is
     * documented to happen right after reasoning_new, before any run) */
    mutex_lock(&r->own_store_.sess_mtx);
    for (size_t i = 0; i < r->own_store_.nsessions; i++)
        session_free(r->own_store_.sessions[i]);
    free(r->own_store_.sessions);
    free(r->own_store_.meta);
    r->own_store_.sessions = NULL;
    r->own_store_.nsessions = 0;
    r->own_store_.scap = 0;
    r->own_store_.meta = NULL;
    r->own_store_.nmeta = 0;
    r->own_store_.metacap = 0;
    r->cur = NULL;
    mutex_unlock(&r->own_store_.sess_mtx);
    mutex_destroy(&r->own_store_.sess_mtx);
    r->ss = ss;
    /* meta_load at reasoning_new filled the EMBEDDED store's metadata index,
     * which the wipe above discarded — reload it into the shared store so the
     * 最近 list keeps human-readable titles after a restart (regression:
     * session tabs fell back to raw uuid). First attach wins; later lanes
     * see nmeta > 0 and skip. */
    if (ss->nmeta == 0)
        meta_load(r);
}

/* Replay a session's persisted turns into its ring (caller holds s->mtx).
 * Keeps at most the last CHAT_PERSIST_MAX turns. */
#define CHAT_PERSIST_MAX 200
static void session_load_persisted(reasoning *r, struct session *s) {
    char fpath[700];
    FILE *f;
    char *buf = NULL;
    size_t cap = 0, len = 0;
    int ch;

    if (!r || !s || !r->state_root)
        return;
    chat_file_path(fpath, sizeof(fpath), r->state_root, s->id);
    f = fopen(fpath, "rb");
    if (!f)
        return;
    /* slurp (bounded: 4 MB) */
    cap = 4096;
    buf = (char *)malloc(cap);
    if (!buf) {
        fclose(f);
        return;
    }
    while ((ch = fgetc(f)) != EOF) {
        if (len + 2 > cap - 1) {
            if (cap >= 4u * 1024 * 1024)
                break;
            size_t ncap = cap * 2;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb)
                break;
            buf = nb;
            cap = ncap;
        }
        buf[len++] = (char)ch;
    }
    fclose(f);
    if (!buf) {
        return;
    }
    buf[len] = '\0';

    /* parse line by line; keep only the last CHAT_PERSIST_MAX turns */
    size_t total = 0;
    for (const char *p = buf; *p; p++)
        if (*p == '\n')
            total++;
    size_t skip = total > CHAT_PERSIST_MAX ? total - CHAT_PERSIST_MAX : 0;
    size_t line_no = 0;
    /* manual '\n' split: strtok_r is not declared under strict -std=c11 on
     * glibc (implicit decl truncates the returned pointer) */
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        if (line_no++ < skip) {
            line = nl ? nl + 1 : NULL;
            continue;
        }
        cJSON *t = cJSON_Parse(line);
        if (!t) {
            line = nl ? nl + 1 : NULL;
            continue;
        }
        cJSON *q = cJSON_GetObjectItemCaseSensitive(t, "q");
        cJSON *a = cJSON_GetObjectItemCaseSensitive(t, "a");
        if (cJSON_IsString(q) && q->valuestring) {
            /* grow the ring to admit the loaded turn (no compaction here:
             * history is the durable record, the live cap applies to runs) */
            if (s->hist_n >= s->hist_cap) {
                size_t ncap = s->hist_cap ? s->hist_cap * 2 : 16;
                char **nq = (char **)realloc(s->hist_q, ncap * sizeof(char *));
                char **na = (char **)realloc(s->hist_a, ncap * sizeof(char *));
                if (nq)
                    s->hist_q = nq;
                if (na)
                    s->hist_a = na;
                if (nq && na)
                    s->hist_cap = ncap;
            }
            if (s->hist_n < s->hist_cap) {
                s->hist_q[s->hist_n] = xstrdup(q->valuestring);
                s->hist_a[s->hist_n] = cJSON_IsString(a) && a->valuestring ? xstrdup(a->valuestring) : xstrdup("");
                if (s->hist_q[s->hist_n] && s->hist_a[s->hist_n])
                    s->hist_n++;
            }
        }
        cJSON_Delete(t);
        line = nl ? nl + 1 : NULL;
    }
    free(buf);
}

static void chat_persist_append(reasoning *r, const char *session_id, const char *q, const char *a) {
    char fpath[700];
    FILE *f;
    cJSON *t;
    char *line;

    if (!r || !r->state_root || !q || !a)
        return;
    chat_file_path(fpath, sizeof(fpath), r->state_root, session_id && *session_id ? session_id : "default");
    t = cJSON_CreateObject();
    if (!t)
        return;
    cJSON_AddStringToObject(t, "q", q);
    cJSON_AddStringToObject(t, "a", a);
    line = cJSON_PrintUnformatted(t);
    cJSON_Delete(t);
    if (!line)
        return;
    f = fopen(fpath, "ab");
    if (f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }
    free(line);
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
#define HIST_TURN_CAP 4000       /* per-turn chars kept in history (chat UI
                                  * serves this store verbatim — 500 made
                                  * reloaded answers visibly truncated) */
#define HIST_BUDGET 8192         /* total chars of history injected per run */
#define LEARN_RESULT_CAP 300     /* chars of a result kept as a memory episode */
#define COMPACT_SUMMARY_CAP 2000 /* rolling compaction summary cap */
#define AGENT_LOOP_MAX_ROUNDS                                                                                          \
    -1                      /* default rounds when config does not set it;                                             \
                               config "reasoning.max_rounds" < 0 = unlimited */
#define REASONING_CONSEC_FAIL_ABORT 5 /* consecutive stage failures before aborting the run */
#define REASONING_CONSEC_FAIL_ABORT_STR "5"

/* HTTP 429 is a provider/quota decision, not a transient planning failure.
 * Retrying it in later reasoning rounds cannot help and can burn the user's
 * remaining request budget. Keep this narrow: other failures still follow the
 * normal recovery path. */
static int reasoning_is_rate_limited(const char *error) {
    if (!error || !*error) return 0;
    return strstr(error, "http 429") != NULL || strstr(error, "HTTP 429") != NULL ||
           strstr(error, "status=429") != NULL || strstr(error, "status 429") != NULL;
}
/* Retrying an unchanged transport failure five times can spend 25 minutes
 * waiting on the same 5-minute HTTP deadline without executing another tool.
 * Allow one recovery attempt, then preserve the task for a later resume. */
static int reasoning_is_transport_failure(const char *error) {
    return error && strstr(error, "http request failed") != NULL;
}
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

/* Append text to a growable log buffer, tail-keeping: once past ROUND_LOG_CAP
 * the oldest half is dropped so recent action results always stay available.
 * Returns 1 when old observations were folded away. */
static int log_append(char **buf, size_t *blen, size_t *bcap, const char *text) {
    const char *add = text;
    char *elided = NULL;
    size_t len;
    size_t need;

    if (!text || !*text)
        return 0;
    len = strlen(text);
    if (len > ROUND_LOG_ENTRY_CAP) {
        /* head+tail keep with an explicit elision marker: the model sees
         * the beginning and end of the output, not a silent hole */
        size_t head = ROUND_LOG_HEAD_KEEP;
        size_t tail_start = len - ROUND_LOG_TAIL_KEEP;
        while (head > 0 && !str_utf8_valid_n(text, (long long)head)) head--;
        while (tail_start < len && ((unsigned char)text[tail_start] & 0xc0) == 0x80)
            tail_start++;
        size_t tail = len - tail_start;
        size_t mid = tail_start - head;
        char marker[80];
        int mlen = snprintf(marker, sizeof(marker),
                            "\n...[%zu bytes of output elided]...\n", mid);
        elided = (char *)malloc(head + (size_t)mlen + tail + 1);
        if (elided) {
            memcpy(elided, text, head);
            memcpy(elided + head, marker, (size_t)mlen);
            memcpy(elided + head + mlen, text + tail_start, tail);
            elided[head + (size_t)mlen + tail] = '\0';
            add = elided;
            len = strlen(elided);
        }
    }

    need = *blen + len + 1;
    if (need > *bcap) {
        size_t ncap = *bcap ? *bcap * 2 : 2048;
        while (ncap < need)
            ncap *= 2;
        char *nb = (char *)realloc(*buf, ncap);
        if (!nb) {
            free(elided);
            return 0;
        }
        *buf = nb;
        *bcap = ncap;
    }

    memcpy(*buf + *blen, add, len + 1);
    *blen += len;
    free(elided);
    if (*blen > ROUND_LOG_CAP) {
        size_t half = *blen / 2;
        /* Keep a complete observation when possible and never start with a
         * UTF-8 continuation byte (long Chinese tool outputs hit this path). */
        size_t line_end = half;
        while (line_end < *blen && (*buf)[line_end] != '\n') line_end++;
        if (line_end < *blen) half = line_end + 1;
        else while (half < *blen && ((unsigned char)(*buf)[half] & 0xc0) == 0x80) half++;
        memmove(*buf, *buf + half, *blen - half + 1);
        *blen -= half;
        /* tell the model the log was folded, so it knows earlier results
         * may be gone and can re-read if truly needed */
        static const char fold_note[] =
            "(earlier action results were folded away to fit the budget)\n";
        size_t nlen = strlen(fold_note);
        if (*bcap > nlen + *blen + 1) {
            memmove(*buf + nlen, *buf, *blen + 1);
            memcpy(*buf, fold_note, nlen);
            *blen += nlen;
        }
        return 1;
    }
    return 0;
}

/* Model-facing log: everything the next planning round must see (action
 * results, narrations, system nudges). */
/* Bound individual tool observations, not an entire batch of file reads. */
static int log_append_entries(reasoning *r, char **buf, size_t *len, size_t *cap, const char *text) {
    int folded = 0;
    const char *start = text;
    if (!text) return 0;
    for (const char *p = text; *p; ++p) {
        if (p == text || p[-1] != '\n' || *p != '[') continue;
        const char *end = strchr(p, ']');
        if (!end || end-p > 48) continue;
        char name[49];
        memcpy(name, p+1, (size_t)(end-p-1)); name[end-p-1] = 0;
        if (!tool_find(r->tools, name)) continue;
        size_t n = (size_t)(p-start);
        char *entry = malloc(n+1);
        if (!entry) break;
        memcpy(entry, start, n); entry[n] = 0;
        folded |= log_append(buf, len, cap, entry);
        free(entry); start = p;
    }
    return folded | log_append(buf, len, cap, start);
}

static void pending_reads_clear(reasoning *r) {
    for (int i=0; i<r->pending_reads_n; ++i) free(r->pending_reads[i].path);
    for (int i=0; i<r->completed_reads_n; ++i) free(r->completed_reads[i].path);
    r->completed_reads_n = 0;
    r->pending_reads_n = r->read_tracking_overflow = r->run_file_reads = 0;
}

static void record_read_page(reasoning *r, const char *args_json, const char *output) {
    cJSON *args = cJSON_Parse(args_json);
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));
    if (!path) { cJSON_Delete(args); return; }
    char full[2048]; normalized_evidence_path(r, path, full, sizeof(full));
    if (fs_is_dir(full)) { cJSON_Delete(args); return; }
    r->run_file_reads++;
    uint64_t version=fs_file_version(full);
    for(int done=0;done<r->completed_reads_n;done++) {
        if(strcmp(r->completed_reads[done].path,full)) continue;
        /* Reinspection of an already-covered version cannot reopen a hole. */
        if(version && r->completed_reads[done].version==version) { cJSON_Delete(args); return; }
        free(r->completed_reads[done].path);
        r->completed_reads[done]=r->completed_reads[--r->completed_reads_n];
        break;
    }
    int i=0;
    while (i<r->pending_reads_n && strcmp(r->pending_reads[i].path, full)) ++i;
    const char *header = output ? strstr(output, "[bytes ") : NULL;
    unsigned long long first=0, last=0, next=0;
    int paged = header && sscanf(header, "[bytes %llu-%llu", &first, &last) == 2;
    const char *marker = paged ? strstr(header, "next_offset_bytes=") : NULL;
    if (marker) sscanf(marker, "next_offset_bytes=%llu", &next);
    int more = paged && strstr(header, "more available") != NULL;
    if (!paged && i == r->pending_reads_n) { cJSON_Delete(args); return; }
    if (i == r->pending_reads_n) {
        if (i == 128 || !(r->pending_reads[i].path = xstrdup(full))) {
            r->read_tracking_overflow = 1; cJSON_Delete(args); return;
        }
        r->pending_reads[i].next = 0;
        r->pending_reads[i].version = version; r->pending_reads_n++;
    }
    if(r->pending_reads[i].version!=version) {
        r->pending_reads[i].version=version;
        r->pending_reads[i].next=0;
    }
    if (!paged || first <= r->pending_reads[i].next) {
        if (!more) {
            if(r->completed_reads_n<128) {
                char *saved=xstrdup(full);
                if(saved) {
                    int done=r->completed_reads_n++;
                    r->completed_reads[done].path=saved;
                    r->completed_reads[done].version=version;
                }
            }
            free(r->pending_reads[i].path);
            memmove(r->pending_reads+i, r->pending_reads+i+1,
                    (size_t)(--r->pending_reads_n-i)*sizeof(r->pending_reads[0]));
        } else if (next > r->pending_reads[i].next) r->pending_reads[i].next = next;
    }
    cJSON_Delete(args);
}

static void round_log_append(reasoning *r, const char *text) {
    mutex_lock(&r->progress_mtx);
    if (log_append_entries(r, &r->round_log, &r->round_log_len, &r->round_log_cap, text))
        r->search_seen_n = r->versioned_seen_n = 0; /* folded observations can be read again */
    mutex_unlock(&r->progress_mtx);
}

/* User-facing observation log: only rounds that actually executed something
 * (or failed to). Narration rounds and system nudges are prompt bookkeeping
 * and must NOT leak into the final answer (GitHub issue #4: a simple "你好"
 * ended up as 4× repeated narration + nudge spam). */
static void obs_log_append(reasoning *r, const char *text) {
    log_append_entries(r, &r->obs_log, &r->obs_log_len, &r->obs_log_cap, text);
}

/* file_read defaults omitted fields, and providers vary JSON key order. Hash
 * the effective page identity so a repeated read of the same page is caught
 * while later offsets remain valid progress. */
static uint64_t file_read_signature(reasoning *r, const char *args_json) {
    cJSON *args = cJSON_Parse(args_json ? args_json : "{}");
    cJSON *pj = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
    cJSON *oj = args ? cJSON_GetObjectItemCaseSensitive(args, "offset_bytes") : NULL;
    cJSON *sj = args ? cJSON_GetObjectItemCaseSensitive(args, "max_bytes") : NULL;
    char resolved[2048];
    uint64_t offset = cJSON_IsNumber(oj) && oj->valuedouble >= 0 ? (uint64_t)oj->valuedouble : 0;
    size_t page = cJSON_IsNumber(sj) && sj->valuedouble >= 4 ? (size_t)sj->valuedouble : 4000;
    if (page > 4000) page = 4000;
    if (cJSON_IsString(pj) && pj->valuestring) {
        path_resolve(resolved, sizeof(resolved), r->workspace, pj->valuestring);
        char *absolute = fs_absolute_path(resolved);
        char normalized[2048];
        normalized_evidence_path(r, absolute ? absolute : resolved, normalized, sizeof(normalized));
        const char *identity = normalized;
        uint64_t version = fs_file_version(identity);
        strbuf key; strbuf_init(&key);
        strbuf_appendf(&key, "file_read|%s|%llu|%zu|%llu", identity,
                       (unsigned long long)offset, page, (unsigned long long)version);
        uint64_t sig = hash64(key.buf ? key.buf : "file_read", key.len);
        free(absolute); strbuf_free(&key); cJSON_Delete(args);
        return sig;
    }
    cJSON_Delete(args);
    return hash64(args_json ? args_json : "file_read", args_json ? strlen(args_json) : 9);
}

/* Directory identity depends on its entries, not on unrelated shell calls.
 * Content edits do not change a directory listing; create/delete/rename does. */
static uint64_t directory_signature(reasoning *r, const char *args_json) {
    cJSON *a=cJSON_Parse(args_json ? args_json : "{}");
    cJSON *p=a ? cJSON_GetObjectItemCaseSensitive(a,"path") : NULL;
    char path[2048];
    normalized_evidence_path(r,cJSON_IsString(p) ? p->valuestring : ".",path,sizeof(path));
    dir_list entries={0};
    uint64_t sig=hash64(path,strlen(path));
    if(fs_list_dir(path,&entries)!=0) { cJSON_Delete(a); return 0; }
    uint64_t contents=0;
    for(size_t i=0;i<entries.count;i++)
        contents += hash64(entries.items[i].name,strlen(entries.items[i].name)) ^
                    ((uint64_t)entries.items[i].is_dir * UINT64_C(0x9e3779b185ebca87));
    sig ^= contents ^ ((uint64_t)entries.count * UINT64_C(0x517cc1b727220a95));
    fs_list_free(&entries); cJSON_Delete(a);
    return sig;
}

static void round_log_reset(reasoning *r) {
    mutex_lock(&r->progress_mtx);
    if (r->round_log)
        r->round_log[0] = '\0';
    r->round_log_len = 0;
    if (r->obs_log)
        r->obs_log[0] = '\0';
    r->obs_log_len = 0;
    mutex_unlock(&r->progress_mtx);
}

/* session notes: append "line\n" to a fixed-size buffer, keeping the TAIL
 * (oldest lines are dropped from the front when the cap would be exceeded). */
static void sn_append_line(char *dst, size_t cap, const char *line) {
    size_t cur;
    size_t add;
    size_t room;
    size_t keep;

    if (!dst || !line || !*line)
        return;
    cur = strlen(dst);
    add = strlen(line) + 1; /* line chars + '\n' */
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

    room = cap - 1 - cur;
    if (room < 2)
        return;
    if (add > room)
        add = room;
    keep = add - 1;
    while (keep > 0 && ((unsigned char)line[keep] & 0xC0) == 0x80)
        keep--; /* utf-8 boundary */
    memcpy(dst + cur, line, keep);
    dst[cur + keep] = '\n';
    dst[cur + keep + 1] = '\0';
}

/* Track a file touched by a file_* action (extract "path" from its args). */
static void sn_note_file(reasoning *r, const char *args_json) {
    cJSON *o;
    cJSON *p;

    if (!args_json || !*args_json)
        return;
    o = cJSON_Parse(args_json);
    if (!o)
        return;
    p = cJSON_GetObjectItemCaseSensitive(o, "path");
    if (p && cJSON_IsString(p) && p->valuestring)
        sn_append_line(r->cur->sn_files, sizeof(r->cur->sn_files), p->valuestring);
    cJSON_Delete(o);
}

static char *str_head(const char *s, size_t cap) {
    size_t n;
    int trunc;
    char *out;

    if (!s)
        return NULL;
    n = strlen(s);
    trunc = n > cap;
    if (trunc)
        n = cap;
    while (trunc && n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--; /* utf-8 boundary */
    out = (char *)malloc(n + 4);
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
static int path_requires_full_read(reasoning *r, const char *prompt, const char *path);
static int required_pending_reads(reasoning *r, const char *prompt);

static char *build_context(reasoning *r, const char *prompt) {
    strbuf b;
    size_t input_budget = planner_input_budget_bytes_with_output(r->llm, r->tools, r->skills,
        r->policy, r->planner_output_escalated ? 65536 : 0);
    size_t warm_used = 0;
    size_t hot_mark;
    size_t cold_mark;

    strbuf_init(&b);

    /* WARM tier: compaction summary + session notes under an explicit budget.
     * Over budget the lowest-value sections shed first:
     * worklog -> errors/files -> task/state only. */
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
    hot_mark = b.len;
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
    cold_mark = b.len;
    if (r->mem && r->attention && atomic_load(&r->ss->shared_memory_global)) {
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

    /* L1 page cache: recover older, still-version-valid source excerpts by
     * relevance when their original tool output falls outside the bounded
     * recent observation window. Scope is the active session only. */
    size_t recent_start = r->round_log_len > input_budget / 2 ? r->round_log_len - input_budget / 2 : 0;
    const char *recent = r->round_log ? r->round_log + recent_start : "";
    char *cached_pages = tool_file_cache_context_excluding(r->cur->id, prompt, input_budget / 3, recent);
    if (cached_pages) {
        strbuf_append(&b, "## Cached file excerpts (session/version validated)\n");
        strbuf_append(&b, cached_pages);
        strbuf_append(&b, "\n");
        free(cached_pages);
    }

    /* agent-loop feedback: results of the rounds executed so far in this run,
     * so the planner can decide "next actions" vs "task complete" */
    if (r->round_log_len > 0) {
        strbuf_appendf(&b, "## 之前轮次的动作结果 (第 %d/%d 轮)\n", r->round_idx - 1, r->max_rounds);
        /* Keep the full run log for the UI, but bound repeated planner input.
         * Otherwise each round resends every prior page and burns tokens. */
        size_t log_budget = input_budget / 2;
        size_t log_start = r->round_log_len > log_budget ? r->round_log_len - log_budget : 0;
        while (log_start < r->round_log_len &&
               (((unsigned char)r->round_log[log_start] & 0xc0) == 0x80)) log_start++;
        if (log_start)
            strbuf_append(&b, "[Earlier action log compressed; full log is retained. Recent observations follow.]\n");
        if (strstr(r->round_log, "(earlier action results were folded away"))
            strbuf_append(&b, "(earlier action results were folded away to fit the budget)\n");
        if (r->progress_cycle_warning_active)
            strbuf_append(&b, "[system] 检测到跨轮次循环：多轮交替读取或检查只返回已有结果，没有新增进展。不要重读相同文件页；请读取缺失的新页、修正失败原因并验证产物，或直接根据已有证据回答。\n");
        strbuf_append(&b, r->round_log + log_start);
        if (r->round_idx >= r->max_rounds)
            strbuf_append(&b, "\n这是最后一轮。不要再调用任何工具，也不要输出 JSON 动作；"
                                  "直接基于以上动作结果用纯文本给出最终答案（说明完成了什么、"
                                  "或还缺什么信息）。\n\n");
        else
            strbuf_append(&b, "\n你是多轮 agent 循环：根据以上动作结果，(a) 任务已完成 → 直接用纯文本回答；"
                                  "(b) 未完成 → 给出下一批 JSON 动作。不要重复已成功的动作。"
                                  "独立且已知参数的读取/检查应合并到同一批动作；后续动作依赖上游结果时再分轮。"
                                  "已知文件路径时直接使用，不要反复列出工作目录；已获取页面只在版本变化或证据缺失时重读。\n\n");
    }

    /* anti-pollution caution: history/notes/retrieved context are reference
     * only — the current request must always be planned and executed fresh */
    if (r->pending_reads_n) {
        int required = required_pending_reads(r, completion_instruction(r, prompt));
        strbuf_append(&b, required ? "## 尚未连续读取到文件末尾 / unread page coverage\n" :
                                    "## 可选片段的续读游标（不是待办事项，不要求读到 EOF）\n");
        int shown = 0;
        for (int i=0; i<r->pending_reads_n && shown<16; ++i) {
            if (required && !path_requires_full_read(r, completion_instruction(r,prompt), r->pending_reads[i].path)) continue;
            strbuf_appendf(&b, "- %s: offset_bytes=%llu; next_offset_bytes=%llu\n", r->pending_reads[i].path,
                           r->pending_reads[i].next, r->pending_reads[i].next);
            shown++;
        }
        if (required > 16)
            strbuf_appendf(&b, "另有 %d 个必读分页文件待续读，先完成以上文件。\n", required-16);
        if (required) strbuf_append(&b, "用户要求全文的文件必须用 file_read 的 offset_bytes 继续到 end of file；只读相关片段的文件和未被要求全文的生成脚本不在此列。\n");
    }
    strbuf_append(&b, "## 重要约束\n"
                          "- 上面的会话摘要、Session notes、Conversation history、Retrieved context 都只是背景参考，"
                          "不代表当前请求已经完成。\n"
                          "- 即使历史记录里出现过相似的任务，也必须针对「Current request」重新规划并实际执行动作，"
                          "不允许凭历史记录直接回答\"已完成\"。\n"
                          "- 回答中声称对文件做过任何改动，必须以本轮实际出现的 [tool] 动作结果为依据；"
                          "没有实际执行过对应动作，就不得声称做过。\n"
                          "- 判断项目是否为 Git 仓库时，必须运行 `git rev-parse --show-toplevel` 或 `git status` 并使用准确路径；"
                          "目录清单可能隐藏 `.git`，不能据此断言仓库不存在。若默认 workspace 不是仓库而请求已提供源码目录，"
                          "应把该目录传给 git 工具的 `dir` 后重新检查；只能报告当前 workspace 不在仓库内，不能外推源码目录不是仓库。\n"
                          "- 严禁伪造执行证据：编译输出、测试结果、命令输出（如 [TEST] ... OK、"
                          "\"17 checks, 0 failed\"、exit code 0 之类）只能来自本轮真实出现的 [shell]/[tool] 结果原文；"
                          "如果命令没有运行过、或运行失败了，必须如实说明失败原因，"
                          "绝不允许在最终回答里编造看似成功的结果。\n\n");

    /* Context MMU accounting: per-tier bytes of this prompt */
    if (r->metrics) {
        metrics_set(r->metrics, "context.bytes_hot", (double)(b.len - hot_mark));
        metrics_set(r->metrics, "context.bytes_warm", (double)warm_used);
        metrics_set(r->metrics, "context.bytes_cold", (double)(b.len - cold_mark));
    }

    /* Drop the oldest background if other tiers already filled the selected
     * model's input allowance. Keep the latest observations, constraints and
     * current request; the complete log remains on disk and in the UI. */
    size_t request_bytes = strlen(prompt) + strlen("## Current request\n");
    if (b.len + request_bytes > input_budget) {
        const char *omitted = "[Older context omitted to fit this model's window.]\n";
        size_t allowance = input_budget > request_bytes ? input_budget - request_bytes : 0;
        size_t keep = allowance > strlen(omitted) ? allowance - strlen(omitted) : 0;
        size_t start = b.len > keep ? b.len - keep : 0;
        while (start < b.len && ((unsigned char)b.buf[start] & 0xc0) == 0x80) start++;
        strbuf tail;
        strbuf_init(&tail);
        if (allowance > strlen(omitted)) strbuf_append(&tail, omitted);
        strbuf_append(&tail, b.buf + start);
        strbuf_free(&b);
        b = tail;
    }
    strbuf_append(&b, "## Current request\n");
    strbuf_append(&b, prompt);
    return strbuf_detach(&b);
}

static uint64_t plan_action_signature(const planned_action *actions, int n_actions);

/* REASON: ask the LLM for a plan (JSON array of actions, or plain text). */
static int h_reason(state_machine *sm, void *ud, const char *input, char **out) {
    reasoning *r = ud;
    char *aug;
    char *raw = NULL;
    char *plan_err = NULL;

    (void)sm;
    clear_actions(r);
    r->had_plan = 0;
    r->ok_actions = 0;
    r->denied_actions = 0;
    r->prog_tool[0] = 0;
    progress_emit(r, "preparing_context");
    aug = build_context(r, input);
    if (!r->llm) {
        free(aug);
        *out = xstrdup("(no LLM provider configured)");
        return 0;
    }

    long long t_llm0 = time_now_ms();
    progress_emit(r, "planning");
    int64_t llm_span = trace_begin_task(r->trace, r->run_task ? r->run_task->id : r->trace_task_id, "llm.plan");
    r->planner_response_truncated=0;
    int output_override = r->planner_output_escalated ? 65536 : 0;
    int rc = planner_plan_turn(r->llm, r->tools, r->skills, r->policy,
                                 aug ? aug : input, &r->actions, &r->n_actions,
                                 &raw, &plan_err, r->run_task ? r->run_task->attachments_json : NULL,
                                 &r->planner_response_truncated, output_override, &r->planner_native_end_turn);
    if (r->planner_response_truncated && !r->planner_output_escalated) {
        r->planner_output_escalated = 1;
        log_info("planner: response hit output limit; escalating subsequent requests to a bounded 64K ceiling");
    }
    r->visual_request_failed = rc == -2;
    trace_end(r->trace, llm_span, rc == 0 && raw ? 1 : -1);
    r->prog_llm_ms += time_now_ms() - t_llm0;
    r->prog_llm_calls++;
    if (rc == 0 && raw)
        progress_emit(r, r->n_actions ? "plan_ready" : "response_ready");
    free(aug);
    if (rc != 0 || !raw) {
        free(raw);
        char *msg = plan_err ? xstrdup(plan_err)
                             : xstrdup("(LLM 调用失败：请先用「测试」按钮验证模型配置 provider/key/base_url/model)");
        free(plan_err);
        char *short_error=str_head(msg,900);
        snprintf(r->last_model_error,sizeof(r->last_model_error),"%s",short_error ? short_error : "LLM request failed");
        free(short_error);
        log_error("reasoning: LLM returned no plan: %s", msg);
        *out = msg; /* surfaced as the task result on FAILED */
        return -1;  /* move to FAILED */
    }

    log_info("reasoning: LLM plan: %s", raw);
    r->last_model_error[0]=0;
    r->had_plan = r->n_actions > 0;
    r->prev_plan_action_sig = r->last_plan_action_sig;
    r->last_plan_action_sig = plan_action_signature(r->actions, r->n_actions);
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
static void utf8_head_copy(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    while (n > 0 && !str_utf8_valid_n(src, (long long)n))
        n--;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Record one executed action in the step registry (Claude-Code style
 * "● Tool(args) ⎿ out-head" display; args/out are truncated heads). */
static void step_strings_free(struct run_step *step) { free(step->args); free(step->out); }
static void run_step_add(reasoning *r, const char *tool, const char *args, const char *out, int ok, int ms) {
    struct run_step *st;

    if (!r)
        return;
    mutex_lock(&r->progress_mtx);
    if (r->n_steps >= 256) {
        step_strings_free(&r->steps[0]);
        memmove(r->steps, r->steps + 1, 255 * sizeof(*r->steps));
        r->n_steps = 255;
    }
    if (r->n_steps == r->steps_cap) {
        int ncap = r->steps_cap ? r->steps_cap * 2 : 16;
        struct run_step *ns = realloc(r->steps, (size_t)ncap * sizeof(*ns));
        if (!ns) { mutex_unlock(&r->progress_mtx); return; }
        r->steps = ns;
        r->steps_cap = ncap;
    }
    st = &r->steps[r->n_steps++];
    memset(st, 0, sizeof(*st));
    st->id = ++r->step_sequence;
    utf8_head_copy(st->tool, sizeof(st->tool), tool ? tool : "?");
    /* Transactions bypass the executor's output sanitizer. Keep persisted
     * observations safe for strict UTF-8 JSON consumers even when a plugin
     * returns malformed bytes. Preserve valid text before applying the cap. */
    char *safe_args=str_utf8_sanitize(args ? args : "");
    char *safe_out=str_utf8_sanitize(out ? out : "");
    st->args = str_head(safe_args ? safe_args : "",65536);
    st->out = str_head(safe_out ? safe_out : "",65536);
    free(safe_args); free(safe_out);
    if (tool && strncmp(tool, "file_", 5) == 0 && args) {
        cJSON *parsed = cJSON_Parse(args);
        cJSON *path = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "path") : NULL;
        if (cJSON_IsString(path) && path->valuestring)
            utf8_head_copy(st->target, sizeof(st->target), path->valuestring);
        cJSON_Delete(parsed);
    }
    st->ok = ok;
    st->ms = ms;
    mutex_unlock(&r->progress_mtx);
    r->prog_tool[0] = 0;
    progress_emit(r, "executing");
}

/* Object key order/whitespace are not progress. Arrays retain their order. */
static uint64_t observation_json_hash(const cJSON *item) {
    if (!item) return 0;
    uint64_t h=(uint64_t)(item->type & 255)+1;
    if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
        const cJSON *child;
        cJSON_ArrayForEach(child,item) {
            uint64_t part=observation_json_hash(child);
            if (cJSON_IsObject(item)) {
                if (child->string) part ^= hash64(child->string,strlen(child->string));
                h += part * UINT64_C(0x9e3779b185ebca87);
            } else h=(h ^ part)*UINT64_C(0x100000001b3);
        }
    } else {
        char *scalar=cJSON_PrintUnformatted(item);
        if (scalar) { h ^= hash64(scalar,strlen(scalar)); free(scalar); }
    }
    return h;
}

/* Plan identity ignores object-key order and JSON whitespace. The LLM may
 * serialize the same tool call differently from one round to the next; raw
 * string comparison lets that formatting variation evade the stall breaker. */
static uint64_t plan_action_signature(const planned_action *actions, int n_actions) {
    if (!actions || n_actions <= 0) return 0;
    uint64_t h = UINT64_C(0xcbf29ce484222325) ^ (uint64_t)n_actions;
    for (int i = 0; i < n_actions; i++) {
        cJSON *args = cJSON_Parse(actions[i].args_json ? actions[i].args_json : "{}");
        uint64_t part = hash64(actions[i].tool ? actions[i].tool : "",
                               actions[i].tool ? strlen(actions[i].tool) : 0);
        if (args) {
            part ^= observation_json_hash(args);
            cJSON_Delete(args);
        } else {
            const char *raw = actions[i].args_json ? actions[i].args_json : "";
            part ^= hash64(raw, strlen(raw));
        }
        /* Preserve action order: two plans that call the same actions in a
         * different sequence may have different side effects. */
        h = (h ^ part) * UINT64_C(0x100000001b3);
    }
    return h ? h : 1;
}

static uint64_t semantic_action_signature(const char *tool, const char *args_json) {
    cJSON *args = cJSON_Parse(args_json ? args_json : "{}");
    uint64_t h = hash64(tool ? tool : "", tool ? strlen(tool) : 0);
    if (args) {
        h ^= observation_json_hash(args);
        cJSON_Delete(args);
    } else {
        const char *raw = args_json ? args_json : "";
        h ^= hash64(raw, strlen(raw));
    }
    return h ? h : 1;
}

static void observe_action_progress(reasoning *r,const planned_action *action,uint64_t output_hash,int is_search) {
    cJSON *args=cJSON_Parse(action->args_json ? action->args_json : "{}");
    /* File identity is the resolved source, not spelling of its path or
     * irrelevant argument fields. Returned byte ranges identify new pages. */
    uint64_t identity=observation_json_hash(args);
    if (is_search) {
        cJSON *pj=cJSON_GetObjectItemCaseSensitive(args,"path");
        if (cJSON_IsString(pj)) {
            char full[2048]; path_resolve(full,sizeof(full),r->workspace,pj->valuestring);
            identity=hash64(full,strlen(full));
        }
    }
    cJSON_Delete(args);
    uint64_t signature=(identity ^ hash64(action->tool,strlen(action->tool)))*UINT64_C(0x100000001b3) ^ output_hash;
    for (int j=0;j<r->observation_seen_n;j++) if (r->observation_seen[j]==signature) {
        if (is_search) r->round_repeat_reads++;
        return;
    }
    int cap=(int)(sizeof(r->observation_seen)/sizeof(r->observation_seen[0]));
    if (r->observation_seen_n==cap) {
        memmove(r->observation_seen,r->observation_seen+1,(size_t)(cap-1)*sizeof(uint64_t));
        r->observation_seen_n--;
    }
    r->observation_seen[r->observation_seen_n++]=signature;
    r->round_new_observations++;
}

static int h_act(state_machine *sm, void *ud, const char *input, char **out) {
    reasoning *r = ud;
    strbuf b;
    tool_ctx tctx;
    tx *tx = NULL;
    executor *exec;
    int round_mutated = 0;

    (void)sm;
    r->all_actions_ok = 1;
    r->round_search_skips = 0;
    r->round_new_observations = r->round_repeat_reads = 0;
    strbuf_init(&b);

    if (r->n_actions == 0) {
        strbuf_append(&b, input);
        *out = strbuf_detach(&b);
        return 0;
    }

    memset(&tctx, 0, sizeof(tctx));
    tctx.reg = r->tools;
    tctx.policy = r->policy;
    tctx.snapshot = r->snap;
    tctx.bus = r->bus;
    tctx.workspace = r->workspace;
    tctx.state_root = r->state_root;
    tctx.session_id = r->cur ? r->cur->id : "default";
    tctx.task_input = r->last_prompt;
    tctx.should_abort = tool_abort_requested;
    tctx.abort_ud = r;
    tctx.metrics = r->metrics;
    tctx.skills = r->skills;
    tctx.mcp = r->mcp;

    if (r->use_transaction && r->snap)
        tx = tx_begin(r->txm, r->snap, r->tools, &tctx);

    /* Execution Runtime: all non-tx actions run behind the executor interface.
     * exec_backend routes shell commands through WSL / ssh by wrapping the
     * local executor (non-shell tools pass through unchanged). */
    exec = tx ? NULL : executor_new_local(r->tools, &tctx, r->snap);
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
        /* A lane holds a pthread mutex: never migrate its coroutine to
         * another worker while the lane is locked. The pool provides fairness. */
        if (run_aborted(r) || task_has_messages(r->run_task)) break;
        snprintf(r->prog_tool, sizeof(r->prog_tool), "%s", r->actions[i].tool);
        progress_emit(r, "executing");
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
                run_step_add(r, r->actions[i].tool, r->actions[i].args_json, NULL, -1, 0);
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
                run_step_add(r, r->actions[i].tool, r->actions[i].args_json, NULL, -1, 0);
                continue;
            }
            free(hp);
        }
        int rc;
        uint64_t action_sig = semantic_action_signature(r->actions[i].tool, r->actions[i].args_json);
        /* Schema failures are deterministic for unchanged arguments. Remember
         * them across unrelated successful actions; return the schema so the
         * model can correct the invocation instead of retrying the same JSON. */
        const tool *planned_tool=tool_find(r->tools,r->actions[i].tool);
        char *arg_error=NULL;
        if(planned_tool && tool_validate_args(planned_tool,r->actions[i].args_json,&arg_error)!=0) {
            int k=0;
            while(k<r->invalid_args_n && r->invalid_args[k].sig!=action_sig) ++k;
            if(k==r->invalid_args_n) {
                if(k==32) { memmove(r->invalid_args,r->invalid_args+1,31*sizeof(r->invalid_args[0])); k=31; }
                else r->invalid_args_n++;
                r->invalid_args[k].sig=action_sig; r->invalid_args[k].attempts=0;
                snprintf(r->invalid_args[k].tool,sizeof(r->invalid_args[k].tool),"%s",r->actions[i].tool);
            }
            int attempts=++r->invalid_args[k].attempts;
            strbuf_appendf(&b,"[system] %s 参数无效：%s。先修正参数再调用；所需 schema: %s\n",
                r->actions[i].tool,arg_error ? arg_error : "invalid",
                planned_tool->json_schema ? planned_tool->json_schema : "{}");
            if(attempts>1) {
                r->all_actions_ok=0; r->run_had_action_failure=1;
                note_failed_tool(r,r->actions[i].tool,r->actions[i].args_json);
                r->round_repeat_reads++;
                run_step_add(r,r->actions[i].tool,r->actions[i].args_json,
                    "repeated invalid arguments suppressed; correct arguments using tool schema",0,0);
                if(attempts>=3) r->tool_fail_aborted=1;
                free(arg_error);
                continue;
            }
        }
        free(arg_error);
        int is_search = strcmp(r->actions[i].tool, "glob") == 0 ||
                        strcmp(r->actions[i].tool, "grep") == 0 ||
                        strcmp(r->actions[i].tool, "file_read") == 0 ||
                        strcmp(r->actions[i].tool, "list_dir") == 0;
        if (is_search) {
            int repeated = 0;
            int versioned = !strcmp(r->actions[i].tool,"file_read") || !strcmp(r->actions[i].tool,"list_dir");
            uint64_t search_sig = !strcmp(r->actions[i].tool,"file_read")
                ? file_read_signature(r,r->actions[i].args_json)
                : !strcmp(r->actions[i].tool,"list_dir") ? directory_signature(r,r->actions[i].args_json) : action_sig;
            uint64_t *seen=versioned ? r->versioned_seen : r->search_seen;
            int count=versioned ? r->versioned_seen_n : r->search_seen_n;
            for (int j = 0; search_sig && j < count && !repeated; j++)
                repeated = seen[j] == search_sig;
            if (repeated) {
                r->round_search_skips++;
                r->round_repeat_reads++;
                r->ok_actions++; /* cached observation satisfies verification */
                resolve_failed_tool(r,r->actions[i].tool,r->actions[i].args_json);
                for(int k=0;k<r->invalid_args_n;k++)
                    if(!strcmp(r->invalid_args[k].tool,r->actions[i].tool)) r->invalid_args[k].attempts=0;
                strbuf_appendf(&b, "[%s] identical read/search already completed; use its earlier observation, "
                                 "continue with unread files or the next offset_bytes; this is not task completion\n", r->actions[i].tool);
                run_step_add(r, r->actions[i].tool, r->actions[i].args_json,
                             "previous search result reused", 1, 0);
                continue;
            }
        } else if (!is_search) {
            /* A write or opaque tool could change the files being searched. */
            r->search_seen_n = 0;
        }
        r->prog_tool_calls++;
        long long t_tool0 = time_now_ms();
        char span_name[64] = "tool.";
        size_t span_len = strlen(span_name);
        const unsigned char *tool_name = (const unsigned char *)r->actions[i].tool;
        while (tool_name && *tool_name && span_len + 1 < sizeof(span_name)) {
            unsigned char c = *tool_name++;
            span_name[span_len++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                     (c >= '0' && c <= '9') || c == '_' || c == '-') ? (char)c : '_';
        }
        span_name[span_len] = '\0';
        int64_t tool_span = trace_begin_task(r->trace, r->run_task ? r->run_task->id : r->trace_task_id,
                                             span_name);
        char step_out[240] = ""; /* output head for the step registry */
        char *full_step_out = NULL;
        uint64_t output_hash=0;
        if (tx) {
            /* per-action output = the chunk tx_run appends to the aggregate */
            size_t out_before = tx_output(tx) ? strlen(tx_output(tx)) : 0;
            rc = tx_run(tx, r->actions[i].tool, r->actions[i].args_json);
            const char *agg = tx_output(tx);
            if (agg && strlen(agg)>out_before) output_hash=hash64(agg+out_before,strlen(agg+out_before));
            if (agg && strlen(agg) > out_before)
                snprintf(step_out, sizeof(step_out), "%s", agg + out_before);
            if (agg && strlen(agg) > out_before) full_step_out = str_head(agg+out_before,65536);
        } else if (exec) {
            executor_result *er = NULL;
            int erc = executor_execute(exec, r->actions[i].tool, r->actions[i].args_json, &er);
            rc = (erc == 0 && er && er->ok) ? 0 : -1;
            if (er) {
                output_hash=hash64(er->output ? er->output : "",er->output ? strlen(er->output) : 0);
                /* executor output is already UTF-8 sanitized */
                strbuf_appendf(&b, "[%s] %s\n", r->actions[i].tool, er->output ? er->output : "");
                snprintf(step_out, sizeof(step_out), "%s", er->output ? er->output : "");
                full_step_out = str_head(er->output ? er->output : "",65536);
                executor_result_free(er);
            }
        } else {
            rc = -1;
        }
        /* Cache-hit metadata intentionally differs from the first disk read,
         * but that is not new source evidence. Compare the actual file page
         * body so cache warming cannot reset the no-progress circuit breaker. */
        if (!strcmp(r->actions[i].tool, "file_read") && rc == 0) {
            const char *observed = full_step_out ? full_step_out : step_out;
            const char *body = observed;
            const char *range = observed ? strstr(observed, "[bytes ") : NULL;
            const char *cache_tag = range ? strstr(range, "; file_page_cache=") : NULL;
            const char *nl = range ? strchr(range, '\n') : NULL;
            if (nl) body = nl + 1;
            if (range && cache_tag) {
                /* Keep byte range/status in the fingerprint so distinct
                 * pages count as progress; exclude cache HIT/MISS and version
                 * metadata, which do not change the source evidence. */
                uint64_t range_hash = hash64(range, (size_t)(cache_tag - range));
                uint64_t body_hash = hash64(body ? body : "", body ? strlen(body) : 0);
                output_hash = range_hash ^ (body_hash * UINT64_C(0x9e3779b185ebca87));
            } else {
                output_hash = hash64(body ? body : "", body ? strlen(body) : 0);
            }
        }
        trace_end(r->trace, tool_span, rc == 0 ? 1 : -1);
        r->prog_tool_ms += time_now_ms() - t_tool0;
        run_step_add(r, r->actions[i].tool, r->actions[i].args_json, full_step_out ? full_step_out : step_out, rc == 0 ? 1 : 0,
                     (int)(time_now_ms() - t_tool0));
        if (rc == 0) record_discovered_read_paths(r, r->actions[i].tool, full_step_out ? full_step_out : step_out);
        free(full_step_out);
        if (rc != 0) {
            r->all_actions_ok = 0;
            r->run_had_action_failure = 1;
            note_failed_tool(r, r->actions[i].tool, r->actions[i].args_json);
            strbuf_appendf(&b, "[%s] FAILED\n", r->actions[i].tool);
            log_warn("reasoning: action '%s' failed", r->actions[i].tool);
            if (r->last_failed_action_sig == action_sig)
                r->same_action_failures++;
            else {
                r->last_failed_action_sig = action_sig;
                r->same_action_failures = 1;
            }
            if (r->same_action_failures >= 3) {
                strbuf_appendf(&b, "[system] 相同的 %s 调用已连续失败 3 次，停止重试；"
                                   "请报告最后一次真实错误并继续处理不依赖它的步骤。\n",
                               r->actions[i].tool);
                r->tool_fail_aborted = 1;
            }
            if (r->hooks)
                hook_dispatch(r->hooks, "exec.on_failure", r->actions[i].tool);
        } else {
            resolve_failed_tool(r, r->actions[i].tool, r->actions[i].args_json);
            for(int k=0;k<r->invalid_args_n;k++)
                if(!strcmp(r->invalid_args[k].tool,r->actions[i].tool)) r->invalid_args[k].attempts=0;
            r->last_failed_action_sig = 0;
            observe_action_progress(r,&r->actions[i],output_hash,is_search);
            if (strcmp(r->actions[i].tool, "file_read") == 0)
                record_read_page(r, r->actions[i].args_json, step_out);
            r->same_action_failures = 0;
            r->ok_actions++;
            r->run_successful_tools++;
            int instruction_only = 0;
            if (!strcmp(r->actions[i].tool, "skill") && r->skills) {
                cJSON *a = cJSON_Parse(r->actions[i].args_json);
                cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
                const skill *sk = cJSON_IsString(name) ? skill_find(r->skills, name->valuestring) : NULL;
                instruction_only = sk && !strcmp(sk->kind, "prompt");
                cJSON_Delete(a);
            }
            if (!is_search && !instruction_only) round_mutated = 1;
            if (is_search) {
                int versioned = !strcmp(r->actions[i].tool,"file_read") || !strcmp(r->actions[i].tool,"list_dir");
                uint64_t *seen=versioned ? r->versioned_seen : r->search_seen;
                int *count=versioned ? &r->versioned_seen_n : &r->search_seen_n;
                if (*count == 512) { memmove(seen,seen+1,511*sizeof(*seen)); --*count; }
                seen[(*count)++] = !strcmp(r->actions[i].tool,"file_read")
                    ? file_read_signature(r,r->actions[i].args_json)
                    : !strcmp(r->actions[i].tool,"list_dir") ? directory_signature(r,r->actions[i].args_json) : action_sig;
            }
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
                    char *canonical = fs_absolute_path(full);
                    const char *index_path = canonical ? canonical : full;
                    /* Avoid rereading the whole file from disk after each
                     * page read just to populate the code index. Index only
                     * once this session cache contains all pages through EOF. */
                    char *content = !strcmp(r->actions[i].tool, "file_read")
                        ? tool_file_cached_content(r->cur->id, index_path)
                        : fs_read_file(index_path);
                    if (content) {
                        index_add_file(r->index, index_path, content);
                        free(content);
                    }
                    free(canonical);
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
            if (round_mutated) r->run_mutated = 1;
            log_info("reasoning: transaction committed");
        } else {
            tx_rollback(tx);
            log_warn("reasoning: transaction rolled back");
            progress_emit(r, "rolled_back");
        }
        const char *tout = tx_output(tx);
        if (tout && *tout) {
            char *safe = str_utf8_sanitize(tout);
            strbuf_append(&b, safe ? safe : tout);
            free(safe);
        }
        tx_free(tx);
    } else if (round_mutated) r->run_mutated = 1;

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
    int eff_total;

    (void)sm;
    eff_total = r->n_actions - r->denied_actions;
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
    /* Shared memory is committed only from the terminal success path below.
     * A final LLM turn may still be superseded by a steering message. */

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

/* Publish a completed run's distilled episode and action graph in one place.
 * This runs only after task_close_messages() has linearized finalization, so
 * another session can retrieve completed knowledge but never a live plan. */
static void memory_record_completed_run(reasoning *r, const char *answer) {
    char *task_head;
    char *shaped;

    if (!r || !r->mem || !atomic_load(&r->ss->shared_memory_global))
        return;
    shaped = str_head(answer ? answer : "", LEARN_RESULT_CAP);
    if (r->memsvc)
        memory_service_remember(r->memsvc, MEM_EPISODIC,
                                r->last_prompt ? r->last_prompt : "(task)", shaped ? shaped : "");
    else
        memory_record_experience(r->mem, r->last_prompt ? r->last_prompt : "(task)", shaped ? shaped : "");
    free(shaped);
    task_head = str_head(r->last_prompt ? r->last_prompt : "(task)", 80);
    mutex_lock(&r->progress_mtx);
    for (int i = 0; i < r->n_steps; i++) {
        struct run_step *step = &r->steps[i];
        if (step->ok != 1)
            continue;
        memory_record_edge(r->mem, task_head ? task_head : "(task)", step->tool, "used_tool");
        if (strncmp(step->tool, "file_", 5) == 0 && step->target[0])
            memory_record_edge(r->mem, step->tool, step->target, "touched");
    }
    mutex_unlock(&r->progress_mtx);
    free(task_head);
    if (memory_maybe_consolidate(r->mem, 10, 60000) == 1 && r->metrics)
        metrics_inc(r->metrics, "memory.consolidations");
    memory_flush(r->mem);
}

reasoning *reasoning_new(const reasoning_config *cfg) {
    reasoning *r;

    if (!cfg || !cfg->llm || !cfg->tools)
        return NULL;
    r = calloc(1, sizeof(reasoning));
    if (!r)
        return NULL;
    if (mutex_init(&r->progress_mtx) != 0) { free(r); return NULL; }
    r->ss = &r->own_store_; /* embedded store until a shared one is attached */
    r->trace_task_id = -1;
    mutex_init(&r->own_store_.sess_mtx);
    atomic_init(&r->own_store_.shared_memory_global, 1);
    r->cur = session_get(r, NULL); /* default session, always present */
    r->llm = cfg->llm;
    r->tools = cfg->tools;
    r->mem = cfg->memory;
    r->memsvc = cfg->memory_service;
    r->policy = cfg->policy;
    r->snap = cfg->snapshot;
    r->bus = cfg->bus;
    r->metrics = cfg->metrics;
    r->trace = cfg->trace;
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
    meta_load(r); /* session metadata index (chat/sessions.json) */
    /* 0 = use default; negative = unlimited (loop guards on round_idx only
     * hitting INT_MAX, so clamp to a practical upper bound) */
    r->max_rounds = cfg->max_rounds != 0 ? cfg->max_rounds : AGENT_LOOP_MAX_ROUNDS;
    if (r->max_rounds < 0)
        r->max_rounds = 1000000;
    r->hooks = cfg->hooks;
    r->usage_acc = cfg->usage_acc; /* borrowed per-model token ledger */
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
    if (r) pending_reads_clear(r);
    if (!r)
        return;
    clear_actions(r);
    free(r->last_prompt);
    free(r->workspace);
    free(r->state_root);
    if (r->ss == &r->own_store_) {
        /* sessions/meta belong to this instance; an attached shared store is
         * owned by its creator and must NOT be torn down here */
        for (size_t i = 0; i < r->own_store_.nsessions; i++)
            session_free(r->own_store_.sessions[i]);
        free(r->own_store_.sessions);
        free(r->own_store_.meta);
        mutex_destroy(&r->own_store_.sess_mtx);
    }
    for (int i=0;i<r->n_steps;++i) step_strings_free(&r->steps[i]);
    free(r->steps);
    free(r->progress_json);
    mutex_destroy(&r->progress_mtx);
    free(r->last_plan_raw);
    free(r->prev_plan);
    free(r->round_log);
    free(r->obs_log);
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
    strbuf tb;
    char *turns;
    strbuf pb;
    char *user_prompt;
    char *sum;

    if (!r || r->cur->hist_n == 0)
        return;
    if (n_drop > r->cur->hist_n)
        n_drop = r->cur->hist_n;
    if (n_drop == 0)
        return;

    strbuf_init(&tb);
    for (size_t i = 0; i < n_drop; i++) {
        strbuf_appendf(&tb, "User: %s\nAssistant: %s\n\n", r->cur->hist_q[i] ? r->cur->hist_q[i] : "",
                           r->cur->hist_a[i] ? r->cur->hist_a[i] : "");
    }

    turns = strbuf_detach(&tb);

    strbuf_init(&pb);
    strbuf_append(&pb, "将以下早期对话压缩为结构化纪要，严格按以下 9 个小节输出（Markdown，"
                           "每节 1-4 行，没有内容的写「无」）：\n"
                           "1. 用户核心意图\n2. 技术概念与术语\n3. 涉及文件与代码\n4. 错误与修复\n"
                           "5. 用户全部消息要点\n6. 已完成事项\n7. 未完成待办\n8. 当前工作状态\n"
                           "9. 下一步建议\n"
                           "总长度不超过 2000 字，只输出纪要本身，不要任何前言。\n\n## 待压缩对话\n");
    strbuf_append(&pb, turns ? turns : "");
    free(turns);
    user_prompt = strbuf_detach(&pb);

    sum = llm_chat_simple(r->llm, "你是会话压缩器。输出简体中文 Markdown 纪要。", user_prompt);
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
/* Detect a text answer that only announces work instead of reporting its
 * outcome. The caller decides whether existing observations make that safe. */
static int looks_like_intent(const char *text) {
    static const char *const marks[] = {
        "I need to", "Let me", "I will", "I'll", "First,", "First ", "I'm going to",
        "我需要", "让我", "我将", "我先", "我来", "我还需要", "接下来", "让我继续", "继续读取", "第一步"
    };
    for (size_t i = 0; text && i < sizeof(marks) / sizeof(marks[0]); ++i)
        if (strstr(text, marks[i])) return 1;
    return 0;
}

/* Match ASCII case without changing UTF-8 bytes. Return the remaining text. */
static const char *information_prefix(const char *text, const char *prefix) {
    while (*prefix) {
        unsigned char a = (unsigned char)*text, b = (unsigned char)*prefix;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (!a || a != b) return NULL;
        ++text; ++prefix;
    }
    return text;
}

/* A successful directory listing or read is only an observation. It cannot
 * turn a short announcement of the next action into a delivered analysis.
 * A developed answer may contain future suggestions, so only classify an
 * opening, single-paragraph announcement here. */
static int answer_only_announces_work(const char *text) {
    static const char *const prefixes[] = {
        "I need to", "Let me", "I will", "I'll", "First,", "I'm going to",
        "我需要", "让我", "我将", "我先", "我来", "我还需要", "接下来", "继续读取"
    };
    if (!text) return 0;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    if (strlen(text) > 1200 || strstr(text, "\n\n")) return 0;
    if (strstr(text, "结果：") || strstr(text, "结论：") || strstr(text, "发现：") ||
        strstr(text, "结果:") || strstr(text, "结论:")) return 0;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
        if (information_prefix(text, prefixes[i])) return 1;
    /* #175: a brief progress preamble ("I have read most files. Let me
     * read the rest") is still narration, not the requested analysis.
     * Keep this confined to short, single-paragraph text without findings. */
    static const char *const pending[] = {
        "让我读取", "让我继续", "我还需要读取", "让我检查", "让我核对",
        "Let me read", "let me read", "Let me check", "I still need to",
        "需要调用工具", "需要使用工具", "现在需要创建", "直接生成 HTML 文件"
    };
    for (size_t i = 0; i < sizeof(pending) / sizeof(pending[0]); ++i)
        if (strstr(text, pending[i])) return 1;
    return 0;
}

/* #139: only a standalone explanation is exempt from narration recovery.
 * Matching the whole request avoids accepting an appended work instruction,
 * including verbs the read/write guards do not know (e.g. connect/download). */
static int prompt_is_self_explanation(const char *prompt) {
    static const char *const intros[] = {
        "please explain ", "please describe ", "explain ", "describe ",
        "说明一下", "解释一下", "介绍一下", "说明", "解释", "介绍"
    };
    static const char *const subjects[] = {
        "你的操作安全吗", "你的操作是否安全", "你的安全机制", "你的安全措施", "你的安全性",
        "你的权限", "你的能力边界", "你的能力限制", "are your operations safe",
        "your safety mechanisms", "your permissions", "your limitations"
    };
    static const char *const endings[] = { "是什么", "有哪些", "如何", "怎么样" };
    while (*prompt == ' ' || *prompt == '\t' || *prompt == '\r' || *prompt == '\n') ++prompt;
    const char *p = information_prefix(prompt, "请");
    if (p) prompt = p;
    for (size_t i = 0; i < sizeof(intros) / sizeof(intros[0]); ++i) {
        p = information_prefix(prompt, intros[i]);
        if (p) { prompt = p; break; }
    }
    for (size_t i = 0; i < sizeof(subjects) / sizeof(subjects[0]); ++i) {
        p = information_prefix(prompt, subjects[i]);
        if (!p) continue;
        for (size_t j = 0; j < sizeof(endings) / sizeof(endings[0]); ++j) {
            const char *end = information_prefix(p, endings[j]);
            if (end) { p = end; break; }
        }
        while (*p) {
            if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
                *p == '?' || *p == '!' || *p == '.') { ++p; continue; }
            const char *end = information_prefix(p, "？");
            if (!end) end = information_prefix(p, "。");
            if (!end) end = information_prefix(p, "！");
            if (!end) break;
            p = end;
        }
        if (!*p) return 1;
    }
    return 0;
}

/* A direct capability/help/safety question is answerable from the prompt and
 * registered context. Some reasoning models preface a perfectly useful list
 * with "我需要…" or "Let me…"; treating that as an unfinished tool plan made
 * a simple "有哪些工具" question burn five model calls before accepting the
 * same answer. Keep the narration recovery for work requests, but accept the
 * first text response for unmistakably informational questions. */
static int prompt_is_direct_information_request(const char *prompt) {
    static const char *const questions[] = {
        "有什么工具", "有哪些工具", "什么工具", "有什么技能", "有哪些技能", "什么技能",
        "你会什么", "帮助", "help", "what tools", "what skills", "capabilities",
        "为什么", "为啥", "为何", "why "
    };
    static const char *const actions[] = {
        "修复", "修改", "创建", "写入", "执行", "运行", "检查", "分析", "搜索", "查找",
        "安装", "部署", "删除", "fix ", "edit ", "create ", "write ", "run ", "execute ",
        "analyze ", "search ", "install ", "deploy ", "delete "
    };
    if (!prompt)
        return 0;
    const char *agent_task = strstr(prompt, "## 用户任务\n");
    if (agent_task) prompt = agent_task + strlen("## 用户任务\n");
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
        if (strstr(prompt, actions[i]))
            return 0;
    if (prompt_is_self_explanation(prompt)) return 1;
    for (size_t i = 0; i < sizeof(questions) / sizeof(questions[0]); i++)
        if (strstr(prompt, questions[i]))
            return 1;
    return 0;
}

/* Explicit inspection requests require at least one successful observation.
 * The planner's tool-action plan, not prose phrasing, drives continuation. */
static int prompt_requires_observation(const char *prompt) {
    if (!prompt) return 0;
    const char *agent_task = strstr(prompt, "## 用户任务\n");
    if (agent_task) prompt = agent_task + strlen("## 用户任务\n");
    if (prompt_is_direct_information_request(prompt)) return 0;
    static const char *const actions[] = {
        "查看", "看看", "检查", "分析", "诊断", "读取", "检视", "搜索", "查找",
        "inspect ", "review ", "check ", "analyze ", "diagnose ", "read ", "search "
    };
    static const char *const subjects[] = {
        "状态", "系统", "机器", "电脑", "服务", "文件", "目录", "代码", "源码", "项目", "日志", "文档",
        "status", "system", "machine", "computer", "service", "file", "directory", "code", "source", "project", "log", "document", "repository"
    };
    int has_action = 0, has_subject = 0;
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
        if (strstr(prompt, actions[i])) { has_action = 1; break; }
    for (size_t i = 0; i < sizeof(subjects) / sizeof(subjects[0]); ++i)
        if (strstr(prompt, subjects[i])) { has_subject = 1; break; }
    return has_action && has_subject;
}

/* A requested file/code change needs evidence of an action before a text-only
 * answer can close the run. In particular, reading source files is not proof
 * that a requested PPT or copy was created (issue #68). */
static int mutation_verb_negated(const char *prompt, const char *verb) {
    /* Common Chinese prohibitions use the bare prefix: 不修改/不写入.
     * Match only immediately before this verb, never a distant 不 elsewhere. */
    size_t no_len=strlen("不");
    if((size_t)(verb-prompt)>=no_len && !memcmp(verb-no_len,"不",no_len)) return 1;
    const char *clause=prompt;
    for (const char *p=prompt;p<verb;p++) {
        if (*p=='\n' || *p==',' || *p==';' || *p=='.' ||
            !strncmp(p,"，",strlen("，")) || !strncmp(p,"。",strlen("。")) ||
            !strncmp(p,"；",strlen("；"))) clause=p+1;
    }
    const char *negations[]={"不要","不得","禁止","无需","不需要","不允许","不必","勿",
                             "do not ","don't ","never ","without "};
    for(size_t i=0;i<sizeof(negations)/sizeof(negations[0]);i++) {
        const char *n=strstr(clause,negations[i]);
        if(n && n<verb) return 1;
    }
    return 0;
}

static int prompt_requires_mutation(const char *prompt) {
    static const char *const verbs[] = {
        "生成", "制作", "创建", "复制", "写入", "修改", "修复", "保存", "导出",
        "安装", "部署", "删除", "register ", "create ", "generate ", "copy ", "write ",
        "edit ", "fix ", "save ", "export ", "install ", "deploy ", "delete "
    };
    if (!prompt) return 0;
    if (prompt_is_direct_information_request(prompt)) return 0;
    const char *agent_task = strstr(prompt, "## 用户任务\n");
    if (agent_task) prompt = agent_task + strlen("## 用户任务\n");
    if (strstr(prompt, "如何") || strstr(prompt, "怎么") || strstr(prompt, "how to "))
        return 0;
    if ((strstr(prompt,"修复建议") || strstr(prompt,"修复原因") || strstr(prompt,"修复方案")) &&
        !strstr(prompt,"并修复") && !strstr(prompt,"实际修复") && !strstr(prompt,"保存") &&
        !strstr(prompt,"写入") && !strstr(prompt,".md") && !strstr(prompt,".txt")) return 0;
    /* #177: "output an HTML report document" is an artifact request even
     * without the create/write verbs below. Do not exempt it as a chat report. */
    if ((strstr(prompt,"html") || strstr(prompt,"HTML")) &&
        (strstr(prompt,"文档") || strstr(prompt,"文件") || strstr(prompt,"报告"))) {
        const char *output=strstr(prompt,"输出");
        if (output && !mutation_verb_negated(prompt,output)) return 1;
    }
    /* A checklist requested in chat is a text answer, even when phrased as
     * "生成一份…". Keep explicit file/PPT requests under the write guard. */
    if ((strstr(prompt, "清单") || strstr(prompt, "面试题") || strstr(prompt,"报告") || strstr(prompt,"资料")) &&
        !strstr(prompt, "文件") && !strstr(prompt, "PPT") &&
        !strstr(prompt, "ppt") && !strstr(prompt, ".md") &&
        !strstr(prompt, ".txt") && !strstr(prompt, ".pdf") &&
        !strstr(prompt, "保存") && !strstr(prompt, "写入") &&
        !strstr(prompt, "导出") && !strstr(prompt, "复制到") &&
        !strstr(prompt, "修复") && !strstr(prompt, "修改") &&
        !strstr(prompt, "删除") && !strstr(prompt, "部署") &&
        !strstr(prompt, "安装"))
        return 0;
    /* 注册表 is a noun (registry), not an instruction to register a skill. */
    for (const char *p=strstr(prompt,"注册"); p; p=strstr(p+strlen("注册"),"注册"))
        if (strncmp(p+strlen("注册"),"表",strlen("表"))) return 1;
    for (size_t i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++)
        for(const char *v=strstr(prompt,verbs[i]);v;v=strstr(v+strlen(verbs[i]),verbs[i]))
            if(!mutation_verb_negated(prompt,v)) return 1;
    return 0;
}

static int prompt_requests_read(const char *prompt) {
    if (!prompt) return 0;
    const char *task=strstr(prompt,"## 用户任务\n"); if(task) prompt=task+strlen("## 用户任务\n");
    return (strstr(prompt,"读") || strstr(prompt,"read ")) &&
        (strstr(prompt,"代码") || strstr(prompt,"源码") || strstr(prompt,"文件") || strstr(prompt,"目录") ||
         strstr(prompt,".c") || strstr(prompt,".h") || strstr(prompt,".txt") || strstr(prompt,".py") || strstr(prompt,".log"));
}
static int prompt_requires_full_read(const char *prompt) {
    if (prompt) { const char *task=strstr(prompt,"## 用户任务\n"); if(task) prompt=task+strlen("## 用户任务\n"); }
    return prompt && (strstr(prompt, "代码") || strstr(prompt, "源码") || strstr(prompt, "source") || strstr(prompt, "code") || strstr(prompt,"浏览器") ||
        strstr(prompt,"网页") || strstr(prompt,"页面") || strstr(prompt,"文档") || strstr(prompt,"文件")) &&
        (strstr(prompt, "读") || strstr(prompt, "read") || strstr(prompt, "review") || strstr(prompt,"提取")) &&
        (strstr(prompt, "全部") || strstr(prompt, "完整") || strstr(prompt, "完备") || strstr(prompt, "所有") ||
         strstr(prompt, "补读") || strstr(prompt, "继续") || strstr(prompt, "entire") || strstr(prompt, "all ") || strstr(prompt, "full"));
}

/* Completion is scoped to the user's reading request, not every file opened
 * while researching or debugging generated code. Only the original request
 * is examined: tool output cannot grant itself a partial-read exemption. */
static const char *read_clause_end(const char *begin) {
    const char *p = begin;
    while (*p && *p != '\n' && *p != '\r' && *p != ';' &&
           strncmp(p,"；",strlen("；")) && strncmp(p,"。",strlen("。"))) ++p;
    return p;
}

static const char *read_next_clause(const char *end) {
    if (!*end) return end;
    if (!strncmp(end,"；",strlen("；"))) return end+strlen("；");
    if (!strncmp(end,"。",strlen("。"))) return end+strlen("。");
    return end+1;
}

static int read_span_has(const char *begin, const char *end, const char *needle) {
    const char *p = strstr(begin, needle);
    return p && p + strlen(needle) <= end;
}

static int file_token_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.' || c == '/' || c == '\\' || c == ':';
}

/* Enumerate explicit file references in a clause without treating a shared
 * basename substring (e.g. foo.c in otherfoo.c) as the same subject. */
static int read_span_files(const char *begin, const char *end, const char *wanted, char *last, size_t cap) {
    int found = 0;
    if (last && cap) last[0] = '\0';
    for (const char *p = begin; p < end;) {
        if (!file_token_char((unsigned char)*p)) { ++p; continue; }
        const char *base = p, *dot = NULL;
        while (p < end && file_token_char((unsigned char)*p)) {
            if (*p == '/' || *p == '\\') { base = p + 1; dot = NULL; }
            else if (*p == '.') dot = p;
            ++p;
        }
        const char *finish=p;
        while(finish>base && (finish[-1]=='.' || finish[-1]==':')) --finish;
        dot=NULL;
        for(const char *q=base;q<finish;q++) if(*q=='.') dot=q;
        if (!dot || dot == base || dot + 1 >= finish || finish - dot > 13) continue;
        char name[256]; size_t n = (size_t)(finish - base);
        if (n >= sizeof(name)) continue;
        memcpy(name,base,n); name[n] = '\0';
        /* Dotted config keys such as llm.model are not file declarations. */
        static const char *const extensions[] = {"c","h","cc","cpp","hpp","rs","go","py","js","ts","tsx","jsx",
            "java","kt","swift","m","mm","cs","sh","ps1","bat","txt","md","json","yaml","yml","toml",
            "xml","html","css","scss","sql","log","ini","conf","cfg","vue","svelte","rb","php","pptx","docx","pdf"};
        const char *extension=strrchr(name,'.')+1;
        int recognized=0;
        for(size_t i=0;i<sizeof(extensions)/sizeof(extensions[0]);i++) {
            const char *rest=information_prefix(extension,extensions[i]);
            if(rest && !*rest) { recognized=1;break; }
        }
        if(!recognized) continue;
        if (last && cap) snprintf(last,cap,"%s",name);
        if (!wanted || (information_prefix(name,wanted) && !*information_prefix(name,wanted))) found = 1;
    }
    return found;
}

static int read_span_full(const char *begin, const char *end) {
    static const char *const full[] = {"完整","全文","全部","所有","补读","entire","all ","full"};
    static const char *const no[] = {"不要求全文","无需全文","不需要全文","不必全文","不用全文",
        "不要求完整","不需要完整","无需完整","不要完整","不必完整","not the entire","not the whole","not full"};
    for (size_t i=0;i<sizeof(full)/sizeof(full[0]);++i) {
        const char *p=begin;
        while ((p=strstr(p,full[i])) && p+strlen(full[i])<=end) {
            int negated=0;
            for(size_t j=0;j<sizeof(no)/sizeof(no[0]);++j) {
                const char *n=begin;
                while((n=strstr(n,no[j])) && n+strlen(no[j])<=end) {
                    if(n<=p && p<n+strlen(no[j])) negated=1;
                    ++n;
                }
            }
            if(!negated) return 1;
            ++p;
        }
    }
    return 0;
}

static int path_requires_full_read(reasoning *r, const char *prompt, const char *path) {
    (void)r;
    if (!prompt_requires_full_read(prompt)) return 0;
    const char *task=strstr(prompt,"## 用户任务\n"); if(task) prompt=task+strlen("## 用户任务\n");
    const char *base=path;
    for(const char *p=path;*p;++p) if(*p=='/' || *p=='\\') base=p+1;
    static const char *const partial[] = {"不要求全文","无需全文","不需要全文","不必全文","不用全文",
        "不要求完整","不需要完整","无需完整","只看相关","只读相关","只读取相关","仅阅读相关",
        "only relevant","relevant sections only","not the entire","not the whole","not full"};
    int scoped=0, target=0, global=0, exempt=0;
    for(const char *begin=prompt;*begin;) {
        const char *end=read_clause_end(begin);
        for(size_t i=0;i<sizeof(partial)/sizeof(partial[0]);++i) {
            const char *p=begin;
            while((p=strstr(p,partial[i])) && p+strlen(partial[i])<=end) {
                char subject[256];
                read_span_files(begin,p,NULL,subject,sizeof(subject));
                if (subject[0] && information_prefix(subject,base) && !*information_prefix(subject,base)) exempt=1;
                ++p;
            }
        }
        if ((read_span_has(begin,end,"读") || read_span_has(begin,end,"read") || read_span_has(begin,end,"review")) && read_span_full(begin,end)) {
            static const char *const groups[] = {"全部源码","所有源码","全部文件","所有文件","整个项目","整个代码库",
                "all source","all files","entire project","entire repository"};
            for(size_t i=0;i<sizeof(groups)/sizeof(groups[0]);i++)
                if(read_span_has(begin,end,groups[i])) global=1;
            if(read_span_files(begin,end,NULL,NULL,0)) {
                scoped=1;
                if(read_span_files(begin,end,base,NULL,0)) target=1;
            } else global=1;
        }
        begin=read_next_clause(end);
    }
    if(exempt) return 0;
    return global || !scoped || target;
}

static int required_pending_reads(reasoning *r, const char *prompt) {
    int count=0;
    for(int i=0;i<r->pending_reads_n;i++)
        count+=path_requires_full_read(r,prompt,r->pending_reads[i].path);
    return count;
}

static int answer_reports_unread(const char *text) {
    const char *marks[] = {"未读取", "未读到", "尚未读", "需要补读", "只读了", "只读取了",
                          "还需要读取", "not yet read", "have not read", "files remain unread"};
    for (size_t i=0; text && i<sizeof(marks)/sizeof(marks[0]); ++i)
        if (strstr(text, marks[i])) return 1;
    return 0;
}

static int answer_reports_required_unread(reasoning *r, const char *prompt, const char *text) {
    if (!answer_reports_unread(text)) return 0;
    for (const char *begin=text;*begin;) {
        const char *end=read_clause_end(begin);
        size_t length=(size_t)(end-begin);
        char *clause=malloc(length+1);
        if(!clause) return 1;
        memcpy(clause,begin,length); clause[length]='\0';
        int unread=answer_reports_unread(clause); free(clause);
        if(unread) {
            int optional=0;
            for(int pass=0;pass<2;pass++) {
                int count=pass ? r->completed_reads_n : r->pending_reads_n;
                for(int i=0;i<count;i++) {
                    const char *path=pass ? r->completed_reads[i].path : r->pending_reads[i].path;
                    const char *base=path;
                    for(const char *p=path;*p;p++) if(*p=='/' || *p=='\\') base=p+1;
                    if(!read_span_files(begin,end,base,NULL,0)) continue;
                    if(path_requires_full_read(r,prompt,path)) return 1;
                    optional=1;
                }
            }
            if(!optional) return 1;
        }
        begin=read_next_clause(end);
    }
    return 0;
}

static int run_has_mutating_action(const reasoning *r) {
    return r->run_mutated;
}

/* The model sometimes echoes the injected "[system] …" nudge text at the
 * start of its reply. Strip that echo (up to the end of a known nudge
 * sentence) so it never lands in the round log / answer, and so the
 * same-answer repetition check below compares clean texts. Returns a
 * pointer into `text` (no allocation). */
static const char *strip_nudge_echo(const char *text) {
    static const char *const tails[] = {
        "\xe8\xaf\xb7\xe7\x9b\xb4\xe6\x8e\xa5\xe7\xbb\x99\xe5\x87\xba\xe6\x9c\x80\xe7\xbb\x88\xe7\xad\x94\xe6\xa1\x88\xe6\x96\x87\xe6\x9c\xac\xe3\x80\x82", /* 请直接给出最终答案文本。 */
        "\xe5\x90\xa6\xe5\x88\x99\xe7\xbb\x99\xe5\x87\xba\xe4\xb8\x8e\xe4\xb9\x8b\xe5\x89\x8d\xe4\xb8\x8d\xe5\x90\x8c\xe7\x9a\x84\xe4\xb8\x8b\xe4\xb8\x80\xe6\xad\xa5\xe5\x8a\xa8\xe4\xbd\x9c\xe3\x80\x82" /* 否则给出与之前不同的下一步动作。 */
    };
    const char *p = text;
    size_t i;
    if (!text)
        return text;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (strncmp(p, "[system]", 8) != 0)
        return text;
    for (i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
        const char *mark = strstr(p, tails[i]);
        if (mark && (size_t)(mark - p) < 1024) {
            p = mark + strlen(tails[i]);
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                p++;
            return p;
        }
    }
    return text;
}

static void record_turn(reasoning *r, const char *q, const char *a) {
    char *stored_q, *stored_a;

    if (!r || !q || !a)
        return;
    mutex_lock(&r->cur->mtx);
    r->cur->loaded = 1; /* live turns win; no replay after this point */
    if (r->cur->hist_n == 0 && !r->cur->title[0]) {
        /* first turn: the user message head becomes the session title */
        char *t = str_head(q, 60);
        snprintf(r->cur->title, sizeof(r->cur->title), "%s", t ? t : "");
        free(t);
    }
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
    /* Display/persistence must retain the answer. Model prompt history has
     * its own hot-tier budget; shrinking that must not corrupt chat records. */
    r->cur->hist_a[r->cur->hist_n] = xstrdup(a);
    if (!r->cur->hist_q[r->cur->hist_n])
        r->cur->hist_q[r->cur->hist_n] = xstrdup(q);
    if (!r->cur->hist_a[r->cur->hist_n])
        r->cur->hist_a[r->cur->hist_n] = xstrdup(a);
    r->cur->hist_n++;
    stored_q = xstrdup(r->cur->hist_q[r->cur->hist_n - 1] ? r->cur->hist_q[r->cur->hist_n - 1] : q);
    stored_a = xstrdup(r->cur->hist_a[r->cur->hist_n - 1] ? r->cur->hist_a[r->cur->hist_n - 1] : a);
    /* ring full → compact the oldest half via LLM instead of silent loss */
    if (r->cur->hist_n >= r->cur->hist_cap && !r->cur->compact_disabled && r->llm)
        compact_history(r, r->cur->hist_cap / 2);
    mutex_unlock(&r->cur->mtx);
    /* durable copy (after unlock — file I/O off the hot path; private copies
     * survive compaction touching the ring) */
    chat_persist_append(r, r->cur->id, stored_q, stored_a);
    free(stored_q);
    free(stored_a);
    /* refresh the metadata index (title on first turn) */
    mutex_lock(&r->ss->sess_mtx);
    meta_upsert_locked(r, r->cur);
    meta_save_locked(r);
    mutex_unlock(&r->ss->sess_mtx);
}

char *reasoning_history_json_ex(reasoning *r, const char *session_id, int max_turns) {
    struct session *s;
    size_t start;
    cJSON *arr;
    char *sjson;
    const char *want = session_id && *session_id ? session_id : "default";
    int known = 0;

    if (!r)
        return xstrdup("[]");
    if (max_turns <= 0)
        max_turns = 20;
    /* History reads must not create a new session: querying a deleted ID
     * used to resurrect it in sessions.json at shutdown. */
    mutex_lock(&r->ss->sess_mtx);
    for (size_t i = 0; i < r->ss->nsessions && !known; i++)
        known = strcmp(r->ss->sessions[i]->id, want) == 0;
    for (size_t i = 0; i < r->ss->nmeta && !known; i++)
        known = strcmp(r->ss->meta[i].id, want) == 0;
    mutex_unlock(&r->ss->sess_mtx);
    if (!known && r->state_root) {
        char path[700];
        chat_file_path(path, sizeof(path), r->state_root, want);
        FILE *f = fopen(path, "rb");
        if (f) {
            known = 1;
            fclose(f);
        }
    }
    if (!known)
        return xstrdup("[]");
    s = session_get(r, session_id);
    if (!s)
        return xstrdup("[]");
    mutex_lock(&s->mtx);
    if (!s->loaded) {
        s->loaded = 1;
        session_load_persisted(r, s); /* replay chat/<id>.jsonl after a restart */
    }
    start = (s->hist_n > (size_t)max_turns) ? s->hist_n - (size_t)max_turns : 0;
    arr = cJSON_CreateArray();
    for (size_t i = start; i < s->hist_n; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "q", s->hist_q[i] ? s->hist_q[i] : "");
        cJSON_AddStringToObject(t, "a", s->hist_a[i] ? s->hist_a[i] : "");
        cJSON_AddItemToArray(arr, t);
    }

    mutex_unlock(&s->mtx);
    sjson = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return sjson ? sjson : xstrdup("[]");
}

char *reasoning_history_json(reasoning *r, int max_turns) {
    return reasoning_history_json_ex(r, NULL, max_turns);
}

/* Sessions listing for the UI:
 * [{id, title, turns, created_ms, total_ms, last_active_ms, shared_memory, task}]. */
char *reasoning_sessions_json(reasoning *r) {
    cJSON *arr;
    char *sjson;
    char dir[600];

    if (!r)
        return xstrdup("[]");
    mutex_lock(&r->ss->sess_mtx);
    arr = cJSON_CreateArray();
    for (size_t i = 0; i < r->ss->nsessions; i++) {
        struct session *s = r->ss->sessions[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s->id);
        mutex_lock(&s->mtx);
        if (!s->title[0]) {
            /* replayed sessions have an empty title (record_turn only names a
             * session on its true first turn; replayed history defeats that)
             * — adopt the title from the persisted sessions.json index so the
             * "最近" list keeps the human-readable name after a click
             * materializes the session (user report: 最近预览回退成 uuid) */
            for (size_t k = 0; k < r->ss->nmeta; k++)
                if (strcmp(r->ss->meta[k].id, s->id) == 0) {
                    snprintf(s->title, sizeof(s->title), "%s", r->ss->meta[k].title);
                    break;
                }
        }
        cJSON_AddStringToObject(o, "title", s->title);
        cJSON_AddNumberToObject(o, "turns", (double)s->hist_n);
        cJSON_AddNumberToObject(o, "created_ms", (double)s->created_ms);
        cJSON_AddNumberToObject(o, "total_ms", (double)s->total_ms);
        cJSON_AddNumberToObject(o, "last_active_ms", (double)s->last_active_ms);
        cJSON_AddBoolToObject(o, "shared_memory", atomic_load(&r->ss->shared_memory_global));
        cJSON_AddStringToObject(o, "task", s->sn_task);
        mutex_unlock(&s->mtx);
        cJSON_AddItemToArray(arr, o);
    }

    /* sessions persisted on disk but not yet materialized in memory (e.g.
     * after a restart) still show up in the "最近" list — metadata (title/
     * times/shared flag) comes from the sessions.json index */
    if (r->state_root) {
        path_join(dir, sizeof(dir), r->state_root, "chat");
        dir_list dl;
        if (fs_list_dir(dir, &dl) == 0) {
            for (size_t i = 0; i < dl.count; i++) {
                if (dl.items[i].is_dir)
                    continue;
                const char *name = dl.items[i].name;
                size_t nl = strlen(name);
                if (nl < 7 || strcmp(name + nl - 6, ".jsonl") != 0)
                    continue;
                char sid[128];
                snprintf(sid, sizeof(sid), "%.*s", (int)(nl - 6), name);
                int seen = 0;
                for (size_t k = 0; k < r->ss->nsessions; k++)
                    if (strcmp(r->ss->sessions[k]->id, sid) == 0) {
                        seen = 1;
                        break;
                    }
                if (seen)
                    continue;
                cJSON *o = cJSON_CreateObject();
                if (!o)
                    break;
                const struct sess_meta *m = NULL;
                for (size_t k = 0; k < r->ss->nmeta; k++)
                    if (strcmp(r->ss->meta[k].id, sid) == 0) {
                        m = &r->ss->meta[k];
                        break;
                    }
                cJSON_AddStringToObject(o, "id", sid);
                cJSON_AddStringToObject(o, "title", m ? m->title : "");
                cJSON_AddNumberToObject(o, "turns", 0);
                cJSON_AddNumberToObject(o, "created_ms", (double)(m ? m->created_ms : 0));
                cJSON_AddNumberToObject(o, "total_ms", (double)(m ? m->total_ms : 0));
                cJSON_AddNumberToObject(o, "last_active_ms", (double)(m ? m->last_active_ms : 0));
                cJSON_AddBoolToObject(o, "shared_memory", atomic_load(&r->ss->shared_memory_global));
                cJSON_AddStringToObject(o, "task", "");
                cJSON_AddItemToArray(arr, o);
            }
            fs_list_free(&dl);
        }
    }

    /* A session can contain only a failed/cancelled task, so it has metadata
     * and a task-journal entry but no successful chat transcript yet. Keep it
     * in 最近 after restart so the durable execution record remains reachable. */
    for (size_t i = 0; i < r->ss->nmeta; i++) {
        const struct sess_meta *m = &r->ss->meta[i];
        int seen = 0;
        cJSON *existing;
        cJSON_ArrayForEach(existing, arr) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(existing, "id");
            if (cJSON_IsString(id) && strcmp(id->valuestring, m->id) == 0) {
                seen = 1;
                break;
            }
        }
        if (seen)
            continue;
        cJSON *o = cJSON_CreateObject();
        if (!o)
            break;
        cJSON_AddStringToObject(o, "id", m->id);
        cJSON_AddStringToObject(o, "title", m->title);
        cJSON_AddNumberToObject(o, "turns", 0);
        cJSON_AddNumberToObject(o, "created_ms", (double)m->created_ms);
        cJSON_AddNumberToObject(o, "total_ms", (double)m->total_ms);
        cJSON_AddNumberToObject(o, "last_active_ms", (double)m->last_active_ms);
        cJSON_AddBoolToObject(o, "shared_memory", atomic_load(&r->ss->shared_memory_global));
        cJSON_AddStringToObject(o, "task", "");
        cJSON_AddItemToArray(arr, o);
    }

    mutex_unlock(&r->ss->sess_mtx);
    sjson = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return sjson ? sjson : xstrdup("[]");
}

/* Create a fresh session with a random UUID id; registers it in the session
 * table and the metadata index. Returns a malloc'd id string, NULL on failure. */
char *reasoning_session_new(reasoning *r) {
    char uuid[37];
    struct session *s;

    if (!r)
        return NULL;
    session_uuid(uuid);
    s = session_get(r, uuid);
    if (!s || strcmp(s->id, uuid) != 0)
        return NULL; /* cap reached or alloc failed — do not silently alias */
    mutex_lock(&r->ss->sess_mtx);
    meta_upsert_locked(r, s);
    meta_save_locked(r);
    mutex_unlock(&r->ss->sess_mtx);
    return xstrdup(uuid);
}

void reasoning_session_note_prompt(reasoning *r, const char *session_id, const char *prompt) {
    struct session *s;
    char *title;

    if (!r || !prompt || !*prompt)
        return;
    s = session_get(r, session_id);
    if (!s)
        return;
    title = str_head(prompt, 60);
    if (!title)
        return;
    mutex_lock(&s->mtx);
    if (!s->title[0])
        snprintf(s->title, sizeof(s->title), "%s", title);
    mutex_unlock(&s->mtx);
    free(title);
    mutex_lock(&r->ss->sess_mtx);
    meta_upsert_locked(r, s);
    meta_save_locked(r);
    mutex_unlock(&r->ss->sess_mtx);
}

/* Compatibility entry point: sharing is now a runtime-wide setting. */
int reasoning_session_set_shared(reasoning *r, const char *session_id, int shared) {
    char path[700], dir[600];
    const char *setting = shared ? "{\"shared\":true}" : "{\"shared\":false}";
    if (!r || !session_id || !*session_id || !r->state_root)
        return -1;
    path_join(dir, sizeof(dir), r->state_root, "chat");
    if (fs_mkdirs(dir) != 0)
        return -1;
    path_join(path, sizeof(path), dir, "memory-sharing.json");
    if (fs_write_file(path, setting, strlen(setting)) != 0)
        return -1;
    atomic_store(&r->ss->shared_memory_global, shared != 0);
    mutex_lock(&r->ss->sess_mtx);
    for (size_t i = 0; i < r->ss->nsessions; i++) {
        struct session *s = r->ss->sessions[i];
        mutex_lock(&s->mtx);
        s->shared_memory = shared != 0;
        mutex_unlock(&s->mtx);
        meta_upsert_locked(r, s);
    }
    meta_save_locked(r);
    mutex_unlock(&r->ss->sess_mtx);
    return 0;
}

int reasoning_shared_memory_enabled(reasoning *r) {
    return r ? atomic_load(&r->ss->shared_memory_global) : 0;
}

/* Last recorded user input of a session (for 恢复/resume). Returns a malloc'd
 * string or NULL when the session is unknown or has no turns. Does not
 * create the session if it does not exist. */
char *reasoning_session_last_input(reasoning *r, const char *session_id) {
    struct session *s = NULL;
    char *out = NULL;

    if (!r || !session_id || !*session_id)
        return NULL;
    mutex_lock(&r->ss->sess_mtx);
    for (size_t i = 0; i < r->ss->nsessions; i++)
        if (strcmp(r->ss->sessions[i]->id, session_id) == 0) {
            s = r->ss->sessions[i];
            break;
        }
    mutex_unlock(&r->ss->sess_mtx);
    if (!s)
        return NULL;
    mutex_lock(&s->mtx);
    if (!s->loaded) {
        s->loaded = 1;
        session_load_persisted(r, s); /* replay chat/<id>.jsonl after a restart */
    }
    if (s->hist_n > 0 && s->hist_q[s->hist_n - 1])
        out = xstrdup(s->hist_q[s->hist_n - 1]);
    mutex_unlock(&s->mtx);
    return out;
}

/* Clear one session's conversation (keeps the session itself). */
int reasoning_session_clear(reasoning *r, const char *session_id) {
    const char *want = (session_id && *session_id) ? session_id : "default";
    struct session *s = NULL;

    if (!r)
        return -1;
    mutex_lock(&r->ss->sess_mtx);
    for (size_t i = 0; i < r->ss->nsessions; i++)
        if (strcmp(r->ss->sessions[i]->id, want) == 0) {
            s = r->ss->sessions[i];
            break;
        }

    mutex_unlock(&r->ss->sess_mtx);
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
    /* drop the durable copy too — clearing a session must survive restarts */
    if (r->state_root) {
        char fpath[700];
        chat_file_path(fpath, sizeof(fpath), r->state_root, want);
        fs_remove(fpath);
        ssh_session_profiles_clear(r->state_root, want);
    }
    return 0;
}

/* Delete a session entirely: unregister it, drop its meta index entry and
 * remove the durable chat/<id>.jsonl transcript. Returns 0 ok, -1 unknown,
 * -2 busy (a run is in flight on the session — cancelling first is safer). */
int reasoning_session_delete(reasoning *r, const char *session_id) {
    const char *want = (session_id && *session_id) ? session_id : "default";
    struct session *s = NULL;
    size_t idx = 0;

    if (!r)
        return -1;
    mutex_lock(&r->ss->sess_mtx);

    for (size_t i = 0; i < r->ss->nsessions; i++)
        if (strcmp(r->ss->sessions[i]->id, want) == 0) {
            s = r->ss->sessions[i];
            idx = i;
            break;
        }

    if (!s) {
        /* not live in memory — still drop it from the persisted meta index */
        int removed = 0;
        for (size_t i = 0; i < r->ss->nmeta; i++) {
            if (strcmp(r->ss->meta[i].id, want) == 0) {
                memmove(&r->ss->meta[i], &r->ss->meta[i + 1], (r->ss->nmeta - i - 1) * sizeof(r->ss->meta[0]));
                r->ss->nmeta--;
                removed = 1;
                break;
            }
        }
        meta_save_locked(r);
        mutex_unlock(&r->ss->sess_mtx);
        if (removed && r->state_root) {
            char fpath[700];
            chat_file_path(fpath, sizeof(fpath), r->state_root, want);
            fs_remove(fpath);
            ssh_session_profiles_clear(r->state_root, want);
        }
        return removed ? 0 : -1;
    }

    if (s->in_run) {
        mutex_unlock(&r->ss->sess_mtx);
        return -2;
    }

    memmove(&r->ss->sessions[idx], &r->ss->sessions[idx + 1],
            (r->ss->nsessions - idx - 1) * sizeof(*r->ss->sessions));
    r->ss->nsessions--;
    if (r->cur == s)
        r->cur = NULL;
    for (size_t i = 0; i < r->ss->nmeta; i++) {
        if (strcmp(r->ss->meta[i].id, want) == 0) {
            memmove(&r->ss->meta[i], &r->ss->meta[i + 1], (r->ss->nmeta - i - 1) * sizeof(r->ss->meta[0]));
            r->ss->nmeta--;
            break;
        }
    }
    meta_save_locked(r);
    mutex_unlock(&r->ss->sess_mtx);

    if (r->state_root) {
        char fpath[700];
        chat_file_path(fpath, sizeof(fpath), r->state_root, want);
        fs_remove(fpath);
        ssh_session_profiles_clear(r->state_root, want);
    }
    session_free(s);
    return 0;
}

int reasoning_run(reasoning *r, const char *prompt, char **answer) {
    return reasoning_run_ex(r, NULL, prompt, answer);
}

int reasoning_run_ex(reasoning *r, const char *session_id, const char *prompt, char **answer) {
    char *safe_prompt;
    /* Router: pick a provider for this run (weighted round-robin). */
    llm *picked = NULL;
    llm *saved = NULL;
    char *final_text = NULL;
    char *result = NULL;
    char *last_narration = NULL; /* previous narration text (repetition break) */
    int stalled = 0;
    /* compose the answer: everything that happened + the final reply */
    strbuf out;
    char *combined;
    int ret = -1;
    long long run_t0 = 0;

    if (!r || !prompt)
        return -1;
    run_t0 = (long long)time_now_ms();

    /* select (or create) the chat session this run belongs to; a lane runs
     * one prompt at a time so swapping r->cur here is race-free. The
     * session's in_run flag is published under sess_mtx so
     * reasoning_session_delete can refuse to free a session that is
     * executing (across lanes via the shared store). */
    mutex_lock(&r->ss->sess_mtx);
    r->cur = session_get_locked(r, session_id);
    /* A session is a sequential conversation. Different sessions may run on
     * different lanes, but accepting two runs for the same session lets their
     * histories, summaries and steering messages interleave. */
    if (r->cur && r->cur->in_run) {
        mutex_unlock(&r->ss->sess_mtx);
        if (answer)
            *answer = xstrdup("(this session already has a running task)");
        return -2;
    }
    if (r->cur)
        r->cur->in_run = 1;
    mutex_unlock(&r->ss->sess_mtx);
    if (r->cur)
        r->cur->last_active_ms = (long long)time_now_ms();

    /* fresh step registry for this run (keep the buffer) */
    mutex_lock(&r->progress_mtx);
    for (int i=0;i<r->n_steps;++i) step_strings_free(&r->steps[i]);
    r->n_steps = 0;
    r->step_sequence = 0;
    mutex_unlock(&r->progress_mtx);
    r->progress_seq = 0;
    r->applied_updates = 0;
    r->prog_started_ms = time_now_ms();
    r->prog_tool[0] = 0;
    r->prog_tool_calls = r->prog_llm_calls = r->round_idx = r->prog_model_failures = 0;
    r->prog_llm_ms = r->prog_tool_ms = 0;
    r->last_model_error[0]=0;
    r->planner_output_escalated = 0;
    /* Shared memory is a live runtime-wide policy, checked at each read/write. */

    /* Ingestion guard: a prompt with invalid UTF-8 (e.g. a non-UTF-8 API
     * client) would poison memory/history and break every later LLM call. */
    safe_prompt = str_utf8_sanitize(prompt);
    if (safe_prompt)
        prompt = safe_prompt;
    char *vault_safe_prompt = ssh_session_prepare_prompt(r->state_root, session_id, prompt);
    free(safe_prompt);
    safe_prompt = vault_safe_prompt;
    if (!safe_prompt) {
        if (answer) *answer = xstrdup("(local SSH credential storage failed)");
        mutex_lock(&r->ss->sess_mtx);
        if (r->cur) r->cur->in_run = 0;
        mutex_unlock(&r->ss->sess_mtx);
        return -1;
    }
    prompt = safe_prompt;

    if (r->router) {
        const route *rt = router_pick(r->router);
        if (rt) {
            picked = llm_create(rt->provider, rt->base_url, rt->api_key, rt->model);
            if (picked) {
                picked->context_window_tokens = rt->context_window_tokens;
                saved = r->llm;
                r->llm = picked;
            }
        }
    }

    llm_usage_thread_totals(&r->usage_base_in, &r->usage_base_out, &r->usage_base_reason, &r->run_usage_reports);
    r->usage_base_reports = r->run_usage_reports;
    r->run_usage_in = r->usage_base_in;
    r->run_usage_out = r->usage_base_out;
    progress_emit(r, "analyzing");

    free(r->last_prompt);
    r->last_prompt = xstrdup(prompt);
    r->gen_attempted = 0; /* one auto-generation attempt per run */
    /* Do not publish an in-flight prompt into shared retrieval. A different
     * session must only see completed knowledge, never an unfinished task it
     * could accidentally continue or execute. */

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
            mutex_lock(&r->ss->sess_mtx);
            if (r->cur)
                r->cur->in_run = 0;
            mutex_unlock(&r->ss->sess_mtx);
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
    r->last_plan_action_sig = r->prev_plan_action_sig = 0;
    r->stall_nudged = 0;
    r->intent_nudged = 0;
    r->prog_tool_calls = 0;
    r->prog_llm_ms = 0;
    r->prog_llm_calls = 0;
    r->prog_tool_ms = 0;
    r->prog_tool[0] = '\0';
    r->last_failed_action_sig = 0;
    r->same_action_failures = 0;
    r->invalid_args_n = 0;
    r->run_had_action_failure = 0;
    memset(r->unresolved_failed_tools, 0, sizeof(r->unresolved_failed_tools));
    memset(r->unresolved_failed_scopes, 0, sizeof(r->unresolved_failed_scopes));
    memset(r->unresolved_discovered_reads, 0, sizeof(r->unresolved_discovered_reads));
    r->unresolved_failed_tool_count = 0;
    r->unresolved_failed_tools_overflow = 0;
    r->tool_fail_aborted = 0;
    r->visual_request_failed = 0;
    r->run_mutated = 0;
    r->run_successful_tools = 0;
    r->search_seen_n = r->versioned_seen_n = 0;
    r->round_search_skips = 0;
    r->observation_seen_n = 0;
    r->round_new_observations = r->round_repeat_reads = 0;
    r->progress_cycle_warning_active = 0;
    pending_reads_clear(r);

     /* LLM's plain-text answer (had_plan == 0) */
         /* per-round pipeline output */
    state st = ST_FAILED;
    int consec_fail = 0; /* consecutive stage failures → circuit breaker */
    int transport_fail_streak = 0;
    int fail_aborted = 0;
    int unfinished = 0; /* text described future work, or requested write never ran */
    int search_only_rounds = 0;
    int artifact_read_nudged = 0;
    int failure_recovery_nudges = 0, truncated_response_nudges = 0;
    int output_truncated_unresolved = 0;
    char *truncated_prefix = NULL;
    int synthesized = 0; /* fallback answer may describe an incomplete run */
    int no_progress_rounds = 0, cycle_reads = 0, cycle_nudged = 0, cycle_aborted = 0;
restart_planning:
    for (r->round_idx = 1; r->round_idx <= r->max_rounds; r->round_idx++) {
        if (run_aborted(r)) { st = ST_FAILED; break; }
        char *messages = task_take_messages(r->run_task, &r->applied_updates);
        if (messages) {
            char *safe_messages = ssh_session_prepare_prompt(r->state_root, session_id, messages);
            free(messages);
            if (!safe_messages) { st = ST_FAILED; break; }
            messages = safe_messages;
            const char *separator = "\n\n## 用户执行中补充（最新要求优先，保留已完成工作，重新规划剩余步骤）\n";
            size_t size = strlen(prompt) + strlen(separator) + strlen(messages) + 1;
            char *updated = malloc(size);
            if (!updated) { free(messages); st = ST_FAILED; break; }
            snprintf(updated, size, "%s%s%s", prompt, separator, messages);
            free(messages); free(safe_prompt); safe_prompt = updated; prompt = updated;
            r->stall_nudged = r->intent_nudged = 0;
            r->invalid_args_n = 0;
            r->search_seen_n = r->versioned_seen_n = 0;
            search_only_rounds = artifact_read_nudged = 0;
            no_progress_rounds = cycle_reads = cycle_nudged = cycle_aborted = 0;
            r->observation_seen_n = 0;
            free(r->prev_plan); r->prev_plan = NULL;
            /* A steering message defines a revised task and needs a fresh
             * planning budget. Without resetting the per-plan counter, an
             * update accepted near the end of a long run reaches the forced
             * tool-free final round before its requested actions can run. */
            r->round_idx = 0;
            progress_emit(r, "replanning");
        }
        free(result);
        result = NULL;
        r->round_new_observations = r->round_repeat_reads = 0;
        st = state_machine_run(r->sm, prompt, &result);
        usage_book_round(r);
        if (run_aborted(r)) { st = ST_FAILED; break; }
        if (r->tool_fail_aborted) {
            round_log_append(r, result && *result ? result : "(tool failed repeatedly)");
            obs_log_append(r, result && *result ? result : "(tool failed repeatedly)");
            st = ST_FAILED; /* do not report an incomplete task as completed */
            break;
        }
        if (task_has_messages(r->run_task)) {
            /* Preserve real completed observations, discard unexecuted plans. */
            if (r->had_plan && result) {
                round_log_append(r, result); obs_log_append(r, result);
            }
            continue;
        }
        /* Count evidence, not rounds or API spend. Successful reads of new
         * pages/changed content reset this streak, regardless of task length.
         * Interleaved failures, cache evictions and reordered plans do not. */
        if (r->had_plan && r->n_actions>0) {
            if (r->round_new_observations>0) {
                no_progress_rounds=cycle_reads=cycle_nudged=0;
                r->progress_cycle_warning_active=0;
            } else {
                no_progress_rounds++;
                cycle_reads += r->round_repeat_reads;
                if (no_progress_rounds>=4 && cycle_reads>=2 && !cycle_nudged) {
                    cycle_nudged=1;
                    r->progress_cycle_warning_active=1;
                    round_log_append(r,"[system] 检测到跨轮次循环：多轮交替读取或检查只返回已有结果，没有新增进展。"
                        "不要重读相同文件页或换序执行旧检查；请读取真正缺失的新页、修正失败原因并验证产物，"
                        "或直接根据已有证据回答。继续重复将停止本次无进展循环。");
                }
                if (no_progress_rounds>=8 && cycle_reads>=2) {
                    round_log_append(r,result ? result : "");
                    obs_log_append(r,result ? result : "");
                    stalled=cycle_aborted=1; st=ST_FAILED; break;
                }
            }
        }
        if (st != ST_DONE) {
            /* Stage failure (planner LLM error, VERIFY gate, …): the failed
             * stage's diagnostic is an observation the agent must see, not a
             * reason to die silently. Feed it back and keep looping while
             * round budget remains — the next round can self-correct (e.g.
             * retry with a fixed path). Terminal only when the budget is
             * exhausted, and the report stays in the answer either way. */
            consec_fail++;
            if (!r->had_plan) r->prog_model_failures++;
            if (!r->had_plan && reasoning_is_transport_failure(r->last_model_error))
                transport_fail_streak++;
            else
                transport_fail_streak = 0;
            round_log_append(r, result && *result ? result : "(run failed)");
            obs_log_append(r, result && *result ? result : "(run failed)");
            if (r->visual_request_failed) {
                final_text=xstrdup(result && *result ? result:"图片模型请求失败");
                fail_aborted = 1;
                break;
            }
            if (!r->had_plan && reasoning_is_rate_limited(r->last_model_error)) {
                round_log_append(r, "[system] 模型服务返回 HTTP 429（请求限额或速率限制）；停止自动重试，避免继续消耗请求额度。修复限额或等待重置后，可由用户重新发起任务。");
                fail_aborted = 1;
                break;
            }
            if (transport_fail_streak >= 2) {
                round_log_append(r, "[system] 模型 HTTP 连接连续失败两次，停止自动重试以避免重复等待。"
                                    "请根据上面的传输阶段和错误码检查网络、代理或模型服务，然后恢复任务。");
                fail_aborted = 1;
                break;
            }
            progress_emit(r, r->had_plan ? "recovering" : "reconnecting");
        if (r->round_idx >= r->max_rounds)
                break;
            /* Circuit breaker: a PERSISTENT failure (LLM endpoint down /
             * rate-limited / network gone) makes every round fail instantly,
             * and with an effectively-unlimited round budget the loop would
             * spin at full CPU appending failure text forever (burned a
             * whole night once). A single transient failure is still fed
             * back per issue #25 — only N consecutive failures abort. */
            if (consec_fail >= REASONING_CONSEC_FAIL_ABORT) {
                round_log_append(r, "[system] 连续 " REASONING_CONSEC_FAIL_ABORT_STR
                                    " 轮阶段失败（如模型服务持续不可用），任务中止。");
                fail_aborted = 1;
                break;
            }
            /* issue #25: a failed action (e.g. ssh login failure) must not end
             * the task. Without an explicit recovery instruction the model
             * tends to answer with an apology and the remaining steps of the
             * plan never execute. Tell it to diagnose, retry/redirect, and
             * carry on with the rest of the task. */
            round_log_append(r, "[system] 上一轮有动作执行失败（失败原因和输出在上面）。"
                                "失败的工具调用只是任务中的一个挫折，不是终止信号："
                                "请分析失败原因（命令错误、路径不存在、网络/登录失败等），"
                                "如果来自 Python、脚本、构建或测试命令，先读完整错误输出与退出码，定位根因并修正所有同类错误；"
                                "编辑后重新运行原命令，核对退出码和预期产物确实存在且可读取；只写入脚本或描述修复方案不算完成。"
                                "修正后重试或改用其他方法，并继续执行任务的剩余步骤；"
                                "只有在确认任务确实无法完成时，才输出最终答案并如实说明失败环节。");
            continue;
        }
        consec_fail = 0; /* a successful (ST_DONE) round resets the breaker */
        transport_fail_streak = 0;
        if (!r->had_plan) { /* no actions planned → this is the final answer */
            /* Intent narration ("Let me check the files…") without a single
             * action is a premature stop: the model announced its plan
             * instead of emitting the JSON action array. Narration is NOT
             * confined to round 1, and chatty models narrate repeatedly, so
             * nudge on ANY round — bounded by CONSECUTIVE narration rounds
             * (reset whenever a round executes a plan), not a per-run total:
             * a narration → plan → narration pattern is normal thinking
             * aloud, while the model stuck narrating 4 rounds in a row will
             * not recover. Exhaustion must report an incomplete task. */
            const char *txt = strip_nudge_echo(result);
            if(r->planner_native_end_turn==2) {
                unfinished=1;final_text=xstrdup(txt?txt:"");break;
            }
            if ((r->unresolved_failed_tool_count > 0 || r->unresolved_failed_tools_overflow) &&
                failure_recovery_nudges < 1 && r->round_idx < r->max_rounds) {
                failure_recovery_nudges++;
                round_log_append(r, txt ? txt : "");
                round_log_append(r, "[system] 任务尚未完成：前面有工具调用失败且没有成功核实。不要输出最终总结或停止；先根据失败工具的真实错误调整参数、路径或方法，重试该工具或用可验证的替代方式完成受影响步骤，再继续原任务。修复后的内容必须经过成功工具结果验证。失败工具：");
                for (int i=0;i<r->unresolved_failed_tool_count;i++) {
                    strbuf failure; strbuf_init(&failure);
                    strbuf_appendf(&failure,"%s: %s",r->unresolved_failed_tools[i],r->unresolved_failed_scopes[i]);
                    if (!strcmp(r->unresolved_failed_tools[i],"file_read"))
                        strbuf_append(&failure," — 核对并读取此文件；头文件可能位于同一仓库 include/ 而不是 src/，请先查找真实路径。");
                    round_log_append(r,failure.buf);strbuf_free(&failure);
                }
                continue;
            }
            if (r->planner_response_truncated) {
                strbuf partial; strbuf_init(&partial);
                if (truncated_prefix) strbuf_append(&partial, truncated_prefix);
                strbuf_append(&partial, txt ? txt : "");
                free(truncated_prefix); truncated_prefix=strbuf_detach(&partial);
                if (truncated_response_nudges >= 2 || r->round_idx >= r->max_rounds) {
                    unfinished=1;
                    output_truncated_unresolved=1;
                    final_text=xstrdup(truncated_prefix ? truncated_prefix : "");
                    break;
                }
                truncated_response_nudges++;
                round_log_append(r, txt ? txt : "");
                round_log_append(r, "[system] 上一条答复因达到模型输出 token 上限而被截断。请从刚才最后一个完整句子之后继续，只补充缺失部分，不要重复开头；完成全部内容后再结束。最多继续两次。");
                continue;
            }
            if (truncated_prefix) {
                strbuf complete; strbuf_init(&complete);
                strbuf_append(&complete, truncated_prefix);
                if (txt && *txt) { strbuf_append(&complete, "\n"); strbuf_append(&complete, txt); }
                final_text=strbuf_detach(&complete);
                free(truncated_prefix); truncated_prefix=NULL;
                break;
            }
            int needs_action = prompt_requires_mutation(completion_instruction(r,prompt)) && !run_has_mutating_action(r);
            int needs_read = (prompt_requires_full_read(completion_instruction(r,prompt)) &&
                (required_pending_reads(r,completion_instruction(r,prompt)) || r->read_tracking_overflow ||
                 answer_reports_required_unread(r,completion_instruction(r,prompt),txt))) ||
                (!r->planner_native_end_turn && prompt_requests_read(completion_instruction(r,prompt)) &&
                 !r->run_file_reads && !run_has_mutating_action(r));
            int needs_observation = !r->planner_native_end_turn && prompt_requires_observation(completion_instruction(r,prompt)) &&
                                    r->run_successful_tools == 0;
            /* Prose heuristics are confined to the explicitly selected legacy
             * text protocol (and offline mock). Native turns can reach here
             * without actions only via a validated complete_task declaration. */
            int needs_intent_recovery = !r->planner_native_end_turn && !prompt_is_direct_information_request(completion_instruction(r,prompt)) &&
                                        (r->run_successful_tools == 0 || answer_only_announces_work(txt)) &&
                                        looks_like_intent(txt);
            if (needs_action || needs_read || needs_observation || needs_intent_recovery) {
                /* A plain-text answer closes the loop once its explicit action,
                 * read, and observation gates are met. If a gate is still
                 * unmet, allow bounded consecutive correction requests, then fail closed. */
                if (last_narration && txt && strcmp(txt, last_narration) == 0) {
                    unfinished = 1;
                    final_text = xstrdup(txt);
                    break;
                }
                if (r->max_rounds <= 1 || r->intent_nudged >= (needs_intent_recovery ? 4 : 1) || r->round_idx >= r->max_rounds) {
                    unfinished = 1;
                    final_text = xstrdup(txt ? txt : "");
                    break;
                }
                r->intent_nudged++;
                free(last_narration);
                last_narration = xstrdup(txt ? txt : "");
                round_log_append(r, txt && *txt ? txt : "");
                if (needs_read) {
                    round_log_append(r, "[system] 当前完整源码阅读尚未完成：有文件未读到 EOF，或答案明确列出未读源码。"
                                        "不要把目录枚举或部分源码总结当作完成，也不要让用户重新指定已要求阅读的范围。"
                                        "请继续未读文件及 next_offset_bytes 指定的分页，完成后再整理资料。");
                    continue;
                }
                round_log_append(r, "[system] 上一轮只输出了意向说明，没有执行任何工具动作。"
                                    "如果任务还需要操作（读写文件、执行命令、生成文件等），"
                                    "请输出 JSON 动作数组并实际执行；"
                                    "如果回答里包含应该写入文件的代码，用 file_write 把它写到磁盘，"
                                    "不要把代码当文本贴在回答里；"
                                    "如果任务已经完成或确实无需任何工具，"
                                    "请直接给出最终答案文本。");
                continue;
            }
            /* A raw JSON array as the "answer" is a malformed plan echo: the
             * model meant to emit actions (e.g. a flat ["file_read","path",…]
             * that the planner cannot parse) but the text fell through the
             * no-plan path. Accepting it as the final answer produced garbage
             * node results (a real reviewer run once ended with a bare tool
             * list). Bounce it back with the correct format instead. */
            {
                const char *t2 = txt;
                while (t2 && *t2 == ' ')
                    t2++;
                if (!r->planner_native_end_turn && r->max_rounds > 1 && r->intent_nudged < 4 &&
                    r->round_idx < r->max_rounds && t2 && *t2 == '[') {
                    r->intent_nudged++;
                    round_log_append(r, txt);
                    round_log_append(r, "[system] 上一轮输出的是动作数组，但格式无法解析为计划。"
                                        "如需执行工具动作，请输出 JSON 对象数组："
                                        "[{\"tool\":\"文件工具名\",\"args\":{...}}]；"
                                        "如果任务已完成，请直接输出纯文本的最终答案（不要输出 JSON）。");
                    continue;
                }
            }
            final_text = xstrdup(txt ? txt : "");
            break;
        }
        /* executed a planned round: keep the observation for the next round */
        round_log_append(r, result ? result : "");
        obs_log_append(r, result ? result : "");
        r->intent_nudged = 0; /* narration recovered: reset the consecutive bound */
        int search_only = r->n_actions > 0;
        for (int i = 0; i < r->n_actions; i++)
            if (strcmp(r->actions[i].tool, "glob") != 0 &&
                strcmp(r->actions[i].tool, "grep") != 0 &&
                strcmp(r->actions[i].tool, "file_read") != 0 &&
                strcmp(r->actions[i].tool, "list_dir") != 0) search_only = 0;
        search_only_rounds = search_only ? search_only_rounds + 1 : 0;
        if (!search_only || run_has_mutating_action(r)) artifact_read_nudged = 0;
        if (r->n_actions > 0 && r->round_search_skips == r->n_actions) {
            round_log_append(r, "[system] 本轮全部动作已经成功执行过，没有新增事实。"
                                "先核对已有结果是否已满足原始请求：满足则立即用纯文本交付答案，不要再次执行原请求中的第一步。"
                                "仅在仍缺少明确必需事实时才查询新的目标；不得为了继续执行而列目录、扩大范围或无条件翻页。"
                                "复用结果不能代替用户要求但尚未完成的写入或验证。");
            /* Use the evidence-based recovery window above. A separate
             * two-round cutoff used to abort before its recovery prompt. */
            free(r->prev_plan); r->prev_plan = NULL;
            continue;
        }
        /* A deliverable request cannot make unlimited progress by reading
         * ever more source pages. Once the model has had ample discovery
         * rounds, require an actual artifact action; research may continue
         * after the first write. This bounds the stalled phase, not the task. */
        if (search_only && prompt_requires_mutation(completion_instruction(r,prompt)) &&
            !run_has_mutating_action(r)) {
            if (search_only_rounds >= 12 && !artifact_read_nudged) {
                artifact_read_nudged = 1;
                round_log_append(r,"[system] 已连续多轮阅读资料，用户要求生成或修改实际文件。"
                    "下一轮必须开始写入产物或运行生成命令；后续仍可按需读取源码。"
                    "仅继续阅读不能算完成任务。");
            } else if (search_only_rounds >= 16) {
                round_log_append(r,"[system] 多轮提醒后仍未开始创建用户要求的产物；"
                    "停止当前只读循环，报告未完成和已有证据。");
                stalled = 1;
                break;
            }
        }
        if ((search_only_rounds == 6 || (search_only_rounds > 6 && search_only_rounds % 12 == 0)) &&
            !prompt_requires_mutation(completion_instruction(r,prompt)))
            round_log_append(r, "[system] 已连续多轮读取或搜索资料。请评估已有证据是否足够回答；"
                                "足够就直接给出最终答案，仍缺关键事实则继续读取不同的目标。");
        /* stall detection: the LLM proposed the exact same plan twice — no
         * progress is possible. Give ONE recovery nudge ("the actions already
         * succeeded; answer from the observations instead of repeating them")
         * before giving up — models often re-emit a successful plan because
         * they lost track of the feedback, not because they are stuck. */
        int same_action_plan = r->prev_plan_action_sig != 0 &&
                               r->prev_plan_action_sig == r->last_plan_action_sig;
        int same_raw_plan = r->prev_plan && r->last_plan_raw &&
                            strcmp(r->prev_plan, r->last_plan_raw) == 0;
        if (!r->round_new_observations && (same_action_plan || same_raw_plan)) {
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
    if (!run_aborted(r) && !(fail_aborted && r->last_model_error[0]) &&
        !final_text && r->round_log_len > 0 && r->llm) {
        r->prog_tool[0] = 0;
        progress_emit(r, "summarizing");
        char sys[320];
        snprintf(sys, sizeof(sys),
                 "You are finalizing an agent run. Based on the original request and the "
                 "action observations gathered below, produce the final answer in the "
                 "user's language. Do not propose any further tool calls. If the task is "
                 "incomplete, summarize what was accomplished and what remains.");
        char tail[12288];
        size_t loglen = r->round_log ? strlen(r->round_log) : 0;
        size_t start = loglen >= sizeof(tail) - 1 ? loglen - (sizeof(tail) - 1) : 0;
        while (start < loglen && ((unsigned char)r->round_log[start] & 0xc0) == 0x80)
            start++;
        snprintf(tail, sizeof(tail), "%s", r->round_log + start);
        char *user = (char *)malloc(strlen(prompt) + sizeof(tail) + 64);
        if (user) {
            snprintf(user, strlen(prompt) + sizeof(tail) + 64, "任务: %s\n\n已执行动作的观察记录（末段）:\n%s", prompt,
                     tail);
            char *ans = llm_chat_simple(r->llm, sys, user);
            if (ans && *ans) {
                final_text = ans;
                synthesized = 1;
            } else {
                free(ans);
            }
            free(user);
        }
    }

    /* Linearize finalization with message acceptance. A message accepted
     * during synthesis always gets another planning pass. */
    if (!run_aborted(r) && !task_close_messages(r->run_task)) {
        free(final_text); final_text = NULL;
        free(result); result = NULL;
        free(last_narration); last_narration = NULL;
        stalled = consec_fail = fail_aborted = synthesized = unfinished = 0;
        goto restart_planning;
    }
    /* Cancellation is a terminal outcome of its own. In particular, do not
     * fall through to the observation-log fallback below: large shell outputs
     * (for example PowerShell CLIXML progress records) belong in the expandable
     * execution details, not in the user-facing cancellation message. */
    int cancelled = (r->run_task && r->run_task->cancel_flag) ||
                    (r->abort_parent && r->abort_parent->cancel_flag);
    int timed_out = !cancelled && run_aborted(r);
    /* A read-only final round may exhaust the budget without ever reaching
     * the no-plan guard above. It is still incomplete if a write was asked. */
    if (prompt_requires_mutation(completion_instruction(r,prompt)) && !run_has_mutating_action(r))
        unfinished = 1;
    if (stalled || synthesized || fail_aborted || r->tool_fail_aborted ||
        r->unresolved_failed_tool_count > 0 || r->unresolved_failed_tools_overflow ||
        (prompt_requires_full_read(completion_instruction(r,prompt)) &&
         (required_pending_reads(r,completion_instruction(r,prompt)) || r->read_tracking_overflow ||
          answer_reports_required_unread(r,completion_instruction(r,prompt),final_text))))
        unfinished = 1;
    if (unfinished)
        st = ST_FAILED;
    strbuf_init(&out);
    /* user-visible answer = the model's final text ONLY. Raw tool output and
     * [tool]/action logs are execution details (visible live via
     * process_log / the UI's process panel), never part of the answer. The
     * obs_log is a fallback for runs that ended without a final answer. */
    if (cancelled) {
        strbuf_append(&out, "任务已取消。取消前的执行记录和已完成步骤已保留，可在“详细执行记录”中查看。");
    } else if (timed_out) {
        strbuf_append(&out, "任务执行超时。超时前的执行记录和已完成步骤已保留，可在“详细执行记录”中查看。");
    } else if (unfinished) {
        strbuf_append(&out, "任务未完成：尚未完成请求的阅读、操作或验证，当前结果仅为部分进展。");
        int explained=0;
        if(r->planner_native_end_turn==2) {
            strbuf_append(&out,"\n原因：模型通过结构化完成声明报告任务受阻，仍有未完成项。");explained=1;
        }
        if (r->last_model_error[0]) {
            strbuf_appendf(&out,"\n模型 %s 请求失败：%s",r->llm && r->llm->model ? r->llm->model : "unknown",r->last_model_error);
            explained=1;
        }
        if (output_truncated_unresolved) {
            strbuf_append(&out,"\n原因：模型连续达到输出 token 上限，已尝试续写但答复仍不完整；保留部分内容，请恢复任务继续生成。");
            explained=1;
        }
        const char *instruction=completion_instruction(r,prompt);
        if (prompt_requires_mutation(instruction) && !run_has_mutating_action(r)) {
            strbuf_append(&out,"\n原因：尚无成功执行所请求操作的工具记录；读取文件或加载技能指令不能代替实际操作。");
            if ((strstr(instruction,"注册") || strstr(instruction,"register ")) &&
                (strstr(instruction,"skill") || strstr(instruction,"技能")))
                strbuf_append(&out," 请使用 skill_manage 注册技能并核对结果。");
            explained=1;
        }
        if (required_pending_reads(r,instruction)) {
            strbuf_append(&out,"\n原因：以下文件尚未连续读取到 EOF；继续使用 file_read 和所列 offset_bytes：");
            for(int i=0,shown=0;i<r->pending_reads_n && shown<8;i++) {
                if(!path_requires_full_read(r,instruction,r->pending_reads[i].path)) continue;
                strbuf_appendf(&out,"\n- %s：offset_bytes=%llu",r->pending_reads[i].path,r->pending_reads[i].next);
                shown++;
            }
            explained=1;
        }
        if (r->tool_fail_aborted) { strbuf_append(&out,"\n原因：相同工具连续失败3次；请查看最后一次失败的工具结果，修正参数或环境后恢复任务。"); explained=1; }
        if (r->unresolved_failed_tool_count > 0 || r->unresolved_failed_tools_overflow) {
            strbuf_append(&out, "\n原因：以下失败工具之后没有成功重试核实，不能将任务标记为完成：");
            for (int i = 0; i < r->unresolved_failed_tool_count; ++i)
                strbuf_appendf(&out, "%s%s", i ? "、" : " ", r->unresolved_failed_tools[i]);
            if (r->unresolved_failed_tools_overflow)
                strbuf_append(&out, "（失败工具记录过多，已按未完成处理）");
            strbuf_append(&out, "。请查看详细执行记录并恢复验证。");
            explained=1;
        }
        if (stalled) { strbuf_append(&out,cycle_aborted ?
            "\n原因：多轮交替读取或检查持续返回已有结果；循环恢复提示后仍无新增进展，已停止重复调用并保留记录。" :
            "\n原因：重复工具动作没有新增进展；需调整路径、分页参数或执行方案。"); explained=1; }
        if (fail_aborted && !r->visual_request_failed) { strbuf_append(&out,"\n原因：模型规划或执行验证连续失败；请核对执行记录中的最后一条错误。"); explained=1; }
        if (r->visual_request_failed) explained=1;
        if (r->round_idx>=r->max_rounds) { strbuf_appendf(&out,"\n原因：达到配置的最大规划轮数 %d；当前步骤未全部完成。",r->max_rounds); explained=1; }
        if (!explained) strbuf_append(&out,"\n原因：模型仍在描述待执行工作，尚未提交可验证的最终结果；请根据保留的执行记录继续任务。");

        if (final_text && *final_text) {
            strbuf_append(&out, "\n\n");
            if (r->unresolved_failed_tool_count > 0 || r->unresolved_failed_tools_overflow)
                strbuf_appendf(&out, "模型阶段性总结（失败调用尚未核实，不代表任务完成）：\n%s", final_text);
            else
                strbuf_append(&out, final_text);
        }
    } else if (final_text && *final_text) {
        strbuf_append(&out, final_text);
    } else if (r->obs_log_len > 0) {
        /* no final answer produced — fall back to the executed-action log.
         * issue #8: cap with head+tail elision (recent actions matter most),
         * line-boundary safe, with an explicit marker for the elided middle. */
        const size_t head_keep = 2048, tail_keep = 12288;
        if (r->obs_log_len <= head_keep + tail_keep + 64) {
            strbuf_append(&out, r->obs_log);
        } else {
            size_t head = head_keep, tail_start = r->obs_log_len - tail_keep;
            while (head < r->obs_log_len && r->obs_log[head] != '\n')
                head++;
            if (head < r->obs_log_len)
                head++;
            while (tail_start > 0 && r->obs_log[tail_start - 1] != '\n')
                tail_start--;
            strbuf_append_n(&out, r->obs_log, head);
            strbuf_appendf(&out, "\n...[%zu bytes of earlier action results elided]...\n",
                           tail_start - head);
            strbuf_append_n(&out, r->obs_log + tail_start, r->obs_log_len - tail_start);
        }
    }
    if (!final_text || !*final_text) {
        if (r->round_log_len > 0 && r->obs_log_len == 0) {
            if (stalled)
                strbuf_appendf(&out, "(连续两轮计划相同，已停止；任务可能未完全完成，第 %d/%d 轮)", r->round_idx,
                               r->max_rounds);
            else
                strbuf_appendf(&out, "(已达到最大轮数 %d，任务可能未完全完成)", r->max_rounds);
        }
    }

    if (fail_aborted && !r->visual_request_failed && consec_fail >= REASONING_CONSEC_FAIL_ABORT)
        strbuf_appendf(&out, "\n(连续 %s 轮阶段失败，任务中止 — 请根据执行记录中的真实错误修正后重试)",
                       REASONING_CONSEC_FAIL_ABORT_STR);

    free(final_text);
    free(truncated_prefix);
    free(result);
    free(last_narration);
    free(r->last_plan_raw);
    r->last_plan_raw = NULL;
    free(r->prev_plan);
    r->prev_plan = NULL;
    r->last_plan_action_sig = r->prev_plan_action_sig = 0;
    combined = strbuf_detach(&out);

    /* accumulate execution duration into the session state (会话执行时长) and
     * persist it — covers both DONE and FAILED runs */
    if (r->cur) {
        mutex_lock(&r->cur->mtx);
        r->cur->total_ms += (long long)time_now_ms() - run_t0;
        mutex_unlock(&r->cur->mtx);
        mutex_lock(&r->ss->sess_mtx);
        meta_upsert_locked(r, r->cur);
        meta_save_locked(r);
        mutex_unlock(&r->ss->sess_mtx);
    }

    if (r->metrics)
        metrics_inc(r->metrics, cancelled ? "tasks.cancelled" : timed_out ? "tasks.timed_out" :
                    st == ST_DONE ? "tasks.done" : "tasks.failed");

    if (st == ST_DONE) {
        /* A forced summary or a stalled loop is useful to the user, but it
         * does not prove that the task completed. Keep it out of shared
         * memory so another session cannot inherit unfinished work. */
        if (!synthesized && !stalled && !fail_aborted && !r->tool_fail_aborted) {
            memory_record_completed_run(r, combined);
            if (r->mem && atomic_load(&r->ss->shared_memory_global)) {
                if (r->memsvc) memory_service_remember(r->memsvc, MEM_WORKING, NULL, combined);
                else memory_working_push(r->mem, combined);
            }
        }
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
        if (r->hooks && !cancelled && !timed_out) {
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

    usage_book_round(r); /* include a final synthesis request, even on failure */
    free(safe_prompt);
    mutex_lock(&r->ss->sess_mtx);
    if (r->cur)
        r->cur->in_run = 0;
    mutex_unlock(&r->ss->sess_mtx);
    progress_emit(r, cancelled ? "cancelled" : timed_out ? "timed_out" :
                  ret == 0 ? "completed" : "failed");
    /* The terminal snapshot must still describe the model actually called. */
    if (picked) {
        r->llm = saved;
        llm_destroy(picked);
    }
    return ret;
}

/* Session-memory snapshot: fixed-section notes + compaction state (JSON). */
/* Live progress snapshot of the current (or most recently finished) run.
 * Fields are read without a lock — display-grade accuracy only. tokens are
 * cumulative LLM usage on the active provider instance. */
void reasoning_progress(reasoning *r, long long *elapsed_ms, int *round, int *tool_calls,
                        const char **cur_tool, long long *tokens_in, long long *tokens_out) {
    reasoning_progress_ex(r, elapsed_ms, round, tool_calls, cur_tool, tokens_in, tokens_out, NULL, NULL, NULL);
}

/* Extended progress snapshot; llm_ms/tool_ms/llm_calls are cumulative timing
 * of planner LLM calls and tool executions within the current run (issue #6). */
void reasoning_progress_ex(reasoning *r, long long *elapsed_ms, int *round, int *tool_calls,
                           const char **cur_tool, long long *tokens_in, long long *tokens_out,
                           long long *llm_ms, long long *tool_ms, int *llm_calls) {
    static _Thread_local char tool_copy[64];
    cJSON *o = NULL;
    if (r) {
        mutex_lock(&r->progress_mtx);
        if (r->progress_json) o = cJSON_Parse(r->progress_json);
        mutex_unlock(&r->progress_mtx);
    }
#define READ_NUM(key) (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, key)))
#define NUM(key) (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(o, key)) ? READ_NUM(key) : 0)
    long long started = (long long)NUM("started_ms");
    if (elapsed_ms) *elapsed_ms = started ? time_now_ms() - started : 0;
    if (round) *round = (int)NUM("round");
    if (tool_calls) *tool_calls = (int)NUM("tool_calls");
    const char *tool = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "cur_tool"));
    snprintf(tool_copy, sizeof(tool_copy), "%s", tool ? tool : "");
    if (cur_tool) *cur_tool = tool_copy;
    if (tokens_in) *tokens_in = (long long)NUM("tokens_in");
    if (tokens_out) *tokens_out = (long long)NUM("tokens_out");
    if (llm_ms) *llm_ms = (long long)NUM("llm_ms");
    if (tool_ms) *tool_ms = (long long)NUM("tool_ms");
    if (llm_calls) *llm_calls = (int)NUM("llm_calls");
#undef NUM
#undef READ_NUM
    cJSON_Delete(o);
}

char *reasoning_round_log_tail(reasoning *r, size_t max_bytes) {
    if (!r || !max_bytes) return NULL;
    mutex_lock(&r->progress_mtx);
    size_t start = r->round_log_len > max_bytes ? r->round_log_len - max_bytes : 0;
    if (start) {
        while (start < r->round_log_len && r->round_log[start] != '\n') start++;
        if (start < r->round_log_len) start++;
    }
    char *copy = r->round_log && r->round_log_len ? xstrdup(r->round_log + start) : NULL;
    mutex_unlock(&r->progress_mtx);
    return copy;
}

/* Steps of the current/most recent run as a JSON array of
 * {tool,args,out,ok,ms} (Claude-Code style execution display). Caller frees. */
char *reasoning_steps_json(reasoning *r) {
    cJSON *arr;
    char *js;

    if (!r)
        return NULL;
    arr = cJSON_CreateArray();
    if (!arr)
        return NULL;
    mutex_lock(&r->progress_mtx);
    for (int i = 0; i < r->n_steps; i++) {
        const struct run_step *st = &r->steps[i];
        cJSON *o = cJSON_CreateObject();
        if (!o)
            break;
        cJSON_AddStringToObject(o, "tool", st->tool);
        cJSON_AddNumberToObject(o, "id", st->id);
        char *args = secret_redact_ssh_passwords(st->args ? st->args : "", st->args ? strlen(st->args) : 0, NULL);
        char *output = secret_redact_ssh_passwords(st->out ? st->out : "", st->out ? strlen(st->out) : 0, NULL);
        cJSON_AddStringToObject(o, "args", args ? args : "");
        cJSON_AddStringToObject(o, "out", output ? output : "");
        if (st->target[0]) {
            char *target = secret_redact_ssh_passwords(st->target, strlen(st->target), NULL);
            cJSON_AddStringToObject(o, "target", target ? target : "");
            free(target);
        }
        free(args); free(output);
        cJSON_AddNumberToObject(o, "ok", st->ok);
        cJSON_AddNumberToObject(o, "ms", st->ms);
        cJSON_AddItemToArray(arr, o);
    }
    mutex_unlock(&r->progress_mtx);
    js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return js;
}

char *reasoning_session_json(reasoning *r) {
    cJSON *o;
    char *s;

    if (!r)
        return xstrdup("{}");
    o = cJSON_CreateObject();
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
    s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s ? s : xstrdup("{}");
}

/* HyDE primitive: LLM writes a hypothetical answer passage for `query`; the
 * caller embeds passage-to-passage for retrieval (see reasoning.h). */
char *hyde_passage(llm *llm, const char *query) {
    char prompt[1200];

    if (!llm || !query || !*query)
        return NULL;
    snprintf(prompt, sizeof(prompt),
             "Write a short passage (3-5 sentences) that directly answers the "
             "question. Output only the passage, no preamble.\n\nQuestion: %.900s",
             query);
    return llm_chat_simple(llm,
                               "You generate concise hypothetical answer passages "
                               "used for semantic retrieval.",
                               prompt);
}
