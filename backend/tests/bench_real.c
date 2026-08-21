/* bench_real.c — real-LLM evaluation across mainstream agent benchmark families
 * that map onto c-agent's current tool set (file_read/file_write/shell/git/mcp)
 * and single-shot plan→act reasoning loop:
 *
 *   toolbench   ToolBench (ToolEval)-style: tool retrieval + parameter construction
 *   agentbench  AgentBench OS-CLI-style: OS command execution with file side effects
 *
 * Modes:
 *   --real  (default)  drive a live LLM via CA_LLM_PROVIDER / CA_LLM_BASE_URL /
 *                      CA_LLM_MODEL / CA_LLM_API_KEY
 *   --mock  offline deterministic planner (sanity check, no network)
 *
 * NOTE on the other mainstream benchmarks (honest scope boundary):
 *   SWE-bench  needs a read→fix→test *iterative* act-observe loop; c-agent's
 *              reasoning is single-shot (one plan), so real issue-fixing is out
 *              of scope for now.
 *   GAIA       needs multimodal (image/audio) + web browsing + document parsing.
 *   OSWorld    needs GUI (screenshot/mouse/keyboard) + a VM.
 *   WebArena   needs a browser (navigate/click/type).
 *   AgentBench DB/Web/KG subtracks need tools not present in the runtime.
 *
 * Reports per-family accuracy, end-to-end success, side-effect correctness,
 * per-task latency, and a one-line JSON summary.
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

/* planner system prompt — matches reasoning.c/planner.c so the probe is representative */
static const char *SYS_PROMPT =
    "You are the planner of a cognitive OS. Decide what tool actions to take "
    "for the user request. Respond with ONLY a JSON array of actions of the "
    "form [{\"tool\":\"...\",\"args\":{...}}].\n"
    "Available tools and their EXACT argument names:\n"
    "- file_read:  args {\"path\": string}\n"
    "- file_write: args {\"path\": string, \"content\": string}\n"
    "- shell:      args {\"command\": string, \"timeout_ms\": integer}\n"
    "- git:        args {\"args\": string (subcommand+flags, e.g. \"log -5\"), \"dir\": string}\n"
    "- mcp:        args {\"server_url\": string, \"tool\": string, \"args\": object}\n"
    "Use the dedicated git tool (not shell) for git operations, and the mcp tool "
    "(not shell) for MCP calls. Use exactly the argument names listed above. "
    "If no tool is needed, respond with plain text instead.";

typedef struct {
    const char *family;
    const char *name;
    const char *prompt;
    const char *expect_tool;   /* NULL = any tool (side-effect is the check) */
    const char *arg_key;       /* NULL = skip parameter check */
    const char *arg_val;       /* substring expected in that argument */
    const char *side_path;     /* NULL = no side-effect check */
    const char *side_contains; /* NULL = just check existence */
} task_t;

/* ---- ToolBench (ToolEval)-style: tool retrieval + parameter construction ---- */
static const task_t TOOLBENCH[] = {
    {"toolbench", "read_file",  "读取配置文件 config.json 的内容",                   "file_read",  "path",    "config.json",     NULL, NULL},
    {"toolbench", "write_file", "在 data/ 目录下创建 hello.txt 并写入内容 world",   "file_write", "path",    "hello.txt",       NULL, NULL},
    {"toolbench", "shell",      "执行命令 ls -la",                                  "shell",      "command", "ls",              NULL, NULL},
    {"toolbench", "git",        "查看 git 仓库最近 5 条提交记录",                   "git",        "args",    "log",             NULL, NULL},
    {"toolbench", "mcp",        "通过 MCP 调用 http://localhost:9000 上的 echo 工具", "mcp",      "server_url", "localhost:9000", NULL, NULL},
};
#define N_TOOLBENCH ((int)(sizeof(TOOLBENCH) / sizeof(TOOLBENCH[0])))

