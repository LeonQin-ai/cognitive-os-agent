/* orchestrator.c — auto-mode multi-agent orchestration on top of the Flow
 * engine.
 *
 * cagent_flow_decompose: the LLM splits a top-level task into 2-4 subtasks
 * assigned to registered agents and compiles them into a Flow DAG JSON (one
 * parallel node per subtask). The caller can inspect/modify the DAG before
 * running it with ca_flow_run.
 *
 * cagent_orchestrate: decompose + ca_flow_run (one isolated reasoning
 * instance per node, parallel) + a final LLM merge into a single answer.
 * Falls back to a plain single-agent run when no agents are registered or
 * the plan is unparseable. */
#include "cagent/cagent.h"
#include "cagent/runtime/flow.h"
#include "cagent/infra/util.h"
#include "cagent/infra/logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define ORCH_MAX_STEPS 4

/* Roster lines for the decompose prompt, from the pool snapshot JSON.
 * Returns malloc'd text ("name (role)" per line) or NULL. */
static char *roster_text(cagent_ctx *ctx) {
    char *snap = ca_agent_pool_snapshot_json(ctx->agents);
    if (!snap) return NULL;
    cJSON *root = cJSON_Parse(snap);
    free(snap);
    if (!root) return NULL;
    cJSON *agents = cJSON_GetObjectItemCaseSensitive(root, "agents");
    char *out = NULL;
    if (cJSON_IsArray(agents) && cJSON_GetArraySize(agents) > 0) {
        size_t cap = 256, len = 0;
        out = (char *)malloc(cap);
        if (out) {
            out[0] = '\0';
            cJSON *a;
            cJSON_ArrayForEach(a, agents) {
                cJSON *n = cJSON_GetObjectItemCaseSensitive(a, "name");
                cJSON *r = cJSON_GetObjectItemCaseSensitive(a, "role");
                const char *name = (n && cJSON_IsString(n)) ? n->valuestring : "";
                const char *role = (r && cJSON_IsString(r)) ? r->valuestring : "";
                size_t need = strlen(name) + strlen(role) + 8;
                if (len + need + 1 > cap) {
                    cap = (len + need + 1) * 2;
                    char *nb = (char *)realloc(out, cap);
                    if (!nb) { free(out); out = NULL; break; }
                    out = nb;
                }
                len += (size_t)snprintf(out + len, cap - len, "- %s (%s)\n", name, role);
            }
        }
    }
    cJSON_Delete(root);
    return out;
}

/* Parse the LLM's plan output into steps. Only entries whose agent is
 * registered are kept. Returns the number of valid steps (0 = parse fail). */
static int parse_plan(cagent_ctx *ctx, const char *raw, char agents[][64],
                      char tasks[][512]) {
    if (!raw) return 0;
    const char *lb = strchr(raw, '[');
    const char *rb = strrchr(raw, ']');
    if (!lb || !rb || rb < lb) return 0;
    size_t alen = (size_t)(rb - lb + 1);
    char *arr_txt = (char *)malloc(alen + 1);
    if (!arr_txt) return 0;
    memcpy(arr_txt, lb, alen);
    arr_txt[alen] = '\0';
    cJSON *arr = cJSON_Parse(arr_txt);
    free(arr_txt);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return 0; }
    int n = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= ORCH_MAX_STEPS) break;
        if (!cJSON_IsObject(it)) continue;
        cJSON *a = cJSON_GetObjectItemCaseSensitive(it, "agent");
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "task");
        if (!a || !cJSON_IsString(a) || !a->valuestring || !*a->valuestring) continue;
        if (!t || !cJSON_IsString(t) || !t->valuestring || !*t->valuestring) continue;
        if (ca_agent_pool_find(ctx->agents, a->valuestring) < 0) continue; /* unknown */
        snprintf(agents[n], 64, "%s", a->valuestring);
        snprintf(tasks[n], 512, "%s", t->valuestring);
        n++;
    }
    cJSON_Delete(arr);
    return n;
}

/* Compile a parsed plan into a Flow DAG JSON document (nodes in parallel,
 * no edges — the merge happens after the run). Returns malloc'd JSON. */
