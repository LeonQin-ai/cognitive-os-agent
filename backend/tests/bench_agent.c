/* bench_agent.c — Agent + LLM capability benchmark.
 * Measures tool-selection accuracy, end-to-end success rate, side-effect
 * correctness, multi-step completion and latency. Two modes:
 *   --mock  (default)  offline deterministic mock planner (tool selection only
 *                      is meaningful; e2e/side-effects are exercised locally)
 *   --real  use a real LLM endpoint via CA_LLM_PROVIDER / CA_LLM_BASE_URL /
 *           CA_LLM_MODEL / CA_LLM_API_KEY (accuracy + success + latency + multistep)
 * Prints per-task lines, a summary table, and a one-line JSON summary.
 */
#include "cagent/cagent.h"
#include "cagent/llm/llm.h"
#include "cagent/os/os_fs.h"
#include "cagent/os/os_time.h"
#include "cagent/infra/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

/* planner system prompt — matches reasoning.c so the probe is representative */
static const char *SYS_PROMPT =
    "You are the planner of a cognitive OS. Decide what tool actions to take "
    "for the user request. Respond with ONLY a JSON array of actions of the "
    "form [{\"tool\":\"file_write\",\"args\":{\"path\":\"...\",\"content\":\"...\"}}]. "
    "Available tools: file_read, file_write, shell, git, mcp. "
    "If no tool is needed, respond with plain text instead.";

typedef struct {
    const char *name;
    const char *prompt;
    int expect_n;               /* expected # of tool actions (0 = plain text) */
    const char *expect[3];      /* tools that must appear, NULL-terminated */
    const char *path;           /* workspace-relative file side effect, NULL=none */
    const char *contains;       /* substring expected in that file */
} bench_task;

static const bench_task TASKS[] = {
    {"file_write", "创建 test/note.txt 写入内容为 hello",      1, {"file_write", NULL},             "test/note.txt", "hello"},
    {"file_read",  "读取 test/note.txt",                       1, {"file_read",  NULL},             NULL,            NULL},
    {"shell",      "执行命令 echo hi",                         1, {"shell",      NULL},             NULL,            NULL},
    {"multi_step", "创建 a.txt 写入 hello，然后读取 a.txt",    2, {"file_write", "file_read", NULL}, "a.txt",         "hello"},
    {"chat",       "你好",                                     0, {NULL},                           NULL,            NULL},
};
#define N_TASKS ((int)(sizeof(TASKS) / sizeof(TASKS[0])))

/* Parse a planner response into a list of tool names. Returns count. */
static int plan_tools(const char *plan, char tools[8][64]) {
    int n = 0;
    cJSON *root = cJSON_Parse(plan ? plan : "");
    if (!root || !cJSON_IsArray(root)) { if (root) cJSON_Delete(root); return 0; }
    cJSON *it;
    cJSON_ArrayForEach(it, root) {
        if (n >= 8 || !cJSON_IsObject(it)) continue;
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "tool");
        if (t && cJSON_IsString(t)) {
            snprintf(tools[n], 64, "%s", t->valuestring);
            n++;
        }
    }
    cJSON_Delete(root);
    return n;
}

/* True if the selected tools match the task's ground truth. */
static int selection_ok(const bench_task *t, char tools[8][64], int n) {
    if (t->expect_n == 0) return n == 0;   /* chat: no tool actions */
    if (n != t->expect_n) return 0;
    int matched = 0;
    for (int e = 0; t->expect[e] != NULL; e++) {
        for (int i = 0; i < n; i++)
            if (strcmp(tools[i], t->expect[e]) == 0) { matched++; break; }
    }
    return matched == t->expect_n;
}

