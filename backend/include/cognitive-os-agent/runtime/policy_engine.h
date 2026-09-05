/* policy_engine.h — permission & risk assessment for tool invocations.
 * Rules are matched by tool name ("*" matches all). Decisions: ALLOW / DENY / ASK.
 * If an interactive ask callback is not installed, ASK degrades to DENY. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum coa_policy_decision {
    COA_POLICY_ALLOW = 0,
    COA_POLICY_DENY  = 1,
    COA_POLICY_ASK   = 2,
} coa_policy_decision;

typedef struct coa_policy_engine coa_policy_engine;

/* Ask callback: return 1 to allow, 0 to deny. */
typedef int (*coa_policy_ask_cb)(const char *tool_name, const char *args_json, void *ud);

coa_policy_engine *coa_policy_engine_new(void);
void coa_policy_engine_free(coa_policy_engine *pe);

/* action is "allow", "deny", or "ask". tool_name "*" is a catch-all. */
void coa_policy_add_rule(coa_policy_engine *pe, const char *tool_name, const char *action,
                        const char *reason);

/* Rule management: exact-name rules always beat wildcard rules regardless of
 * registration order; among rules of the same specificity the LAST one wins. */
int coa_policy_rule_count(const coa_policy_engine *pe);
/* Borrowed pointers, valid until the engine changes. Returns 0 ok, -1 range. */
int coa_policy_rule_get(const coa_policy_engine *pe, size_t index, const char **tool,
                       const char **action, const char **reason);
/* Remove the rule at index (no-op if out of range). */
void coa_policy_remove_rule(coa_policy_engine *pe, size_t index);

/* Persist/load rules as a JSON array [{tool,action,reason}] at path.
 * save returns 0 ok; load returns the number of rules loaded (-1 on error). */
int coa_policy_save_file(const coa_policy_engine *pe, const char *path);
int coa_policy_load_file(coa_policy_engine *pe, const char *path);

/* Evaluate policy for a tool call. reason (if non-NULL) receives the decision
 * reason (static string, do not free). */
coa_policy_decision coa_policy_check(coa_policy_engine *pe, const char *tool_name,
                                   const char *args_json, const char **reason);

/* Heuristic risk score 0-100 for a tool call (independent of rules). */
int coa_policy_risk(const char *tool_name, const char *args_json);

void coa_policy_set_ask_cb(coa_policy_engine *pe, coa_policy_ask_cb cb, void *ud);

#ifdef __cplusplus
}
#endif
