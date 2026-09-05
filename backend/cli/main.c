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
#include "cognitive-os-agent/cognitive-os-agent.h"
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/memory/memory.h"
#include "cognitive-os-agent/snapshot/snapshot.h"
#include "cognitive-os-agent/infra/config.h"

#define STATE_ROOT "state"

static void print_usage(void) {
    printf("cognitive-os-agent %s — Cognitive OS Runtime\n", coa_version());
    printf("usage:\n");
    printf("  cognitive-os-agent run \"<prompt>\"         run one prompt through the cognitive pipeline\n");
    printf("  cognitive-os-agent serve [port]            serve HTTP API + web console (default 8080)\n");
    printf("  cognitive-os-agent tools                   list available tools\n");
    printf("  cognitive-os-agent memory                  show working + long-term memory\n");
    printf("  cognitive-os-agent snapshot list|rollback  list snapshots / rollback to latest\n");
    printf("  cognitive-os-agent config                  show effective config\n");
    printf("  cognitive-os-agent                         interactive shell\n");
}

static int cmd_tools(void) {
    coa_tool_registry *reg = coa_tool_registry_new();
    coa_tool_register_builtins(reg);
    int n = coa_tool_registry_count(reg);
    printf("%d tools:\n", n);
    for (int i = 0; i < n; i++) {
        const coa_tool *t = coa_tool_registry_get(reg, (size_t)i);
        printf("  %-12s %s%s\n", t->name, t->description ? t->description : "",
               t->is_write ? "  [write]" : "");
    }
    coa_tool_registry_free(reg);
    return 0;
}

static int cmd_snapshot(const char *sub) {
    coa_snapshot *s = coa_snapshot_open(STATE_ROOT);
    if (!s) {
        printf("error: cannot open snapshot store at %s\n", STATE_ROOT);
        return 1;
    }
    int rc = 0;
    if (strcmp(sub, "list") == 0) {
        char *j = coa_snapshot_list(s);
        printf("%s\n", j ? j : "[]");
        free(j);
    } else if (strcmp(sub, "rollback") == 0 || strcmp(sub, "restore") == 0) {
        rc = coa_snapshot_restore_latest(s);
        printf(rc == 0 ? "rolled back to latest snapshot\n" : "no snapshot to restore\n");
    } else {
        printf("usage: snapshot list|rollback\n");
    }
    coa_snapshot_close(s);
    return rc;
}

static int run_prompt(coa_ctx *ctx, const char *prompt) {
    char *answer = NULL;
    int rc = coa_run(ctx, prompt, &answer);
    printf("\n==== answer (%s) ====\n", rc == 0 ? "ok" : "failed");
    if (answer) {
        printf("%s\n", answer);
        free(answer);
    }
    return rc;
}

static void print_memory(coa_ctx *ctx) {
    char *w = coa_memory_working_json(ctx->memory);
    char *l = coa_memory_longterm_json(ctx->memory);
    printf("working:\n  %s\nlongterm:\n  %s\n", w ? w : "[]", l ? l : "{}");
    free(w);
    free(l);
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
        printf("cognitive-os-agent %s\n", coa_version());
        return 0;
    }
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
                     strcmp(argv[1], "help") == 0)) {
        print_usage();
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "tools") == 0) return cmd_tools();
    if (argc > 1 && strcmp(argv[1], "snapshot") == 0)
        return cmd_snapshot(argc > 2 ? argv[2] : "list");
    if (argc > 1 && strcmp(argv[1], "run") == 0) {
        if (argc < 3) {
            printf("usage: cognitive-os-agent run \"<prompt>\"\n");
            return 1;
        }
        coa_ctx ctx;
        coa_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        /* provider/model/base_url resolved from config defaults -> state/cognitive-os-agent.json
         * -> COA_* env vars (defaults to the offline "mock" provider) */
        if (coa_init(&ctx, &cfg) != 0) { printf("init failed\n"); return 1; }
        int rc = run_prompt(&ctx, argv[2]);
        coa_shutdown(&ctx);
        return rc;
    }
    if (argc > 1 && strcmp(argv[1], "serve") == 0) {
        uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 8080;
        coa_ctx ctx;
        coa_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.http_port = port;
        if (coa_init(&ctx, &cfg) != 0) { printf("init failed\n"); return 1; }
        printf("cognitive-os-agent serving at http://localhost:%u  (Ctrl+C to stop)\n", (unsigned)port);
        coa_serve(&ctx);
        coa_shutdown(&ctx);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "memory") == 0) {
        coa_ctx ctx;
        coa_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        if (coa_init(&ctx, &cfg) != 0) { printf("init failed\n"); return 1; }
        print_memory(&ctx);
        coa_shutdown(&ctx);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "config") == 0) {
        coa_ctx ctx;
        coa_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        if (coa_init(&ctx, &cfg) != 0) { printf("init failed\n"); return 1; }
        char *j = coa_config_to_json(ctx.config);
        printf("%s\n", j ? j : "{}");
        free(j);
        coa_shutdown(&ctx);
        return 0;
    }
    if (argc > 1) {
        printf("unknown command: %s\n", argv[1]);
        print_usage();
        return 1;
    }

    /* interactive shell */
    coa_ctx ctx;
    coa_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (coa_init(&ctx, &cfg) != 0) { printf("init failed\n"); return 1; }
    printf("cognitive-os-agent %s — Cognitive OS Runtime. Type 'help' for commands.\n", coa_version());

    char line[4096];
    for (;;) {
        printf("cognitive-os-agent> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
        if (strcmp(line, "help") == 0) { print_usage(); continue; }
        if (strcmp(line, "tools") == 0) { cmd_tools(); continue; }
        if (strcmp(line, "memory") == 0) { print_memory(&ctx); continue; }
        if (strncmp(line, "run ", 4) == 0) { run_prompt(&ctx, line + 4); continue; }
        if (strncmp(line, "snapshot ", 9) == 0) { cmd_snapshot(line + 9); continue; }
        printf("unknown command: %s\n", line);
    }
    coa_shutdown(&ctx);
    return 0;
}
