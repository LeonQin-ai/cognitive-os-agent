/* cli/main.c — cognitive-os-agent command-line interface.
 *   cognitive-os-agent run "<prompt>"       run one prompt through the cognitive pipeline
 *   cognitive-os-agent serve [port]         serve the HTTP API + web console
 *   cognitive-os-agent tools                list available tools
 *   cognitive-os-agent memory               show working + long-term memory
 *   cognitive-os-agent snapshot list|rollback
 *   cognitive-os-agent config               show effective config
 *   cognitive-os-agent                      interactive shell
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif
#include "cognitive-os-agent.h"
#include "action/tools.h"
#include "memory/memory.h"
#include "snapshot/snapshot.h"
#include "infra/config.h"
#include "cJSON.h"

#define STATE_ROOT "state"

static void print_usage(void) {
    printf("cognitive-os-agent %s — Cognitive OS Runtime\n", version());
    printf("usage:\n");
    printf("  cognitive-os-agent [--config FILE] [--workspace DIR] [--state DIR]\n");
    printf("                                            interactive chat; type a task directly\n");
    printf("  cognitive-os-agent [options] run <prompt>  run one task\n");
    printf("  cognitive-os-agent [options] <prompt>      run one task directly\n");
    printf("  cognitive-os-agent [options] serve [port]  serve HTTP API + web console\n");
#ifdef _WIN32
    printf("  cognitive-os-agent install [dir]           copy self to install dir (default %%ProgramFiles%%\\cognitive-os-agent)\n");
#endif
    printf("  cognitive-os-agent tools                   list available tools\n");
    printf("  cognitive-os-agent memory                  show working + long-term memory\n");
    printf("  cognitive-os-agent snapshot list|rollback  list snapshots / rollback to latest\n");
    printf("  cognitive-os-agent config                  show effective config\n");
    printf("Configuration: default state/cognitive-os-agent.json; environment COA_* overrides file values.\n");
    printf("Interactive commands: /help /config /tools /memory /snapshot list /exit\n");
}

/* Self-install: copy the running exe to <dir> (default %ProgramFiles%\
 * cognitive-os-agent). Used by the setup wrapper; avoids the fragile
 * "launch a .bat from a temp dir" step that breaks on some systems.
 * Windows-only (self-locate + CopyFile); the dispatch site is guarded too. */
