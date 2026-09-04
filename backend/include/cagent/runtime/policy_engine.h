/* policy_engine.h — permission & risk assessment for tool invocations.
 * Rules are matched by tool name ("*" matches all). Decisions: ALLOW / DENY / ASK.
 * If an interactive ask callback is not installed, ASK degrades to DENY. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ca_policy_decision {
    CA_POLICY_ALLOW = 0,
    CA_POLICY_DENY  = 1,
    CA_POLICY_ASK   = 2,
} ca_policy_decision;

typedef struct ca_policy_engine ca_policy_engine;

/* Ask callback: return 1 to allow, 0 to deny. */
typedef int (*ca_policy_ask_cb)(const char *tool_name, const char *args_json, void *ud);

ca_policy_engine *ca_policy_engine_new(void);
void ca_policy_engine_free(ca_policy_engine *pe);

/* action is "allow", "deny", or "ask". tool_name "*" is a catch-all. */
void ca_policy_add_rule(ca_policy_engine *pe, const char *tool_name, const char *action,
                        const char *reason);

/* Rule management: exact-name rules always beat wildcard rules regardless of
 * registration order; among rules of the same specificity the LAST one wins. */
int ca_policy_rule_count(const ca_policy_engine *pe);
/* Borrowed pointers, valid until the engine changes. Returns 0 ok, -1 range. */
int ca_policy_rule_get(const ca_policy_engine *pe, size_t index, const char **tool,
                       const char **action, const char **reason);
/* Remove the rule at index (no-op if out of range). */
void ca_policy_remove_rule(ca_policy_engine *pe, size_t index);

/* Persist/load rules as a JSON array [{tool,action,reason}] at path.
 * save returns 0 ok; load returns the number of rules loaded (-1 on error). */
int ca_policy_save_file(const ca_policy_engine *pe, const char *path);
int ca_policy_load_file(ca_policy_engine *pe, const char *path);

/* Evaluate policy for a tool call. reason (if non-NULL) receives the decision
 * reason (static string, do not free). */
ca_policy_decision ca_policy_check(ca_policy_engine *pe, const char *tool_name,
                                   const char *args_json, const char **reason);

/* Heuristic risk score 0-100 for a tool call (independent of rules). */
int ca_policy_risk(const char *tool_name, const char *args_json);

void ca_policy_set_ask_cb(ca_policy_engine *pe, ca_policy_ask_cb cb, void *ud);

#ifdef __cplusplus
}
#endif
