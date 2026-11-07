/* runtime.h — thread-safe production bridge for records + Context MMU. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct memory_runtime memory_runtime;

memory_runtime *memory_runtime_new(const char *state_root);
char *memory_runtime_recall_ex(memory_runtime *rt,const char *scope,const char *query,int char_budget,int include_shared);
void memory_runtime_free(memory_runtime *rt);

/* Returns Context-MMU JSON for the requested isolation scope. */
char *memory_runtime_recall(memory_runtime *rt, const char *scope,
                            const char *query, int char_budget);

/* Promote a completed, evidenced task into the canonical record store. */
int memory_runtime_learn(memory_runtime *rt, const char *scope,
                         const char *task, const char *answer,
                         int successful_tools, int all_actions_ok);

int memory_runtime_count(memory_runtime *rt);

#ifdef __cplusplus
}
#endif