static char *build_dag_json(char agents[][64], char tasks[][512], int n) {
    cJSON *root = cJSON_CreateObject();
    cJSON *nodes = cJSON_CreateArray();
    if (!root || !nodes) { cJSON_Delete(root); cJSON_Delete(nodes); return NULL; }
    for (int i = 0; i < n; i++) {
        cJSON *nd = cJSON_CreateObject();
        char id[16];
        snprintf(id, sizeof(id), "step%d", i + 1);
        cJSON_AddStringToObject(nd, "id", id);
        cJSON_AddStringToObject(nd, "agent", agents[i]);
        cJSON_AddStringToObject(nd, "task", tasks[i]);
        cJSON_AddItemToArray(nodes, nd);
    }
    cJSON_AddItemToObject(root, "nodes", nodes);
    cJSON_AddItemToObject(root, "edges", cJSON_CreateArray());
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* LLM decomposition: roster + task in, parsed plan out. Returns the number of
 * valid steps (0 = no roster / no parseable plan). */
static int decompose_task(cagent_ctx *ctx, const char *task,
                          char (*agents)[64], char (*tasks)[512]) {
    char *roster = ctx->llm && ctx->agents && ca_agent_pool_count(ctx->agents) > 0
                       ? roster_text(ctx) : NULL;
    int nsteps = 0;
    if (roster && *roster) {
        char sys[] =
            "你是多 agent 编排器。把用户任务分解为 2-4 个子任务并分配给可用的 agent。"
            "只输出 JSON 数组，格式：[{\"agent\":\"agent 名\",\"task\":\"子任务描述\"}]，"
            "不要输出任何其他文字。";
        size_t ulen = strlen(roster) + strlen(task) + 64;
        char *user = (char *)malloc(ulen);
        if (user) {
            snprintf(user, ulen, "可用 agent:\n%s\n任务: %s", roster, task);
            char *raw = ca_llm_chat_simple(ctx->llm, sys, user);
            free(user);
            if (raw) {
                nsteps = parse_plan(ctx, raw, agents, tasks);
                free(raw);
            }
        }
    }
    free(roster);
    return nsteps;
}

/* Compile a task into a Flow DAG without executing it (0 ok, -1 no plan).
 * *dag_json receives a malloc'd {"nodes":[...],"edges":[]} document. */
int cagent_flow_decompose(cagent_ctx *ctx, const char *task, char **dag_json) {
    if (!ctx || !task || !*task || !dag_json) return -1;
    *dag_json = NULL;
    char (*agents)[64] = calloc(ORCH_MAX_STEPS, sizeof(*agents));
    char (*tasks)[512] = calloc(ORCH_MAX_STEPS, sizeof(*tasks));
    if (!agents || !tasks) { free(agents); free(tasks); return -1; }
    int nsteps = decompose_task(ctx, task, agents, tasks);
    if (nsteps == 0) {
        free(agents);
        free(tasks);
        return -1;
    }
    *dag_json = build_dag_json(agents, tasks, nsteps);
    free(agents);
    free(tasks);
    return *dag_json ? 0 : -1;
}

int cagent_orchestrate(cagent_ctx *ctx, const char *task, char **answer,
                       char **trace_json) {
    if (!ctx || !task || !*task || !answer) return -1;
    *answer = NULL;
    if (trace_json) *trace_json = NULL;

    /* ---- DECOMPOSE (skipped with no roster) ---- */
    char (*agents)[64] = calloc(ORCH_MAX_STEPS, sizeof(*agents));
    char (*tasks)[512] = calloc(ORCH_MAX_STEPS, sizeof(*tasks));
    if (!agents || !tasks) { free(agents); free(tasks); return -1; }
    int nsteps = decompose_task(ctx, task, agents, tasks);

    if (nsteps == 0) {
        /* fallback: no agents / no parseable plan → plain single-agent run */
        free(agents);
        free(tasks);
        ca_log_info("orchestrator: no multi-agent plan, running single-agent");
        return cagent_run(ctx, task, answer);
    }
    char *dag = build_dag_json(agents, tasks, nsteps);
    free(agents);
    free(tasks);
    if (!dag) return -1;

    /* ---- EXECUTE through the Flow engine (parallel, isolated per node) ---- */
    char *flow_answer = NULL;
    char *trace = NULL;
    int rc = ca_flow_run(ctx, dag, &flow_answer, &trace);
    free(dag);
    if (rc != 0) return -1;

    /* ---- MERGE into the final answer ---- */
    char *final = NULL;
    char *merged = NULL;
    if (trace) {
        cJSON *arr = cJSON_Parse(trace);
        if (arr) {
            ca_strbuf b;
            ca_strbuf_init(&b);
            ca_strbuf_appendf(&b, "任务: %s\n\n各 agent 结果:\n", task);
            cJSON *it;
            int i = 1;
            cJSON_ArrayForEach(it, arr) {
                cJSON *a = cJSON_GetObjectItemCaseSensitive(it, "agent");
                cJSON *r = cJSON_GetObjectItemCaseSensitive(it, "result");
                const char *ag = (a && cJSON_IsString(a)) ? a->valuestring : "?";
                const char *rs = (r && cJSON_IsString(r)) ? r->valuestring : "";
                ca_strbuf_appendf(&b, "%d. [%s] %s\n", i++, ag, rs);
            }
            cJSON_Delete(arr);
            merged = b.buf;
        }
    }
    if (ctx->llm && merged) {
        char sys2[] =
            "你是编排器。综合各 agent 的子任务结果，针对任务给出最终统一答案。"
            "直接输出答案正文，不要罗列过程。";
        final = ca_llm_chat_simple(ctx->llm, sys2, merged);
    }
    if (!final || !*final)
        final = ca_strdup(merged && *merged ? merged : flow_answer);

    if (trace_json && trace) *trace_json = ca_strdup(trace);
    ca_blackboard_put(ctx->blackboard, "flow/final", final);
    free(trace);
    free(merged);
    free(flow_answer);

    *answer = final;
    return 0;
}
