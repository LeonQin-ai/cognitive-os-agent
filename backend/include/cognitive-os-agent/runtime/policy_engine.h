/* policy_engine.h — permission & risk assessment for tool invocations.
 * Rules are matched by tool name ("*" matches all). Decisions: ALLOW / DENY / ASK.
 * If an interactive ask callback is not installed, ASK degrades to DENY. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum policy_decision {
    POLICY_ALLOW = 0,
    POLICY_DENY = 1,
    POLICY_ASK = 2,
} policy_decision;

typedef struct policy_engine policy_engine;

/* Ask callback: return 1 to allow, 0 to deny. */
typedef int (*policy_ask_cb)(const char *tool_name, const char *args_json, void *ud);

policy_engine *policy_engine_new(void);
void policy_engine_free(policy_engine *pe);

/* action is "allow", "deny", or "ask". tool_name "*" is a catch-all. */
void policy_add_rule(policy_engine *pe, const char *tool_name, const char *action, const char *reason);

/* Rule management: exact-name rules always beat wildcard rules regardless of
 * registration order; among rules of the same specificity the LAST one wins. */
int policy_rule_count(const policy_engine *pe);
/* Borrowed pointers, valid until the engine changes. Returns 0 ok, -1 range. */
int policy_rule_get(const policy_engine *pe, size_t index, const char **tool, const char **action,
                        const char **reason);
/* Remove the rule at index (no-op if out of range). */
void policy_remove_rule(policy_engine *pe, size_t index);

/* Persist/load rules as a JSON array [{tool,action,reason}] at path.
 * save returns 0 ok; load returns the number of rules loaded (-1 on error). */
int policy_save_file(const policy_engine *pe, const char *path);
int policy_load_file(policy_engine *pe, const char *path);

/* Evaluate policy for a tool call. reason (if non-NULL) receives the decision
 * reason (static string, do not free). */
policy_decision policy_check(policy_engine *pe, const char *tool_name, const char *args_json,
                                     const char **reason);

/* Heuristic risk score 0-100 for a tool call (independent of rules). */
int policy_risk(const char *tool_name, const char *args_json);

#ifdef __cplusplus
}
#endif
