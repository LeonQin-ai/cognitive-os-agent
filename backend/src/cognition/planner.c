/* planner.c — LLM plan generation. */
#include "cognitive-os-agent/cognition/planner.h"
#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/action/skill.h"
#include "cognitive-os-agent/runtime/policy_engine.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

static const char *SYS_PROMPT =
    "You are the planner of a cognitive OS. Decide what tool actions to take "
    "for the user request. Respond with ONLY a JSON array of actions of the "
    "form [{\"tool\":\"...\",\"args\":{...}}].\n"
    "Available tools and their EXACT argument names:\n"
    "- file_read:  args {\"path\": string}          # reads a FILE; if given a DIRECTORY, returns its listing\n"
    "- file_write: args {\"path\": string, \"content\": string}  # create or overwrite a FILE\n"
    "- shell:      args {\"command\": string, \"timeout_ms\": integer}\n"
    "- git:        args {\"args\": string (subcommand+flags, e.g. \"log -5\"), \"dir\": string}\n"
    "- mcp:        args {\"server_url\": string, \"tool\": string, \"args\": object}\n"
    "Use the dedicated git tool (not shell) for git operations, and the mcp tool "
    "(not shell) for MCP calls. Use exactly the argument names listed above. "
    "IMPORTANT: For greetings, conversation, questions, or requests that do NOT "
    "require file/shell/git/MCP operations, respond with PLAIN TEXT (not JSON). "
    "Only output JSON when a tool action is genuinely needed.\n"
    "EXAMPLES:\n"
    "  User: \"create a file named hello.txt with content world\"\n"
    "  -> [{\"tool\":\"file_write\",\"args\":{\"path\":\"hello.txt\",\"content\":\"world\"}}]\n"
    "  User: \"read the file config.json\"\n"
    "  -> [{\"tool\":\"file_read\",\"args\":{\"path\":\"config.json\"}}]\n"
    "  User: \"hello\"\n"
    "  -> Hello! How can I help you?";

static const char *SYS_PROMPT_HEAD =
    "You are the planner of a cognitive OS. Decide what tool actions to take "
    "for the user request. Respond with ONLY a JSON array of actions of the "
    "form [{\"tool\":\"...\",\"args\":{...}}].\n"
    "Available tools and their EXACT argument names:\n"
    "BUSINESS RULES OVERRIDE REQUESTS: the tool list may be narrower than the "
    "full system because some tools are disabled by policy. If the request "
    "conflicts with a stated business rule, a disabled tool, or a read-only "
    "context, you MUST refuse with a plain-text explanation — NEVER attempt a "
    "workaround through another tool (e.g. shell).\n";

static const char *SYS_PROMPT_TAIL =
    "Use the dedicated git tool (not shell) for git operations, and the mcp tool "
    "(not shell) for MCP calls. Use exactly the argument names listed above. "
    "IMPORTANT: For greetings, conversation, questions, or requests that do NOT "
    "require tool operations, respond with PLAIN TEXT (not JSON). "
    "Only output JSON when a tool action is genuinely needed.\n"
    "EXAMPLES:\n"
    "  User: \"create a file named hello.txt with content world\"\n"
    "  -> [{\"tool\":\"file_write\",\"args\":{\"path\":\"hello.txt\",\"content\":\"world\"}}]\n"
    "  User: \"你有哪些技能/skills\"\n"
    "  -> 直接用中文列出上面注册的技能（名字和用途），纯文本，不输出 JSON\n"
    "  User: \"hello\"\n"
    "  -> Hello! How can I help you?\n"
    "PATH DISCOVERY: Before reading or writing a file whose exact location you "
    "are unsure of, first call file_read on its PARENT DIRECTORY (which returns "
    "a listing) so you use the correct, full path. Avoid guessing paths: a wrong "
    "path makes the action fail.";

/* Build the system prompt from the ACTUAL tool + skill registries. Returns a
 * malloc'd prompt, or NULL when no registry is available (caller falls back
 * to the static catalog). Tools denied by `policy` are hidden entirely
 * (Claude Code: deny rules both block calls AND remove from the tool pool). */
