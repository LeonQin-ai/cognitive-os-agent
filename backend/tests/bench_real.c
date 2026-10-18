/* bench_real.c — real-LLM evaluation across mainstream agent benchmark families
 * that map onto cognitive-os-agent's current tool set (file_read/file_write/shell/git/mcp)
 * and single-shot plan→act reasoning loop:
 *
 *   toolbench   ToolBench (ToolEval)-style: tool retrieval + parameter construction
 *   agentbench  AgentBench OS-CLI-style: OS command execution with file side effects
 *
 * Modes:
 *   --real  (default)  drive a live LLM via LLM_PROVIDER / LLM_BASE_URL /
 *                      LLM_MODEL / LLM_API_KEY
 *   --mock  offline deterministic planner (sanity check, no network)
 *
 * NOTE on the other mainstream benchmarks (honest scope boundary):
 *   SWE-bench  needs a read→fix→test *iterative* act-observe loop; cognitive-os-agent's
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
#include "cognitive-os-agent.h"
#include "llm/llm.h"
#include "os/os_fs.h"
#include "os/os_time.h"
#include "infra/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static const char *g_executable_path;

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
    "- mcp:        args {\"server\": string, \"tool\": string, \"args\": object}\n"
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
    const char *required_evidence; /* expected text in executed tool observations */
} task_t;

/* ---- ToolBench (ToolEval)-style: tool retrieval + parameter construction ---- */
static const task_t TOOLBENCH[] = {
    {"toolbench", "read_file",  "读取配置文件 config.json 的内容",                   "file_read",  "path",    "config.json",     NULL, NULL, "{\"name\":\"bench\"}"},
    {"toolbench", "write_file", "在 data/ 目录下创建 hello.txt 并写入内容 world",   "file_write", "path",    "hello.txt",       "data/hello.txt", "world", NULL},
    {"toolbench", "shell",      "执行命令 ls -la",                                  "shell",      "command", "ls",              NULL, NULL, "config.json"},
    {"toolbench", "git",        "查看 git 仓库最近 5 条提交记录",                   "git",        "args",    "log",             NULL, NULL, "bench-baseline"},
    {"toolbench", "mcp",        "通过已注册的 MCP 服务器 bench-echo 调用 echo 工具，参数 text=bench-echo-proof", "mcp", "server", "bench-echo", NULL, NULL, "echo: bench-echo-proof"},
};
#define N_TOOLBENCH ((int)(sizeof(TOOLBENCH) / sizeof(TOOLBENCH[0])))

/* ---- AgentBench OS-CLI-style: OS commands with verifiable file side effects ---- */
static const task_t AGENTBENCH[] = {
    {"agentbench", "mkdir_write", "创建目录 out 并写入文件 out/result.txt 内容 done",  NULL, NULL, NULL, "out/result.txt", "done", NULL},
    {"agentbench", "shell_echo",  "执行命令 echo agentbench-ok > out/echo.txt",        NULL, NULL, NULL, "out/echo.txt",   "agentbench-ok", NULL},
    {"agentbench", "copy",        "把 test/note.txt 复制为 out/note_copy.txt",          NULL, NULL, NULL, "out/note_copy.txt", NULL, NULL},
    {"agentbench", "ls_redirect", "执行命令 ls > out/listing.txt",                      NULL, NULL, NULL, "out/listing.txt", NULL, NULL},
};
#define N_AGENTBENCH ((int)(sizeof(AGENTBENCH) / sizeof(AGENTBENCH[0])))

#define N_TASKS (N_TOOLBENCH + N_AGENTBENCH)

