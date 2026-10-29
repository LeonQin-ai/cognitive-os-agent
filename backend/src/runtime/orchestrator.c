/* orchestrator.c — auto-mode multi-agent orchestration on top of the Flow
 * engine.
 *
 * flow_decompose: the LLM splits a top-level task into 2-4 subtasks
 * assigned to registered agents and compiles them into a Flow DAG JSON (one
 * parallel node per subtask). The caller can inspect/modify the DAG before
 * running it with flow_run.
 *
 * orchestrate: decompose + flow_run (one isolated reasoning
 * instance per node, parallel) + a final LLM merge into a single answer.
 * Falls back to a plain single-agent run when no agents are registered or
 * the plan is unparseable. */
#include "cognitive-os-agent.h"
#include "runtime/flow.h"
#include "infra/util.h"
#include "infra/logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define ORCH_MAX_STEPS 4
#define ORCH_MAX_TASKLEN 8192

/* llm_chat_simple + book the token delta into the per-model usage ledger.
 * Decompose/merge call the LLM directly, bypassing the reasoning loop's
 * per-round accounting — without this they would never show on the dashboard. */
static char *llm_simple_booked(runtime_ctx *ctx, const char *sys, const char *user) {
    long long bin = 0, bout = 0, tin = 0, tout = 0, bren = 0, tren = 0;
    char *out;

    if (!ctx || !ctx->llm)
        return NULL;
    long long before_reports=0, after_reports=0;
    llm_usage_thread_totals(&bin, &bout, &bren, &before_reports);
    out = llm_chat_simple(ctx->llm, sys, user);
    llm_usage_thread_totals(&tin, &tout, &tren, &after_reports);
    if (ctx->usage && after_reports > before_reports) {
        long long rd = tren - bren;
        if (rd < 0)
            rd = 0;
        long long vd = (tout - bout) - rd;
        usage_add_ex(ctx->usage, ctx->llm->model ? ctx->llm->model : "?",
                     (long)(tin - bin), (long)(vd > 0 ? vd : 0), (long)rd);
    }
    return out;
}

/* Roster lines for the decompose prompt, from the pool snapshot JSON.
 * Returns malloc'd text ("name (role)" per line) or NULL. */
static int scope_has(cJSON *allowed,const char *name) {
    if(!allowed)return 1;
    cJSON *item;cJSON_ArrayForEach(item,allowed)
        if(cJSON_IsString(item)&&!strcmp(item->valuestring,name))return 1;
    return 0;
}

/* Persist the execution roster with the task so resume cannot broaden it. */
char *flow_scope_task(runtime_ctx *ctx,const char *task,const char *agents_json) {
    if(!agents_json)return xstrdup(task);
    cJSON *allowed=cJSON_Parse(agents_json),*item;
    if(!cJSON_IsArray(allowed)||!cJSON_GetArraySize(allowed)){cJSON_Delete(allowed);return NULL;}
    cJSON_ArrayForEach(item,allowed)if(!cJSON_IsString(item)||agent_pool_find(ctx->agents,item->valuestring)<0){cJSON_Delete(allowed);return NULL;}
    cJSON *scope=cJSON_CreateObject();cJSON_AddNumberToObject(scope,"coa_flow_scope",1);
    cJSON_AddStringToObject(scope,"task",task);cJSON_AddItemToObject(scope,"agents",allowed);
    char *out=cJSON_PrintUnformatted(scope);cJSON_Delete(scope);return out;
}