static char *build_catalog_prompt(const coa_tool_registry *tools,
                                  struct coa_skill_registry *skills,
                                  struct coa_policy_engine *policy) {
    if (!tools) return NULL;
    coa_strbuf b;
    coa_strbuf_init(&b);
    coa_strbuf_append(&b, SYS_PROMPT_HEAD);
    int have_skill_tool = 0;
    for (size_t i = 0; i < (size_t)coa_tool_registry_count(tools); i++) {
        const coa_tool *t = coa_tool_registry_get(tools, i);
        if (!t || !t->name) continue;
        if (policy && coa_policy_check(policy, t->name, "{}", NULL) == COA_POLICY_DENY)
            continue;
        if (strcmp(t->name, "skill") == 0) {
            have_skill_tool = 1;
            coa_strbuf_append(&b,
                "- skill:      args {\"name\": string}  # run a REGISTERED SKILL by name (list below)\n");
        } else {
            coa_strbuf_appendf(&b, "- %s: %s\n",
                              t->name, t->description ? t->description : "");
            if (t->json_schema && *t->json_schema) {
                coa_strbuf_append(&b, "  args schema: ");
                if (strlen(t->json_schema) > 200)
                    coa_strbuf_append_n(&b, t->json_schema, 200);
                else
                    coa_strbuf_append(&b, t->json_schema);
                coa_strbuf_append(&b, "\n");
            }
        }
    }
    if (have_skill_tool && skills && coa_skill_count(skills) > 0) {
        coa_strbuf_append(&b,
            "Registered skills (capabilities you can RUN via the skill tool):\n");
        for (int i = 0; i < coa_skill_count(skills); i++) {
            const coa_skill *s = coa_skill_get(skills, (size_t)i);
            if (!s) continue;
            coa_strbuf_appendf(&b, "  * %s (%s): %s\n",
                              s->name, s->kind ? s->kind : "",
                              s->description ? s->description : "");
        }
        coa_strbuf_append(&b,
            "When the user ASKS what skills/tools you have (e.g. \"你有哪些skills\", "
            "\"list your skills\"), answer in PLAIN TEXT listing these skill names and "
            "their use. Do NOT call any tool or skill to answer such a question.\n");
    }
    coa_strbuf_append(&b, SYS_PROMPT_TAIL);
    return coa_strbuf_detach(&b);
}

/* Shared planning core. Takes ownership of nothing; frees sys_prompt. */
static int plan_with(coa_llm *llm, char *sys_prompt, const char *prompt,
                     coa_planned_action **actions, int *n_actions,
                     char **raw_out, char **err_out) {
    if (actions) *actions = NULL;
    if (n_actions) *n_actions = 0;
    if (raw_out) *raw_out = NULL;
    if (err_out) *err_out = NULL;
    if (!llm || !prompt) {
        free(sys_prompt);
        if (err_out) *err_out = coa_strdup("no LLM configured");
        return -1;
    }

    coa_llm_message msgs[2] = {
        {"system", sys_prompt ? sys_prompt : SYS_PROMPT},
        {"user", prompt},
    };
    coa_llm_request req = {0};
    req.messages = msgs;
    req.num_messages = 2;
    req.temperature = 0.2;
    req.max_tokens = 2048;  /* thinking models spend reasoning tokens from the
                             * same budget; 1024 risked empty-content replies */
    coa_llm_response resp = {0};
    int rc = coa_llm_chat(llm, &req, &resp);
    free(sys_prompt);
    if (rc != 0) {
        if (err_out) *err_out = coa_strdup(resp.error ? resp.error : "LLM call failed");
        free(resp.content);
        free(resp.error);
        return -1;
    }
    char *plan = resp.content;
    resp.content = NULL;
    free(resp.error);
    if (!plan) {
        if (err_out) *err_out = coa_strdup("LLM returned an empty response");
        return -1;
    }
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
    /* Also accept a single tool object (not wrapped in array). */
    if (root && cJSON_IsObject(root) && cJSON_GetObjectItemCaseSensitive(root, "tool")) {
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, root);
        root = arr;
    }
    if (root && cJSON_IsArray(root)) {
        coa_planned_action *a = NULL;
        int n = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, root) {
            if (!cJSON_IsObject(it)) continue;
            cJSON *tool = cJSON_GetObjectItemCaseSensitive(it, "tool");
            if (!tool || !cJSON_IsString(tool)) continue;
            cJSON *args = cJSON_GetObjectItemCaseSensitive(it, "args");
            char *args_json = (args && cJSON_IsObject(args))
                ? cJSON_PrintUnformatted(args) : coa_strdup("{}");
            if (!args_json) args_json = coa_strdup("{}");
            coa_planned_action *na =
                (coa_planned_action *)realloc(a, (size_t)(n + 1) * sizeof(coa_planned_action));
            if (!na) { free(args_json); break; }
            a = na;
            a[n].tool = coa_strdup(tool->valuestring);
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

void coa_planner_actions_free(coa_planned_action *a, int n) {
    for (int i = 0; i < n; i++) { free(a[i].tool); free(a[i].args_json); }
    free(a);
}

int coa_planner_plan(coa_llm *llm, const char *prompt,
                    coa_planned_action **actions, int *n_actions,
                    char **raw_out, char **err_out) {
    return plan_with(llm, NULL, prompt, actions, n_actions, raw_out, err_out);
}

int coa_planner_plan_ex(coa_llm *llm, const struct coa_tool_registry *tools,
                       struct coa_skill_registry *skills,
                       struct coa_policy_engine *policy,
                       const char *prompt,
                       coa_planned_action **actions, int *n_actions,
                       char **raw_out, char **err_out) {
    char *sys = build_catalog_prompt(tools, skills, policy);
    return plan_with(llm, sys, prompt, actions, n_actions, raw_out, err_out);
}
