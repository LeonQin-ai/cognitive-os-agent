/* usage.h — per-model token consumption accounting.
 * Thread-safe. Callers add prompt/completion token counts; the tracker keeps
 * per-model and global totals and renders them as JSON for the Models UI. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_usage ca_usage;

ca_usage *ca_usage_new(void);
void ca_usage_free(ca_usage *u);

/* Record a completion for a model. */
void ca_usage_add(ca_usage *u, const char *model, long prompt_tokens, long completion_tokens);

long ca_usage_prompt_total(ca_usage *u);
long ca_usage_completion_total(ca_usage *u);
/* JSON object: {models:{<model>:{prompt,completion,calls}}, total:{prompt,completion}} (caller frees). */
char *ca_usage_json(ca_usage *u);

#ifdef __cplusplus
}
#endif
