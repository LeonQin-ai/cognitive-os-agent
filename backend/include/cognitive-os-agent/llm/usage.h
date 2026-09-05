/* usage.h — per-model token consumption accounting.
 * Thread-safe. Callers add prompt/completion token counts; the tracker keeps
 * per-model and global totals and renders them as JSON for the Models UI. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_usage coa_usage;

coa_usage *coa_usage_new(void);
void coa_usage_free(coa_usage *u);

/* Record a completion for a model. */
void coa_usage_add(coa_usage *u, const char *model, long prompt_tokens, long completion_tokens);

long coa_usage_prompt_total(coa_usage *u);
long coa_usage_completion_total(coa_usage *u);
/* JSON object: {models:{<model>:{prompt,completion,calls}}, total:{prompt,completion}} (caller frees). */
char *coa_usage_json(coa_usage *u);

#ifdef __cplusplus
}
#endif