int main(int argc, char **argv) {
    int real = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--real") == 0) real = 1;
        else if (strcmp(argv[i], "--mock") == 0) real = 0;
        else { fprintf(stderr, "usage: %s [--mock|--real]\n", argv[0]); return 2; }
    }

    cagent_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_root = "state-bench";
    cfg.workspace  = "state-bench/ws";
    cfg.http_port  = 0;                 /* no HTTP API for the benchmark */
    if (real) {
        cfg.provider = getenv("CA_LLM_PROVIDER");
        cfg.base_url = getenv("CA_LLM_BASE_URL");
        cfg.api_key  = getenv("CA_LLM_API_KEY");
        cfg.model    = getenv("CA_LLM_MODEL");
        if (!cfg.provider || !*cfg.provider) cfg.provider = "openai";
    } else {
        cfg.provider = "mock";
    }

    cagent_ctx ctx;
    if (cagent_init(&ctx, &cfg) != 0) {
        fprintf(stderr, "bench: cagent_init failed (provider=%s)\n",
                cfg.provider ? cfg.provider : "?");
        return 1;
    }
    printf("c-agent agent capability benchmark [%s] provider=%s\n\n",
           real ? "REAL" : "MOCK", ctx.provider ? ctx.provider : "?");

    /* clean prior side effects for a deterministic run */
    ca_fs_mkdirs("state-bench/ws");
    ca_fs_remove("state-bench/ws/test/note.txt");
    ca_fs_remove("state-bench/ws/a.txt");

    int sel_correct = 0, e2e_ok = 0, side_ok = 0, side_n = 0, multi_ok = 0;
    int64_t lat[N_TASKS];
    int64_t t0 = ca_time_now_ms();

    for (int i = 0; i < N_TASKS; i++) {
        const bench_task *t = &TASKS[i];

        /* 1) tool selection — direct planner probe */
        char tools[8][64];
        int nt = 0;
        char *plan = ca_llm_chat_simple(ctx.llm, SYS_PROMPT, t->prompt);
        if (plan) { nt = plan_tools(plan, tools); free(plan); }
        int sel = selection_ok(t, tools, nt);
        if (sel) sel_correct++;

        /* 2) end-to-end — full reasoning pipeline (latency + success) */
        char *answer = NULL;
        int64_t s0 = ca_time_now_ms();
        int rc = cagent_run(&ctx, t->prompt, &answer);
        lat[i] = ca_time_now_ms() - s0;
        if (rc == 0) e2e_ok++;
        free(answer);

        /* 3) side effect — file exists and contains expected substring */
        int side = 1;                    /* tasks without a file side effect pass */
        if (t->path) {
            side_n++;
            char full[1024];
            ca_path_resolve(full, sizeof(full), "state-bench/ws", t->path);
            char *data = ca_fs_read_file(full);
            side = (data != NULL) && (!t->contains || strstr(data, t->contains) != NULL);
            free(data);
            if (side) side_ok++;
        }

        if (t->expect_n == 2 && sel && rc == 0) multi_ok++;

        printf("  [%d] %-10s expect=%-8s got=%d tool(s) sel=%s e2e=%s side=%s lat=%lldms\n",
               i, t->name, t->expect[0] ? t->expect[0] : "(text)", nt,
               sel ? "OK" : "X", rc == 0 ? "OK" : "X", side ? "OK" : "X",
               (long long)lat[i]);
    }

    int64_t total = ca_time_now_ms() - t0;
    cagent_shutdown(&ctx);

    /* latency percentiles (nearest-rank over the small sample) */
    int64_t sorted[N_TASKS];
    memcpy(sorted, lat, sizeof(sorted));
    for (int i = 0; i < N_TASKS; i++)
        for (int j = i + 1; j < N_TASKS; j++)
            if (sorted[j] < sorted[i]) { int64_t x = sorted[i]; sorted[i] = sorted[j]; sorted[j] = x; }
    int64_t sum = 0;
    for (int i = 0; i < N_TASKS; i++) sum += sorted[i];
    int i50 = (N_TASKS - 1) * 50 / 100;
    int i90 = (N_TASKS - 1) * 90 / 100;
    int64_t p50 = sorted[i50], p90 = sorted[i90], mx = sorted[N_TASKS - 1];

    double acc   = 100.0 * sel_correct / N_TASKS;
    double succ  = 100.0 * e2e_ok / N_TASKS;
    double sacc  = side_n ? 100.0 * side_ok / side_n : 100.0;
    double avg   = (double)sum / N_TASKS;

    printf("\n== summary ==\n");
    printf("  tool-selection accuracy : %d/%d (%.1f%%)\n", sel_correct, N_TASKS, acc);
    printf("  end-to-end success rate : %d/%d (%.1f%%)\n", e2e_ok, N_TASKS, succ);
    printf("  side-effect correctness : %d/%d (%.1f%%)\n", side_ok, side_n, sacc);
    printf("  multi-step completion   : %d/%d\n", multi_ok, 1);
    printf("  latency (ms) avg/p50/p90/max : %.1f/%lld/%lld/%lld   total=%lldms\n",
           avg, (long long)p50, (long long)p90, (long long)mx, (long long)total);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "tool_selection_pct", acc);
    cJSON_AddNumberToObject(j, "success_pct", succ);
    cJSON_AddNumberToObject(j, "side_effect_pct", sacc);
    cJSON_AddNumberToObject(j, "multi_step", multi_ok);
    cJSON_AddNumberToObject(j, "avg_latency_ms", avg);
    cJSON_AddNumberToObject(j, "p50_latency_ms", (double)p50);
    cJSON_AddNumberToObject(j, "p90_latency_ms", (double)p90);
    cJSON_AddNumberToObject(j, "max_latency_ms", (double)mx);
    cJSON_AddNumberToObject(j, "total_ms", (double)total);
    char *js = cJSON_PrintUnformatted(j);
    printf("  JSON: %s\n", js ? js : "{}");
    free(js);
    cJSON_Delete(j);

    return 0;
}
