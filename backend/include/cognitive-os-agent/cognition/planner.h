/* planner.h — LLM plan generation.
 * Asks the model for a JSON plan of tool actions for a user request. This is
 * the "planner" concern extracted from the reasoning engine: a cognitive
 * accelerator that proposes actions, not the control center. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct llm llm;
struct tool_registry;  /* action/tools.h */
struct skill_registry; /* action/skill.h */
struct policy_engine;  /* runtime/policy_engine.h */

typedef struct planned_action {
    char *tool;      /* tool name, e.g. "file_write" */
    char *args_json; /* JSON object string of arguments, e.g. "{\"path\":\"a\"}" */
} planned_action;

/* Plan tool actions for `prompt` via the LLM. Returns 0 ok, -1 error (no LLM
 * or no response). On success:
 *   - *actions / *n_actions hold the parsed plan (0 actions if the model
 *     answered in plain text with no tools). Caller frees via
 *     planner_actions_free().
 *   - *raw_out holds the verbatim model output (caller frees; may be NULL).
 *   - *err_out (optional) holds a malloc'd diagnostic on failure; caller frees.
 * Uses a static built-in tool catalog (legacy/test entry point). */
int planner_plan(llm *llm, const char *prompt, planned_action **actions, int *n_actions, char **raw_out,
                     char **err_out);

/* Same as planner_plan, but the system prompt is built dynamically from
 * the ACTUAL registered tools and skills, so the model sees (and can invoke
 * via the "skill" tool) everything the runtime has. NULL registries fall
 * back to the static catalog. `policy` (may be NULL) hides denied tools from
 * the catalog — deny rules both block calls and remove the tool from the
 * pool the model can see. */
int planner_plan_ex(llm *llm, const struct tool_registry *tools, struct skill_registry *skills,
                        struct policy_engine *policy, const char *prompt, planned_action **actions,
                        int *n_actions, char **raw_out, char **err_out);

void planner_actions_free(planned_action *a, int n);

#ifdef __cplusplus
}
#endif