static char *roster_text(runtime_ctx *ctx,cJSON *allowed) {
    char *snap = agent_pool_snapshot_json(ctx->agents);
    cJSON *root;
    cJSON *agents;
    char *out = NULL;

    if (!snap)
        return NULL;
    root = cJSON_Parse(snap);
    free(snap);
    if (!root)
        return NULL;
    agents = cJSON_GetObjectItemCaseSensitive(root, "agents");
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
                if(!scope_has(allowed,name))continue;
                size_t need = strlen(name) + strlen(role) + 8;
                if (len + need + 1 > cap) {
                    cap = (len + need + 1) * 2;
                    char *nb = (char *)realloc(out, cap);
                    if (!nb) {
                        free(out);
                        out = NULL;
                        break;
                    }
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
 * registered are kept. `after[][ORCH_MAX_STEPS]` receives 0-based prerequisite
 * step indices per step (1-based "after" in the plan text, converted here).
 * Returns the number of valid steps (0 = parse fail). */
static int parse_plan(runtime_ctx *ctx, const char *raw, char agents[][64], char tasks[][ORCH_MAX_TASKLEN],
                      int after[][ORCH_MAX_STEPS]) {
    size_t alen;
    char *arr_txt;
    cJSON *arr;
    int n = 0;
    cJSON *it;

    if (!raw)
        return 0;
    const char *lb = strchr(raw, '[');
    const char *rb = strrchr(raw, ']');
    if (!lb || !rb || rb < lb)
        return 0;
    alen = (size_t)(rb - lb + 1);
    arr_txt = (char *)malloc(alen + 1);
    if (!arr_txt)
        return 0;
    memcpy(arr_txt, lb, alen);
    arr_txt[alen] = '\0';
    arr = cJSON_Parse(arr_txt);
    free(arr_txt);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return 0;
    }

    if(cJSON_GetArraySize(arr)>ORCH_MAX_STEPS) { cJSON_Delete(arr); return -1; }
    cJSON_ArrayForEach(it, arr) {
    cJSON *a;
    cJSON *t;
    cJSON *jaf;

        if (n >= ORCH_MAX_STEPS)
            break;
        if (!cJSON_IsObject(it)) { cJSON_Delete(arr); return -1; }
        a = cJSON_GetObjectItemCaseSensitive(it, "agent");
        t = cJSON_GetObjectItemCaseSensitive(it, "task");
        if (!a || !cJSON_IsString(a) || !a->valuestring || !*a->valuestring) { cJSON_Delete(arr); return -1; }
        if (!t || !cJSON_IsString(t) || !t->valuestring || !*t->valuestring) { cJSON_Delete(arr); return -1; }
        if (agent_pool_find(ctx->agents, a->valuestring) < 0 || strlen(a->valuestring)>=64 || strlen(t->valuestring)>=ORCH_MAX_TASKLEN) { cJSON_Delete(arr); return -1; }
        snprintf(agents[n], 64, "%s", a->valuestring);
        snprintf(tasks[n], ORCH_MAX_TASKLEN, "%s", t->valuestring);
        /* "after": [1-based step indices] → 0-based, forward-only, deduped;
         * -1 terminated (calloc'd 0xFF == -1) */
        memset(after[n], 0xFF, sizeof(after[n]));
        jaf = cJSON_GetObjectItemCaseSensitive(it, "after");
        if (!jaf && n>0) after[n][0]=n-1;
        if (jaf && !cJSON_IsArray(jaf)) { cJSON_Delete(arr); return -1; }
        if (jaf && cJSON_IsArray(jaf)) {
            cJSON *jd;
            cJSON_ArrayForEach(jd, jaf) {
    int idx;
    int dup = 0;

                if (!cJSON_IsNumber(jd) || jd->valuedouble<1 || jd->valuedouble>n || jd->valuedouble!=(int)jd->valuedouble) { cJSON_Delete(arr); return -1; }
                idx = (int)jd->valuedouble - 1; /* 1-based → 0-based */
                if (idx < 0 || idx >= n)
                    continue; /* only earlier steps */
                for (int k = 0; k < ORCH_MAX_STEPS && after[n][k] >= 0; k++)
                    if (after[n][k] == idx) {
                        dup = 1;
                        break;
                    }

                if (!dup) {
                    for (int k = 0; k < ORCH_MAX_STEPS; k++) {
                        if (after[n][k] < 0) {
                            after[n][k] = idx;
                            break;
                        }
                    }
                }
            }
        }

        n++;
    }

    cJSON_Delete(arr);
    return n;
}

/* Compile each step's validated prerequisites. Missing after defaults to
 * the previous step; explicit after:[] preserves independent parallelism. */
static char *build_dag_json(char agents[][64], char tasks[][ORCH_MAX_TASKLEN], int after[][ORCH_MAX_STEPS], int n) {
    cJSON *root=cJSON_CreateObject(), *nodes=cJSON_CreateArray(), *edges=cJSON_CreateArray();
    if(!root || !nodes || !edges) { cJSON_Delete(root); cJSON_Delete(nodes); cJSON_Delete(edges); return NULL; }
    cJSON_AddItemToObject(root,"nodes",nodes); cJSON_AddItemToObject(root,"edges",edges);
    for(int i=0;i<n;i++) {
        char id[16]; snprintf(id,sizeof(id),"step%d",i+1);
        cJSON *node=cJSON_CreateObject();
        if(!node) { cJSON_Delete(root); return NULL; }
        cJSON_AddStringToObject(node,"id",id); cJSON_AddStringToObject(node,"agent",agents[i]);
        cJSON_AddStringToObject(node,"task",tasks[i]); cJSON_AddItemToArray(nodes,node);
        for(int k=0;k<ORCH_MAX_STEPS && after[i][k]>=0;k++) {
            char from[16]; snprintf(from,sizeof(from),"step%d",after[i][k]+1);
            cJSON *edge=cJSON_CreateObject(); if(!edge) { cJSON_Delete(root); return NULL; }
            cJSON_AddStringToObject(edge,"from",from); cJSON_AddStringToObject(edge,"to",id); cJSON_AddItemToArray(edges,edge);
        }
    }
    char *out=cJSON_PrintUnformatted(root); cJSON_Delete(root); return out;
}

/* LLM decomposition: roster + task in, parsed plan out. Returns the number of
 * valid steps (0 = no roster / no parseable plan). */
static int decompose_task(runtime_ctx *ctx, const char *task, char (*agents)[64], char (*tasks)[ORCH_MAX_TASKLEN],
                          int (*after)[ORCH_MAX_STEPS]) {
    cJSON *scope=cJSON_Parse(task),*allowed=NULL;
    if(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(scope,"coa_flow_scope"))){
        allowed=cJSON_GetObjectItemCaseSensitive(scope,"agents");
        const char *original=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(scope,"task"));
        if(!original||!cJSON_IsArray(allowed)||!cJSON_GetArraySize(allowed)){cJSON_Delete(scope);return -1;}
        cJSON *item;cJSON_ArrayForEach(item,allowed)if(!cJSON_IsString(item)||agent_pool_find(ctx->agents,item->valuestring)<0){cJSON_Delete(scope);return -1;}
        task=original;
    }
    char *roster = ctx->llm && ctx->agents && agent_pool_count(ctx->agents) > 0 ? roster_text(ctx,allowed) : NULL;
    int nsteps = 0;
    if (roster && *roster) {
        char sys[] = "你是多 agent 编排器。把用户任务分解为 1-4 个必要的子任务，仅分配给所列可用 agent。按任务需要选择最少角色，不必使用所有角色；一个角色足够时只生成一个步骤。"
                     "只输出 JSON 数组，格式：[{\"agent\":\"agent 名\",\"task\":\"子任务描述\","
                     "\"after\":[1,2]}]。"
                     "\"after\" 是本步骤依赖的前置步骤编号（从 1 开始，按数组顺序编号），"
                     "省略 after 表示依赖前一步；确实独立可并行的步骤必须显式 after:[]。分析类步骤在前，修复/执行类步骤必须声明依赖分析步骤，"
                     "验证类步骤必须声明依赖修复步骤。"
                     "子任务描述中可用 {{stepN}} 占位符引用第 N 步的输出结果"
                     "（如 {{step1}} 运行时会替换为步骤 1 的结果）。不要输出任何其他文字。";
        size_t ulen = strlen(roster) + strlen(task) + 64;
        char *user = (char *)malloc(ulen);
        if (user) {
            snprintf(user, ulen, "可用 agent:\n%s\n任务: %s", roster, task);
            char *raw = llm_simple_booked(ctx, sys, user);
            free(user);
            if (raw) {
                log_info("orchestrator: decompose raw: %s", raw);
                nsteps = parse_plan(ctx, raw, agents, tasks, after);
                for(int i=0;i<nsteps;i++)if(!scope_has(allowed,agents[i])){nsteps=-1;break;}
                log_info("orchestrator: parsed %d steps", nsteps);
                free(raw);
            }
        }
    }

    free(roster);
    if(allowed && nsteps<=0)nsteps=-1; /* never fall back outside a restricted roster */
    cJSON_Delete(scope);
    return nsteps;
}

