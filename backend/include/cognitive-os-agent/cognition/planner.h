/* planner.h — LLM plan generation.
 * Asks the model for a JSON plan of tool actions for a user request. This is
 * the "planner" concern extracted from the reasoning engine: a cognitive
 * accelerator that proposes actions, not the control center. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_llm coa_llm;
struct coa_tool_registry;   /* action/tools.h */
struct coa_skill_registry;  /* action/skill.h */
struct coa_policy_engine;   /* runtime/policy_engine.h */

typedef struct coa_planned_action {
    char *tool;      /* tool name, e.g. "file_write" */
    char *args_json; /* JSON object string of arguments, e.g. "{\"path\":\"a\"}" */
} coa_planned_action;

/* Plan tool actions for `prompt` via the LLM. Returns 0 ok, -1 error (no LLM
 * or no response). On success:
 *   - *actions / *n_actions hold the parsed plan (0 actions if the model
 *     answered in plain text with no tools). Caller frees via
 *     coa_planner_actions_free().
 *   - *raw_out holds the verbatim model output (caller frees; may be NULL).
 *   - *err_out (optional) holds a malloc'd diagnostic on failure; caller frees.
 * Uses a static built-in tool catalog (legacy/test entry point). */
int coa_planner_plan(coa_llm *llm, const char *prompt,
                    coa_planned_action **actions, int *n_actions,
                    char **raw_out, char **err_out);

/* Same as coa_planner_plan, but the system prompt is built dynamically from
 * the ACTUAL registered tools and skills, so the model sees (and can invoke
 * via the "skill" tool) everything the runtime has. NULL registries fall
 * back to the static catalog. `policy` (may be NULL) hides denied tools from
 * the catalog — deny rules both block calls and remove the tool from the
 * pool the model can see. */
int coa_planner_plan_ex(coa_llm *llm, const struct coa_tool_registry *tools,
                       struct coa_skill_registry *skills,
                       struct coa_policy_engine *policy,
                       const char *prompt,
                       coa_planned_action **actions, int *n_actions,
                       char **raw_out, char **err_out);

void coa_planner_actions_free(coa_planned_action *a, int n);

#ifdef __cplusplus
}
#endif
