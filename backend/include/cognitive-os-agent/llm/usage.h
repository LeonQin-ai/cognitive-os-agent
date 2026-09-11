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

long usage_prompt_total(usage *u);
long usage_completion_total(usage *u);
/* JSON object: {models:{<model>:{prompt,completion,calls}}, total:{prompt,completion}} (caller frees). */
char *usage_json(usage *u);

#ifdef __cplusplus
}
#endif
