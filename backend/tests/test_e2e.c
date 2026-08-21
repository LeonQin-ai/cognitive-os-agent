/* test_e2e.c — end-to-end test: run a prompt through the full reasoning
 * pipeline against a real HTTP LLM endpoint (the bundled mock-llm-server) with
 * the OpenAI adapter, verify the tool executed, the file exists, and the tx
 * snapshot captured it.
 *
 * Requires: ./build/mock-llm-server 9000  (running first)
 * Build:    zig cc -Iinclude -Ithird_party/cJSON $(find src third_party -name '*.c') \
 *               tests/test_e2e.c -o build/cagent-e2e
 */
#include "cagent/cagent.h"
#include "cagent/os/os_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (cond) { printf("  ok   %s\n", #cond); } \
                        else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main(void) {
    const char *base_url = "http://localhost:9000";
    const char *providers[] = {"openai", "anthropic"};

    for (size_t pi = 0; pi < sizeof(providers) / sizeof(char *); pi++) {
        const char *provider = providers[pi];
        char root[64], file[128];
        snprintf(root, sizeof(root), "state-e2e-%s", provider);
        snprintf(file, sizeof(file), "%s/w/note.txt", root);
        printf("== provider=%s ==\n", provider);

        char workspace[80];
        snprintf(workspace, sizeof(workspace), "%s/w", root);
        snprintf(file, sizeof(file), "%s/note.txt", workspace);

        cagent_ctx ctx;
        cagent_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = root;
        cfg.workspace = workspace;
        cfg.provider = provider;
        cfg.base_url = base_url;
        cfg.api_key = provider[0] == 'a' ? "test-key" : NULL;
        cfg.model = "mock-model";
        cfg.http_port = 0;         /* no HTTP API in this test */
        cfg.workers = 1;

        CHECK(cagent_init(&ctx, &cfg) == 0);

        char *answer = NULL;
        int rc = cagent_run(&ctx, "创建 note.txt 写入内容为 hello-e2e", &answer);
        CHECK(rc == 0);
        if (answer) {
            printf("  answer: %s\n", answer);
            CHECK(strstr(answer, "ok") != NULL);
            free(answer);
        }

        /* the tool should have created the file via the LLM adapter */
        char *data = ca_fs_read_file(file);
        CHECK(data != NULL);
        if (data) {
            CHECK(strstr(data, "hello-e2e") != NULL);
            free(data);
        }

        /* snapshot should list the captured (resolved) path */
        char *list = ca_snapshot_list(ctx.snapshot);
        CHECK(list != NULL);
        if (list) {
            printf("  snapshots: %s\n", list);
            CHECK(strstr(list, "note.txt") != NULL);
            free(list);
        }

        cagent_shutdown(&ctx);

        /* cleanup */
        ca_fs_remove(file);
        ca_fs_remove(workspace);
        ca_fs_remove(root);
    }

    printf(g_fail == 0 ? "E2E PASS\n" : "E2E FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