#ifdef _WIN32
static int cmd_install(const char *dir_arg) {
    char src[MAX_PATH], dst_dir[MAX_PATH], dst[MAX_PATH];
    const char *pf;
    HMODULE me;
    DWORD n;
    char *p;
    BOOL ok;

    me = GetModuleHandleA(NULL);
    n = GetModuleFileNameA(me, src, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        printf("error: cannot locate own executable path\n");
        return 1;
    }
    pf = getenv("ProgramFiles");
    if (!pf || !*pf)
        pf = "C:\\Program Files";
    if (dir_arg && *dir_arg)
        snprintf(dst_dir, sizeof dst_dir, "%s", dir_arg);
    else
        snprintf(dst_dir, sizeof dst_dir, "%s\\cognitive-os-agent", pf);
    snprintf(dst, sizeof dst, "%s\\cognitive-os-agent.exe", dst_dir);

    /* create the target dir plus any missing parents */
    for (p = dst_dir; *p; p++) {
        if (*p == '\\' && p != dst_dir) {
            *p = '\0';
            CreateDirectoryA(dst_dir, NULL);
            *p = '\\';
        }
    }
    if (!CreateDirectoryA(dst_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        printf("error: cannot create %s (run as administrator)\n", dst_dir);
        return 1;
    }
    ok = CopyFileA(src, dst, FALSE);
    if (!ok) {
        printf("error: cannot copy to %s (error %lu; run as administrator)\n",
               dst, (unsigned long)GetLastError());
        return 1;
    }
    printf("cognitive-os-agent %s installed:\n", version());
    printf("  %s\n", dst);
    printf("Start the server:\n");
    printf("  \"%s\" serve 8080\n", dst);
    printf("then open http://localhost:8080\n");
    return 0;
}
#endif /* _WIN32 */

static int cmd_tools(void) {
    tool_registry *reg = tool_registry_new();
    tool_register_builtins(reg);
    int n = tool_registry_count(reg);
    printf("%d tools:\n", n);
    for (int i = 0; i < n; i++) {
        const tool *t = tool_registry_get(reg, (size_t)i);
        printf("  %-12s %s%s\n", t->name, t->description ? t->description : "",
               t->is_write ? "  [write]" : "");
    }
    tool_registry_free(reg);
    return 0;
}

static int cmd_snapshot(const char *state_root, const char *sub) {
    snapshot *s = snapshot_open(state_root);
    if (!s) {
        printf("error: cannot open snapshot store at %s\n", state_root);
        return 1;
    }
    int rc = 0;
    if (strcmp(sub, "list") == 0) {
        char *j = snapshot_list(s);
        printf("%s\n", j ? j : "[]");
        free(j);
    } else if (strcmp(sub, "rollback") == 0 || strcmp(sub, "restore") == 0) {
        rc = snapshot_restore_latest(s);
        printf(rc == 0 ? "rolled back to latest snapshot\n" : "no snapshot to restore\n");
    } else {
        printf("usage: snapshot list|rollback\n");
    }
    snapshot_close(s);
    return rc;
}

static int run_prompt(runtime_ctx *ctx, const char *prompt) {
    char *answer = NULL;
    int rc = run(ctx, prompt, &answer);
    printf("\n");
    if (answer) {
        printf("%s\n", answer);
        free(answer);
    }
    if (rc != 0) fprintf(stderr, "task failed\n");
    return rc;
}

typedef struct cli_progress_state { char last[256]; } cli_progress_state;
static void cli_progress(const char *json, void *ud) {
    cli_progress_state *state = (cli_progress_state *)ud;
    cJSON *root = cJSON_Parse(json);
    cJSON *activity = root ? cJSON_GetObjectItemCaseSensitive(root, "activity") : NULL;
    cJSON *stage = root ? cJSON_GetObjectItemCaseSensitive(root, "stage") : NULL;
    const char *label = cJSON_IsString(activity) ? activity->valuestring : NULL;
    if (cJSON_IsString(stage)) {
        if (strcmp(stage->valuestring, "completed") == 0) label = NULL;
        else if (strcmp(stage->valuestring, "analyzing") == 0) label = "正在分析任务";
        else if (strcmp(stage->valuestring, "replanning") == 0) label = "正在根据执行结果调整计划";
    }
    if (state && label && strcmp(state->last, label) != 0) {
        snprintf(state->last, sizeof(state->last), "%s", label);
        fprintf(stderr, "· %s\n", state->last);
        fflush(stderr);
    }
    cJSON_Delete(root);
}

static void print_config(const runtime_ctx *ctx) {
    const char *provider = config_get_str(ctx->config, "llm.provider", "mock");
    const char *model = config_get_str(ctx->config, "llm.model", "");
    const char *base = config_get_str(ctx->config, "llm.base_url", "");
    const char *key = config_get_str(ctx->config, "llm.api_key", "");
    printf("state: %s\nworkspace: %s\nprovider: %s\nmodel: %s\nendpoint: %s\napi_key: %s\n",
           ctx->state_root, ctx->workspace, provider, model,
           base && *base ? "configured" : "default", key && *key ? "configured" : "missing");
}

static char *read_line(void) {
    size_t cap = 1024, len = 0;
    char *line = malloc(cap);
    int ch;
    if (!line) return NULL;
    while ((ch = getchar()) != EOF && ch != '\n') {
        if (len + 1 >= cap) {
            size_t next = cap * 2;
            char *grown = realloc(line, next);
            if (!grown) { free(line); return NULL; }
            line = grown; cap = next;
        }
        line[len++] = (char)ch;
    }
    if (ch == EOF && len == 0) { free(line); return NULL; }
    if (len && line[len - 1] == '\r') len--;
    line[len] = '\0';
    return line;
}

static char *join_args(int argc, char **argv, int from) {
    size_t n = 1;
    for (int i = from; i < argc; i++) n += strlen(argv[i]) + 1;
    char *out = malloc(n);
    if (!out) return NULL;
    out[0] = '\0';
    for (int i = from; i < argc; i++) {
        if (i > from) strcat(out, " ");
        strcat(out, argv[i]);
    }
    return out;
}

static const char *config_parent(const char *file, char *out, size_t cap) {
    const char *slash = strrchr(file, '/');
    const char *backslash = strrchr(file, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) return ".";
    size_t len = (size_t)(slash - file);
    if (len == 0) len = 1;
    if (len == 2 && file[1] == ':') len = 3; /* C:\\file.json -> C:\\ */
    if (len >= cap) return NULL;
    memcpy(out, file, len);
    out[len] = '\0';
    return out;
}

#ifdef _WIN32
/* Windows CRT main() receives argv in the active ANSI code page. Convert the
 * Unicode command line before parsing so Chinese tasks and paths survive. */
static char **utf8_argv(int *argc) {
    int count = 0;
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) return NULL;
    char **args = calloc((size_t)count + 1, sizeof(*args));
    if (!args) { LocalFree(wide); return NULL; }
    for (int i = 0; i < count; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        if (n <= 0 || !(args[i] = malloc((size_t)n)) ||
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, args[i], n, NULL, NULL) <= 0) {
            for (int j = 0; j <= i; j++) free(args[j]);
            free(args); LocalFree(wide); return NULL;
        }
    }
    LocalFree(wide);
    *argc = count;
    return args;
}
#endif