/* ---- AgentBench OS-CLI-style: OS commands with verifiable file side effects ---- */
static const task_t AGENTBENCH[] = {
    {"agentbench", "mkdir_write", "创建目录 out 并写入文件 out/result.txt 内容 done",  NULL, NULL, NULL, "out/result.txt", "done"},
    {"agentbench", "shell_echo",  "执行命令 echo agentbench-ok > out/echo.txt",        NULL, NULL, NULL, "out/echo.txt",   "agentbench-ok"},
    {"agentbench", "copy",        "把 test/note.txt 复制为 out/note_copy.txt",          NULL, NULL, NULL, "out/note_copy.txt", NULL},
    {"agentbench", "ls_redirect", "执行命令 ls > out/listing.txt",                      NULL, NULL, NULL, "out/listing.txt", NULL},
};
#define N_AGENTBENCH ((int)(sizeof(AGENTBENCH) / sizeof(AGENTBENCH[0])))

#define N_TASKS (N_TOOLBENCH + N_AGENTBENCH)

/* True if the plan selects <tool>. */
static int plan_has_tool(const char *plan, const char *tool) {
    cJSON *root = cJSON_Parse(plan ? plan : "");
    if (!root || !cJSON_IsArray(root)) { if (root) cJSON_Delete(root); return 0; }
    int ok = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, root) {
        if (!cJSON_IsObject(it)) continue;
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "tool");
        if (t && cJSON_IsString(t) && strcmp(t->valuestring, tool) == 0) { ok = 1; break; }
    }
    cJSON_Delete(root);
    return ok;
}

/* True if some <tool> action's args[<key>] (string) contains <val>. */
static int plan_arg_contains(const char *plan, const char *tool,
                             const char *key, const char *val) {
    cJSON *root = cJSON_Parse(plan ? plan : "");
    if (!root || !cJSON_IsArray(root)) { if (root) cJSON_Delete(root); return 0; }
    int ok = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, root) {
        if (!cJSON_IsObject(it)) continue;
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "tool");
        if (!t || !cJSON_IsString(t) || strcmp(t->valuestring, tool) != 0) continue;
        cJSON *a = cJSON_GetObjectItemCaseSensitive(it, "args");
        if (!a || !cJSON_IsObject(a)) continue;
        cJSON *k = cJSON_GetObjectItemCaseSensitive(a, key);
        if (k && cJSON_IsString(k) && strstr(k->valuestring, val) != NULL) { ok = 1; break; }
    }
    cJSON_Delete(root);
    return ok;
}

