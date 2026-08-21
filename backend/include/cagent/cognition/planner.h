/* planner.h — LLM plan generation.
 * Asks the model for a JSON plan of tool actions for a user request. This is
 * the "planner" concern extracted from the reasoning engine: a cognitive
 * accelerator that proposes actions, not the control center. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_llm ca_llm;

typedef struct ca_planned_action {
    char *tool;      /* tool name, e.g. "file_write" */
    char *args_json; /* JSON object string of arguments, e.g. "{\"path\":\"a\"}" */
} ca_planned_action;

/* Plan tool actions for `prompt` via the LLM. Returns 0 ok, -1 error (no LLM
 * or no response). On success:
 *   - *actions / *n_actions hold the parsed plan (0 actions if the model
 *     answered in plain text with no tools). Caller frees via
 *     ca_planner_actions_free().
 *   - *raw_out holds the verbatim model output (caller frees; may be NULL). */
int ca_planner_plan(ca_llm *llm, const char *prompt,
                    ca_planned_action **actions, int *n_actions, char **raw_out);

void ca_planner_actions_free(ca_planned_action *a, int n);

#ifdef __cplusplus
}
#endif