static int locate_mock_mcp_script(char *out, size_t cap) {
    static const char *const cwd_candidates[] = {
        "tools/mock_mcp_server.js", "../tools/mock_mcp_server.js", "../../tools/mock_mcp_server.js",
    };
    char candidate[2048];
    for (size_t i = 0; i < sizeof(cwd_candidates) / sizeof(cwd_candidates[0]); i++) {
        if (fs_exists(cwd_candidates[i])) {
            char *absolute = fs_absolute_path(cwd_candidates[i]);
            if (!absolute) return 0;
            snprintf(out, cap, "%s", absolute);
            free(absolute);
            return 1;
        }
    }
    if (!g_executable_path || !*g_executable_path) return 0;
    char *absolute_exe = fs_absolute_path(g_executable_path);
    if (!absolute_exe) return 0;
    char *slash = strrchr(absolute_exe, '/');
    char *backslash = strrchr(absolute_exe, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) { free(absolute_exe); return 0; }
    *slash = '\0';
    static const char *const exe_candidates[] = {
        "../tools/mock_mcp_server.js", "tools/mock_mcp_server.js", "../../tools/mock_mcp_server.js",
    };
    for (size_t i = 0; i < sizeof(exe_candidates) / sizeof(exe_candidates[0]); i++) {
        path_join(candidate, sizeof(candidate), absolute_exe, exe_candidates[i]);
        if (fs_exists(candidate)) {
            snprintf(out, cap, "%s", candidate);
            free(absolute_exe);
            return 1;
        }
    }
    free(absolute_exe);
    return 0;
}

static int register_mcp_fixture(runtime_ctx *ctx) {
    char script[2048];
    if (!ctx || !ctx->mcp || !locate_mock_mcp_script(script, sizeof(script))) return 0;
    cJSON *args = cJSON_CreateArray();
    if (!args) return 0;
    cJSON_AddItemToArray(args, cJSON_CreateString(script));
    cJSON_AddItemToArray(args, cJSON_CreateString("--stdio"));
    char *args_json = cJSON_PrintUnformatted(args);
    cJSON_Delete(args);
    if (!args_json) return 0;
    mcp_conn conn = {.name = "bench-echo", .transport = "stdio", .command = "node", .args_csv = args_json};
    int added = mcp_manager_add_ex(ctx->mcp, &conn) == 0;
    free(args_json);
    if (!added) return 0;
    return mcp_manager_sync_tools_one(ctx->mcp, ctx->tools, "bench-echo") > 0;
}

static int prepare_git_fixture(void) {
    if (system("git -C \"state-bench/ws\" init -q") != 0) return 0;
    if (system("git -C \"state-bench/ws\" config user.name bench && "
               "git -C \"state-bench/ws\" config user.email bench@example.invalid") != 0) return 0;
    char seed[96], command[512];
    snprintf(seed, sizeof(seed), "baseline-%lld\n", (long long)time_now_ms());
    if (fs_write_file("state-bench/ws/.benchmark-seed", seed, strlen(seed)) != 0) return 0;
    if (system("git -C \"state-bench/ws\" add config.json .benchmark-seed") != 0) return 0;
    snprintf(command, sizeof(command), "git -C \"state-bench/ws\" commit -q -m bench-baseline");
    return system(command) == 0;
}

static int execution_observed(reasoning *reasoner, int run_rc, const char *required_evidence) {
    long long elapsed = 0, tokens_in = 0, tokens_out = 0;
    int round = 0, calls = 0;
    const char *last_tool = NULL;
    reasoning_progress(reasoner, &elapsed, &round, &calls, &last_tool, &tokens_in, &tokens_out);
    char *log = reasoning_round_log_tail(reasoner, 65536);
    int ok = run_rc == 0 && calls > 0 && log && !strstr(log, "] FAILED");
    if (ok && required_evidence && *required_evidence)
        ok = strstr(log, required_evidence) != NULL;
    free(log);
    return ok;
}

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

static int plan_matches_mcp_fixture(const char *plan) {
    cJSON *root = cJSON_Parse(plan ? plan : "");
    if (!root || !cJSON_IsArray(root)) { if (root) cJSON_Delete(root); return 0; }
    int ok = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, root) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(it, "tool");
        cJSON *args = cJSON_GetObjectItemCaseSensitive(it, "args");
        cJSON *server = args ? cJSON_GetObjectItemCaseSensitive(args, "server") : NULL;
        cJSON *remote = args ? cJSON_GetObjectItemCaseSensitive(args, "tool") : NULL;
        cJSON *params = args ? cJSON_GetObjectItemCaseSensitive(args, "args") : NULL;
        cJSON *text = params ? cJSON_GetObjectItemCaseSensitive(params, "text") : NULL;
        if (cJSON_IsString(name) && strcmp(name->valuestring, "mcp") == 0 &&
            cJSON_IsString(server) && strcmp(server->valuestring, "bench-echo") == 0 &&
            cJSON_IsString(remote) && strcmp(remote->valuestring, "echo") == 0 &&
            cJSON_IsString(text) && strcmp(text->valuestring, "bench-echo-proof") == 0) {
            ok = 1;
            break;
        }
    }
    cJSON_Delete(root);
    return ok;
}

