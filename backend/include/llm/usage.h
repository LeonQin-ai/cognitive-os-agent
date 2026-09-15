/* usage.h — per-model token consumption accounting.
 * Thread-safe. Callers add prompt/completion token counts; the tracker keeps
 * per-model and global totals and renders them as JSON for the Models UI. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct usage usage;

usage *usage_new(void);
void usage_free(usage *u);

/* Record a completion for a model. */
void usage_add(usage *u, const char *model, long prompt_tokens, long completion_tokens);

/* Same, but `reasoning_tokens` is tracked separately: thinking models report
 * reasoning tokens INSIDE completion_tokens, and the dashboard must show
 * visible output apart from invisible thinking (GitHub issue #7). */
void usage_add_ex(usage *u, const char *model, long prompt_tokens, long completion_tokens,
                  long reasoning_tokens);

long usage_prompt_total(usage *u);
long usage_completion_total(usage *u);
/* JSON object: {models:{<model>:{prompt,completion,reasoning,calls}}, total:{prompt,completion,reasoning}} (caller frees). */
char *usage_json(usage *u);

#ifdef __cplusplus
}
#endif