static void print_memory(runtime_ctx *ctx) {
    char *w = memory_working_json(ctx->memory);
    char *l = memory_longterm_json(ctx->memory);
    printf("working:\n  %s\nlongterm:\n  %s\n", w ? w : "[]", l ? l : "{}");
    free(w);
    free(l);
}

int main(int argc, char **argv) {
    const char *config_file = NULL, *state_root = STATE_ROOT, *workspace = NULL;
    char state_dir[1024];
    int arg = 1, rc = 0, state_overridden = 0;
    runtime_ctx ctx;
    config cfg;
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    argv = utf8_argv(&argc);
    if (!argv) { fprintf(stderr, "cannot decode Windows command line\n"); return 2; }
#endif
    while (arg < argc) {
        const char *opt = argv[arg];
        if (strcmp(opt, "--config") == 0 || strcmp(opt, "--state") == 0 ||
            strcmp(opt, "--workspace") == 0) {
            if (arg + 1 >= argc || !argv[arg + 1][0]) {
                fprintf(stderr, "missing value for %s\n", opt);
                return 2;
            }
            if (strcmp(opt, "--config") == 0) config_file = argv[arg + 1];
            else if (strcmp(opt, "--state") == 0) { state_root = argv[arg + 1]; state_overridden = 1; }
            else workspace = argv[arg + 1];
            arg += 2;
            continue;
        }
        if (strcmp(opt, "--help") == 0 || strcmp(opt, "-h") == 0 || strcmp(opt, "help") == 0) {
            print_usage(); return 0;
        }
        if (strcmp(opt, "--version") == 0 || strcmp(opt, "-v") == 0) {
            printf("cognitive-os-agent %s\n", version()); return 0;
        }
        if (opt[0] == '-' && opt[1] == '-') {
            fprintf(stderr, "unknown option: %s\n", opt);
            return 2;
        }
        break;
    }
    if (config_file) {
        if (strlen(config_file) >= 600) {
            fprintf(stderr, "config path is too long\n");
            return 2;
        }
        config *probe = config_new();
        if (!probe || config_load_file(probe, config_file) != 0) {
            fprintf(stderr, "cannot read valid JSON config: %s\n", config_file);
            config_free(probe);
            return 2;
        }
        config_free(probe);
        if (!state_overridden) {
            state_root = config_parent(config_file, state_dir, sizeof(state_dir));
            if (!state_root) { fprintf(stderr, "config path is too long\n"); return 2; }
        }
    }
    if (arg < argc && strcmp(argv[arg], "tools") == 0) return cmd_tools();
#ifdef _WIN32
    if (arg < argc && strcmp(argv[arg], "install") == 0)
        return cmd_install(arg + 1 < argc ? argv[arg + 1] : NULL);
#endif
    if (arg < argc && strcmp(argv[arg], "snapshot") == 0)
        return cmd_snapshot(state_root, arg + 1 < argc ? argv[arg + 1] : "list");
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_root = state_root;
    cfg.config_file = config_file;
    cfg.workspace = workspace;
    if (arg < argc && strcmp(argv[arg], "serve") == 0) {
        if (arg + 2 < argc) { fprintf(stderr, "usage: serve [port]\n"); return 2; }
        if (arg + 1 < argc) {
            char *end;
            long port = strtol(argv[arg + 1], &end, 10);
            if (*end || port < 1 || port > 65535) { fprintf(stderr, "invalid port\n"); return 2; }
            cfg.http_port = (uint16_t)port;
        } else {
            config *probe = config_new();
            char path[1200];
            if (config_file) snprintf(path, sizeof(path), "%s", config_file);
            else snprintf(path, sizeof(path), "%s/cognitive-os-agent.json", state_root);
            if (probe) config_load_file(probe, path);
            long port = (long)config_get_int(probe, "http.port", 8080);
            cfg.http_port = (uint16_t)(port > 0 && port <= 65535 ? port : 8080);
            config_free(probe);
        }
    }
    if (init(&ctx, &cfg) != 0) { fprintf(stderr, "initialization failed\n"); return 1; }
    if (arg < argc && strcmp(argv[arg], "serve") == 0) {
        printf("Serving at http://localhost:%u (Ctrl+C to stop)\n", (unsigned)ctx.http_port);
        rc = serve(&ctx);
    } else if (arg < argc && strcmp(argv[arg], "config") == 0) {
        print_config(&ctx);
    } else if (arg < argc && strcmp(argv[arg], "memory") == 0) {
        print_memory(&ctx);
    } else if (arg < argc && strcmp(argv[arg], "run") == 0 && arg + 1 == argc) {
        fprintf(stderr, "usage: run <prompt>\n");
        rc = 2;
    } else if (arg < argc) {
        int from = strcmp(argv[arg], "run") == 0 ? arg + 1 : arg;
        char *prompt = join_args(argc, argv, from);
        cli_progress_state progress = {{0}};
        if (!prompt) rc = 1;
        else {
            reasoning_set_observer(ctx.reasoning, cli_progress, &progress);
            rc = run_prompt(&ctx, prompt);
            reasoning_set_observer(ctx.reasoning, NULL, NULL);
            free(prompt);
        }
    } else {
        cli_progress_state progress = {{0}};
        reasoning_set_observer(ctx.reasoning, cli_progress, &progress);
        printf("Cognitive OS %s · %s / %s\nType a task directly; /help for commands.\n",
               version(), ctx.provider, ctx.llm && ctx.llm->model ? ctx.llm->model : "default");
        if (ctx.provider && strcmp(ctx.provider, "mock") == 0)
            fprintf(stderr, "Offline mock mode. Configure llm.provider, llm.model and llm.api_key for a real model.\n");
        for (;;) {
            printf("❯ ");
            fflush(stdout);
            char *line = read_line();
            if (!line) break;
            char *p = line;
            while (isspace((unsigned char)*p)) p++;
            if (!*p) { free(line); continue; }
            if (strcmp(p, "/exit") == 0 || strcmp(p, "/quit") == 0 || strcmp(p, "exit") == 0) {
                free(line);
                break;
            } else if (strcmp(p, "/help") == 0 || strcmp(p, "help") == 0) print_usage();
            else if (strcmp(p, "/tools") == 0 || strcmp(p, "tools") == 0) cmd_tools();
            else if (strcmp(p, "/memory") == 0 || strcmp(p, "memory") == 0) print_memory(&ctx);
            else if (strcmp(p, "/config") == 0 || strcmp(p, "config") == 0) print_config(&ctx);
            else if (strncmp(p, "/snapshot ", 10) == 0) cmd_snapshot(state_root, p + 10);
            else {
                if (strncmp(p, "run ", 4) == 0) p += 4;
                progress.last[0] = '\0';
                run_prompt(&ctx, p);
            }
            free(line);
        }
        reasoning_set_observer(ctx.reasoning, NULL, NULL);
    }
    runtime_shutdown(&ctx);
    return rc;
}