int main(int argc, char **argv) {
    int real = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--real") == 0) real = 1;
        else if (strcmp(argv[i], "--mock") == 0) real = 0;
        else { fprintf(stderr, "usage: %s [--mock|--real]\n", argv[0]); return 2; }
    }

    cagent_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_root = "state-bench";
    cfg.workspace  = "state-bench/ws";
    cfg.http_port  = 0;
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
        fprintf(stderr, "bench_real: cagent_init failed (provider=%s)\n",
                cfg.provider ? cfg.provider : "?");
        return 1;
    }
    printf("c-agent real-LLM benchmark [%s] provider=%s\n\n",
           real ? "REAL" : "MOCK", ctx.provider ? ctx.provider : "?");

    /* deterministic start state */
    ca_fs_mkdirs("state-bench/ws");
    ca_fs_mkdirs("state-bench/ws/test");
    ca_fs_mkdirs("state-bench/ws/out");
    ca_fs_write_file("state-bench/ws/test/note.txt", "hello\n", 6);
    ca_fs_write_file("state-bench/ws/config.json", "{\"name\":\"bench\"}\n", 17);
    ca_fs_remove("state-bench/ws/out/result.txt");
    ca_fs_remove("state-bench/ws/out/echo.txt");
    ca_fs_remove("state-bench/ws/out/note_copy.txt");
    ca_fs_remove("state-bench/ws/out/listing.txt");

    int fam_sel[N_TASKS], fam_ok[N_TASKS], fam_side[N_TASKS];
    int64_t lat[N_TASKS];
    int64_t t0 = ca_time_now_ms();

    for (int i = 0; i < N_TASKS; i++) {
        const task_t *t = (i < N_TOOLBENCH) ? &TOOLBENCH[i] : &AGENTBENCH[i - N_TOOLBENCH];

        /* 1) planner probe: tool selection + parameter construction */
        int sel = 1;
        if (t->expect_tool) {
            char *plan = ca_llm_chat_simple(ctx.llm, SYS_PROMPT, t->prompt);
            sel = plan ? plan_has_tool(plan, t->expect_tool) : 0;
            if (sel && t->arg_key)
                sel = plan_arg_contains(plan, t->expect_tool, t->arg_key, t->arg_val);
            free(plan);
        }

        /* 2) end-to-end full pipeline (latency + success) */
        char *answer = NULL;
        int64_t s0 = ca_time_now_ms();
        int rc = cagent_run(&ctx, t->prompt, &answer);
        lat[i] = ca_time_now_ms() - s0;
        free(answer);

        /* 3) side effect check */
        int side = 1;
        if (t->side_path) {
            char full[1024];
            ca_path_resolve(full, sizeof(full), "state-bench/ws", t->side_path);
            char *data = ca_fs_read_file(full);
            side = data != NULL && (!t->side_contains || strstr(data, t->side_contains) != NULL);
            free(data);
        }

        fam_sel[i] = sel;
        fam_ok[i] = (rc == 0);
        fam_side[i] = side;

        printf("  [%s/%-12s] sel=%s e2e=%s side=%s lat=%lldms\n",
               t->family, t->name,
               sel ? "OK" : "X", rc == 0 ? "OK" : "X", side ? "OK" : "X",
               (long long)lat[i]);
    }

    int64_t total = ca_time_now_ms() - t0;
    cagent_shutdown(&ctx);

    /* aggregate per family */
    int tb_sel = 0, tb_ok = 0;
    for (int i = 0; i < N_TOOLBENCH; i++) { tb_sel += fam_sel[i]; tb_ok += fam_ok[i]; }
    int ab_ok = 0, ab_side = 0;
    for (int i = 0; i < N_AGENTBENCH; i++) { ab_ok += fam_ok[i + N_TOOLBENCH]; ab_side += fam_side[i + N_TOOLBENCH]; }

    int64_t sum = 0, mx = 0;
    for (int i = 0; i < N_TASKS; i++) { sum += lat[i]; if (lat[i] > mx) mx = lat[i]; }
    double avg = (double)sum / N_TASKS;

    printf("\n== summary ==\n");
    printf("  ToolBench  tool+param accuracy : %d/%d\n", tb_sel, N_TOOLBENCH);
    printf("  AgentBench OS-CLI  side-effect : %d/%d   e2e=%d/%d\n",
           ab_side, N_AGENTBENCH, ab_ok, N_AGENTBENCH);
    printf("  end-to-end success (all)       : %d/%d\n", tb_ok + ab_ok, N_TASKS);
    printf("  latency avg/max                : %.1f/%lld ms   total=%lldms\n",
           avg, (long long)mx, (long long)total);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "toolbench_tool_param_ok", tb_sel);
    cJSON_AddNumberToObject(j, "toolbench_total", N_TOOLBENCH);
    cJSON_AddNumberToObject(j, "agentbench_side_ok", ab_side);
    cJSON_AddNumberToObject(j, "agentbench_total", N_AGENTBENCH);
    cJSON_AddNumberToObject(j, "e2e_success", tb_ok + ab_ok);
    cJSON_AddNumberToObject(j, "e2e_total", N_TASKS);
    cJSON_AddNumberToObject(j, "avg_latency_ms", avg);
    cJSON_AddNumberToObject(j, "max_latency_ms", (double)mx);
    cJSON_AddNumberToObject(j, "total_ms", (double)total);
    char *js = cJSON_PrintUnformatted(j);
    printf("  JSON: %s\n", js ? js : "{}");
    free(js);
    cJSON_Delete(j);

    return 0;
}
