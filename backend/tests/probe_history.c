/* probe_history.c — one-off manual probe: reproduce the "history pollution"
 * bug (agent cites past session notes and skips a NEW similar request).
 * Run 1 completes a real task; Run 2 asks a similar-but-new task. Before the
 * fix the agent answered "already completed" citing history without acting;
 * after the fix it must plan and execute fresh actions.
 *
 * Build: see command in docs. Uses the user's LLM config from state/cognitive-os-agent.json
 * (api_key is never printed). Temp state root: state-probe.
 */
#include "cognitive-os-agent/cognitive-os-agent.h"
#include "cognitive-os-agent/os/os_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

int main(void) {
    /* read the user's live LLM config (state/cognitive-os-agent.json) */
    char *cfgtxt = coa_fs_read_file("state/cognitive-os-agent.json");
    if (!cfgtxt) { printf("no state/cognitive-os-agent.json\n"); return 1; }
    cJSON *root = cJSON_Parse(cfgtxt);
    free(cfgtxt);
    if (!root) { printf("bad config json\n"); return 1; }
    const char *provider = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "llm.provider"));
    const char *base_url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "llm.base_url"));
    const char *api_key  = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "llm.api_key"));
    const char *model    = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "llm.model"));
    if (!provider || !base_url || !api_key || !model) {
        printf("config missing llm fields\n"); cJSON_Delete(root); return 1;
    }
    printf("provider=%s model=%s (key hidden)\n", provider, model);

    coa_ctx ctx;
    coa_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_root = "state-probe";
    cfg.workspace = "state-probe/w";
    cfg.provider = provider;
    cfg.base_url = base_url;
    cfg.api_key = api_key;
    cfg.model = model;
    cfg.http_port = 0;
    cfg.workers = 1;

    if (coa_init(&ctx, &cfg) != 0) { printf("init failed\n"); return 1; }

    /* run 1: a real task that completes (creates a file) */
    char *a1 = NULL;
    int rc1 = coa_run(&ctx, "在 state-probe/w 目录创建 probe-a.txt，内容写 probe-run-one", &a1);
    printf("\n=== RUN1 rc=%d ===\n%s\n", rc1, a1 ? a1 : "(null)");
    free(a1);

    /* run 2: a similar-but-NEW request — must not be skipped via history */
    char *a2 = NULL;
    int rc2 = coa_run(&ctx, "查看 state-probe/w 目录下 probe-a.txt 的内容并告诉我", &a2);
    printf("\n=== RUN2 rc=%d ===\n%s\n", rc2, a2 ? a2 : "(null)");

    int acted = a2 && strstr(a2, "[file_read]") != NULL;
    printf("\nRUN2 acted=%s\n", acted ? "YES (fresh actions executed)" : "NO (may still be skipping)");
    free(a2);

    coa_shutdown(&ctx);
    cJSON_Delete(root);
    return acted ? 0 : 2;
}
