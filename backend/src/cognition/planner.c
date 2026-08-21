/* planner.c — LLM plan generation. */
#include "cagent/cognition/planner.h"
#include "cagent/llm/llm.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

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

int ca_planner_plan(ca_llm *llm, const char *prompt,
                    ca_planned_action **actions, int *n_actions, char **raw_out) {
    if (actions) *actions = NULL;
    if (n_actions) *n_actions = 0;
    if (raw_out) *raw_out = NULL;
    if (!llm || !prompt) return -1;

    char *plan = ca_llm_chat_simple(llm, SYS_PROMPT, prompt);
    if (!plan) return -1;
    if (raw_out) *raw_out = plan;
    else free(plan);

    if (!actions || !n_actions) return 0;

    cJSON *root = cJSON_Parse(plan);
    if (!root) {
        /* Real LLMs often wrap the JSON array in markdown fences or prose.
         * Extract the first '[' .. last ']' span and retry so tool plans are
         * still honoured. */
        const char *start = strchr(plan, '[');
        const char *end = start ? strrchr(plan, ']') : NULL;
        if (start && end && end > start) {
            size_t n = (size_t)(end - start) + 1;
            char *slice = (char *)malloc(n + 1);
            if (slice) {
                memcpy(slice, start, n);
                slice[n] = '\0';
                root = cJSON_Parse(slice);
                free(slice);
            }
        }
    }
    if (root && cJSON_IsArray(root)) {
        ca_planned_action *a = NULL;
        int n = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, root) {
            if (!cJSON_IsObject(it)) continue;
            cJSON *tool = cJSON_GetObjectItemCaseSensitive(it, "tool");
            if (!tool || !cJSON_IsString(tool)) continue;
            cJSON *args = cJSON_GetObjectItemCaseSensitive(it, "args");
            char *args_json = (args && cJSON_IsObject(args))
                ? cJSON_PrintUnformatted(args) : ca_strdup("{}");
            if (!args_json) args_json = ca_strdup("{}");
            ca_planned_action *na =
                (ca_planned_action *)realloc(a, (size_t)(n + 1) * sizeof(ca_planned_action));
            if (!na) { free(args_json); break; }
            a = na;
            a[n].tool = ca_strdup(tool->valuestring);
            a[n].args_json = args_json;
            n++;
        }
        cJSON_Delete(root);
        *actions = a;
        *n_actions = n;
    } else {
        if (root) cJSON_Delete(root);
        /* plain text answer: no tool plan */
    }
    return 0;
}

void ca_planner_actions_free(ca_planned_action *a, int n) {
    for (int i = 0; i < n; i++) { free(a[i].tool); free(a[i].args_json); }
    free(a);
}