int main(int argc, char **argv) {
    int real = 1;
    g_executable_path = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--real") == 0) real = 1;
        else if (strcmp(argv[i], "--mock") == 0) real = 0;
        else { fprintf(stderr, "usage: %s [--mock|--real]\n", argv[0]); return 2; }
    }

    config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_root = "state-bench";
    cfg.workspace  = "state-bench/ws";
    cfg.http_port  = 0;
    if (real) {
        cfg.provider = getenv("LLM_PROVIDER");
        cfg.base_url = getenv("LLM_BASE_URL");
        cfg.api_key  = getenv("LLM_API_KEY");
        cfg.model    = getenv("LLM_MODEL");
        if (!cfg.provider || !*cfg.provider) cfg.provider = "openai";
    } else {
        cfg.provider = "mock";
    }

    runtime_ctx ctx;
    if (init(&ctx, &cfg) != 0) {
        fprintf(stderr, "bench_real: init failed (provider=%s)\n",
                cfg.provider ? cfg.provider : "?");
        return 1;
    }
    printf("cognitive-os-agent real-LLM benchmark [%s] provider=%s\n\n",
           real ? "REAL" : "MOCK", ctx.provider ? ctx.provider : "?");

    int mcp_available = register_mcp_fixture(&ctx);
    printf("MCP fixture: %s%s\n\n", mcp_available ? "ready" : "SKIP",
           mcp_available ? " (bench-echo stdio server)" : " (Node.js or fixture unavailable)");

    /* deterministic start state */
    fs_mkdirs("state-bench/ws");
    fs_mkdirs("state-bench/ws/test");
    fs_mkdirs("state-bench/ws/out");
    fs_mkdirs("state-bench/ws/data");
    fs_write_file("state-bench/ws/test/note.txt", "hello\n", 6);
    fs_write_file("state-bench/ws/config.json", "{\"name\":\"bench\"}\n", 17);
    fs_remove("state-bench/ws/data/hello.txt");
    fs_remove("state-bench/ws/out/result.txt");
    fs_remove("state-bench/ws/out/echo.txt");
    fs_remove("state-bench/ws/out/note_copy.txt");
    fs_remove("state-bench/ws/out/listing.txt");
    int git_available = prepare_git_fixture();
    if (!git_available) printf("Git fixture: SKIP (git CLI unavailable)\n");

    int fam_valid[N_TASKS], fam_sel[N_TASKS], fam_ok[N_TASKS], fam_side[N_TASKS];
    int64_t lat[N_TASKS];
    int64_t t0 = time_now_ms();

    for (int i = 0; i < N_TASKS; i++) {
        const task_t *t = (i < N_TOOLBENCH) ? &TOOLBENCH[i] : &AGENTBENCH[i - N_TOOLBENCH];
        fam_valid[i] = 1;
        if (strcmp(t->name, "git") == 0 && !git_available) {
            fam_valid[i] = fam_sel[i] = fam_ok[i] = 0;
            fam_side[i] = 1;
            lat[i] = 0;
            printf("  [%s/%-12s] SKIP (git fixture unavailable)\n", t->family, t->name);
            continue;
        }
        if (strcmp(t->name, "mcp") == 0 && !mcp_available) {
            fam_valid[i] = fam_sel[i] = fam_ok[i] = 0;
            fam_side[i] = 1;
            lat[i] = 0;
            printf("  [%s/%-12s] SKIP (MCP fixture unavailable)\n", t->family, t->name);
            continue;
        }

        /* 1) planner probe: tool selection + parameter construction */
        int sel = 1;
        if (t->expect_tool) {
            char *plan = llm_chat_simple(ctx.llm, SYS_PROMPT, t->prompt);
            sel = plan ? plan_has_tool(plan, t->expect_tool) : 0;
            if (sel && strcmp(t->name, "mcp") == 0)
                sel = plan_matches_mcp_fixture(plan);
            else if (sel && t->arg_key)
                sel = plan_arg_contains(plan, t->expect_tool, t->arg_key, t->arg_val);
            free(plan);
        }

        /* 2) end-to-end full pipeline (latency + success) */
        char *answer = NULL;
        int64_t s0 = time_now_ms();
        int rc = run(&ctx, t->prompt, &answer);
        lat[i] = time_now_ms() - s0;
        int observed = execution_observed(ctx.reasoning, rc, t->required_evidence);
        free(answer);

        /* 3) side effect check */
        int side = 1;
        if (t->side_path) {
            char full[1024];
            path_resolve(full, sizeof(full), "state-bench/ws", t->side_path);
            char *data = fs_read_file(full);
            side = data != NULL && (!t->side_contains || strstr(data, t->side_contains) != NULL);
            free(data);
        }

        fam_sel[i] = sel;
        fam_ok[i] = observed && side;
        fam_side[i] = side;

        printf("  [%s/%-12s] sel=%s e2e=%s side=%s lat=%lldms\n",
               t->family, t->name,
               sel ? "OK" : "X", fam_ok[i] ? "OK" : "X",
               t->side_path ? (side ? "OK" : "X") : "N/A",
               (long long)lat[i]);
    }

    int64_t total = time_now_ms() - t0;
    runtime_shutdown(&ctx);

    /* aggregate per family */
    int tb_sel = 0, tb_ok = 0, tb_total = 0;
    for (int i = 0; i < N_TOOLBENCH; i++) {
        if (!fam_valid[i]) continue;
        tb_total++; tb_sel += fam_sel[i]; tb_ok += fam_ok[i];
    }
    int ab_ok = 0, ab_side = 0;
    for (int i = 0; i < N_AGENTBENCH; i++) { ab_ok += fam_ok[i + N_TOOLBENCH]; ab_side += fam_side[i + N_TOOLBENCH]; }

    int e2e_total = tb_total + N_AGENTBENCH;
    int64_t sum = 0, mx = 0;
    for (int i = 0; i < N_TASKS; i++) {
        if (!fam_valid[i]) continue;
        sum += lat[i]; if (lat[i] > mx) mx = lat[i];
    }
    double avg = e2e_total ? (double)sum / e2e_total : 0.0;

    printf("\n== summary ==\n");
    printf("  ToolBench  tool+param accuracy : %d/%d\n", tb_sel, tb_total);
    printf("  AgentBench OS-CLI  side-effect : %d/%d   e2e=%d/%d\n",
           ab_side, N_AGENTBENCH, ab_ok, N_AGENTBENCH);
    printf("  end-to-end success (all)       : %d/%d\n", tb_ok + ab_ok, e2e_total);
    printf("  latency avg/max                : %.1f/%lld ms   total=%lldms\n",
           avg, (long long)mx, (long long)total);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "toolbench_tool_param_ok", tb_sel);
    cJSON_AddNumberToObject(j, "toolbench_total", tb_total);
    cJSON_AddNumberToObject(j, "agentbench_side_ok", ab_side);
    cJSON_AddNumberToObject(j, "agentbench_total", N_AGENTBENCH);
    cJSON_AddNumberToObject(j, "e2e_success", tb_ok + ab_ok);
    cJSON_AddNumberToObject(j, "e2e_total", e2e_total);
    cJSON_AddNumberToObject(j, "avg_latency_ms", avg);
    cJSON_AddNumberToObject(j, "max_latency_ms", (double)mx);
    cJSON_AddNumberToObject(j, "total_ms", (double)total);
    char *js = cJSON_PrintUnformatted(j);
    printf("  JSON: %s\n", js ? js : "{}");
    free(js);
    cJSON_Delete(j);

    return 0;
}