/* Compile a task into a Flow DAG without executing it (0 ok, -1 no plan).
 * *dag_json receives a malloc'd {"nodes":[...],"edges":[]} document. */
int flow_decompose(runtime_ctx *ctx, const char *task, char **dag_json) {
    int nsteps;

    if (!ctx || !task || !*task || !dag_json)
        return -1;
    *dag_json = NULL;
    char (*agents)[64] = calloc(ORCH_MAX_STEPS, sizeof(*agents));
    char (*tasks)[ORCH_MAX_TASKLEN] = calloc(ORCH_MAX_STEPS, sizeof(*tasks));
    int (*after)[ORCH_MAX_STEPS] = calloc(ORCH_MAX_STEPS, sizeof(*after));
    if (!agents || !tasks || !after) {
        free(agents);
        free(tasks);
        free(after);
        return -1;
    }

    nsteps = decompose_task(ctx, task, agents, tasks, after);
    if (nsteps <= 0) {
        free(agents);
        free(tasks);
        free(after);
        return -1;
    }

    *dag_json = build_dag_json(agents, tasks, after, nsteps);
    free(agents);
    free(tasks);
    free(after);
    return *dag_json ? 0 : -1;
}

static void orchestration_stage(runtime_ctx *ctx,int64_t id,const char *stage,const char *activity){
    if(id<0)return;task *t=scheduler_acquire(ctx->scheduler,id);if(!t)return;
    char *old=task_progress_copy(t);cJSON *o=old?cJSON_Parse(old):NULL;free(old);if(!o)o=cJSON_CreateObject();
    cJSON_DeleteItemFromObject(o,"stage");cJSON_AddStringToObject(o,"stage",stage);
    cJSON_DeleteItemFromObject(o,"activity");cJSON_AddStringToObject(o,"activity",activity);
    char *nodes=flow_progress_json(id);cJSON *arr=nodes?cJSON_Parse(nodes):NULL;free(nodes);
    if(arr){cJSON_DeleteItemFromObject(o,"flow_nodes");cJSON_AddItemToObject(o,"flow_nodes",arr);}
    char *json=cJSON_PrintUnformatted(o);if(json)task_set_progress(t,json);free(json);cJSON_Delete(o);scheduler_release(ctx->scheduler,t);
}
int orchestrate(runtime_ctx *ctx,const char *task,char **answer,char **trace_json){
    return orchestrate_task(ctx,task,-1,answer,trace_json);
}
int orchestrate_task(runtime_ctx *ctx, const char *task,int64_t task_id, char **answer, char **trace_json) {
    int nsteps;
    char *dag;
    /* ---- EXECUTE through the Flow engine (parallel, isolated per node) ---- */
    char *flow_answer = NULL;
    char *trace = NULL;
    int rc;
    /* ---- MERGE into the final answer ---- */
    char *final = NULL;
    char *merged = NULL;

    if (!ctx || !task || !*task || !answer)
        return -1;
    *answer = NULL;
    if (trace_json)
        *trace_json = NULL;

    /* ---- DECOMPOSE (skipped with no roster) ---- */
    char (*agents)[64] = calloc(ORCH_MAX_STEPS, sizeof(*agents));
    char (*tasks)[ORCH_MAX_TASKLEN] = calloc(ORCH_MAX_STEPS, sizeof(*tasks));
    int (*after)[ORCH_MAX_STEPS] = calloc(ORCH_MAX_STEPS, sizeof(*after));
    if (!agents || !tasks || !after) {
        free(agents);
        free(tasks);
        free(after);
        return -1;
    }

    orchestration_stage(ctx,task_id,"decomposing","正在分解任务并分配 Agent");
    nsteps = decompose_task(ctx, task, agents, tasks, after);

    if (nsteps < 0) {
        free(agents); free(tasks); free(after);
        *answer=xstrdup("多 Agent 计划无效：请检查 agent、任务长度和 after 前置编号；步骤未执行。");
        return -1;
    }
    if (nsteps == 0) {
        /* fallback: no agents / no parseable plan → plain single-agent run */
        free(agents);
        free(tasks);
        free(after);
        log_info("orchestrator: no multi-agent plan, running single-agent");
        struct task *parent=task_id>=0?scheduler_acquire(ctx->scheduler,task_id):NULL;
        reasoning_config config={0};config.llm=ctx->llm;config.tools=ctx->tools;config.memory=ctx->memory;
        config.policy=ctx->policy;config.bus=ctx->bus;config.metrics=ctx->metrics;config.trace=ctx->trace;
        config.workspace=parent && parent->workspace?parent->workspace:ctx->workspace;
        config.skills=ctx->skills;config.mcp=ctx->mcp;config.state_root=ctx->state_root;
        config.max_rounds=ctx->config?(int)config_get_int(ctx->config,"reasoning.max_rounds",-1):-1;config.usage_acc=ctx->usage;
        reasoning *r=reasoning_new(&config);if(r){reasoning_set_task(r,parent);rc=reasoning_run(r,task,answer);reasoning_free(r);}else rc=-1;
        if(parent)scheduler_release(ctx->scheduler,parent);return rc;
    }

    dag = build_dag_json(agents, tasks, after, nsteps);
    free(agents);
    free(tasks);
    free(after);
    if (!dag)
        return -1;

    orchestration_stage(ctx,task_id,"executing","正在执行多 Agent 节点");
    rc = flow_run(ctx, dag, task_id, &flow_answer, &trace);
    orchestration_stage(ctx,task_id,rc?"failed":"merging",rc?"节点执行未完成":"正在综合各 Agent 的结果");
    free(dag);
    if (rc != 0) {
        *answer=flow_answer ? flow_answer : xstrdup("多 Agent 执行未完成。");
        if(trace_json) *trace_json=trace; else free(trace);
        return -1;
    }

    if (trace) {
        cJSON *arr = cJSON_Parse(trace);
        if (arr) {
            strbuf b;
            strbuf_init(&b);
            strbuf_appendf(&b, "任务: %s\n\n各 agent 结果:\n", task);
            cJSON *it;
            int i = 1;
            cJSON_ArrayForEach(it, arr) {
                cJSON *a = cJSON_GetObjectItemCaseSensitive(it, "agent");
                cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "status");
                cJSON *r = cJSON_GetObjectItemCaseSensitive(it, "result");
                const char *ag = (a && cJSON_IsString(a)) ? a->valuestring : "?";
                const char *st = (s && cJSON_IsString(s)) ? s->valuestring : "?";
                const char *rs = (r && cJSON_IsString(r)) ? r->valuestring : "";
                strbuf_appendf(&b, "%d. [%s/status=%s] %s\n", i++, ag, st, rs);
            }
            cJSON_Delete(arr);
            merged = b.buf;
        }
    }

    /* A one-node plan already has a single authoritative answer. Sending it
     * through another model call only adds latency/cost and can dilute the
     * worker's execution evidence. Multi-node plans still need synthesis. */
    if (nsteps > 1 && ctx->llm && merged) {
        char sys2[] = "你是编排器。综合各 agent 的子任务结果，针对任务给出最终统一答案。"
                      "直接输出答案正文，不要罗列过程。\n"
                      "严格禁止虚构：答案中对文件、命令、测试结果的任何断言，"
                      "必须能在对应 agent 的结果文本中找到原文依据。"
                      "若某步骤 status 不是 ok，或其结果只是意向说明而没有任何实际执行证据"
                      "（没有工具输出、没有验证过文件生成、没有真实测试运行结果），"
                      "必须如实报告该部分未完成，并说明缺失了什么，不得声称成功。";
        final = llm_simple_booked(ctx, sys2, merged);
    }

    if (!final || !*final)
        final = xstrdup(merged && *merged ? merged : flow_answer);

    if (trace_json && trace)
        *trace_json = xstrdup(trace);
    blackboard_put(ctx->blackboard, "flow/final", final);
    free(trace);
    free(merged);
    free(flow_answer);

    *answer = final;
    orchestration_stage(ctx,task_id,final?"completed":"failed",final?"多 Agent 执行与综合已完成":"综合结果未完成");
    return final ? 0 : -1;
}
