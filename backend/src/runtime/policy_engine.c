#include "cagent/runtime/policy_engine.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

typedef struct rule {
    char *tool;
    int decision;   /* 0 allow, 1 deny, 2 ask */
    char *reason;
} rule;

struct ca_policy_engine {
    rule *rules;
    size_t count, cap;
    ca_policy_ask_cb ask_cb;
    void *ask_ud;
};

ca_policy_engine *ca_policy_engine_new(void) {
    return calloc(1, sizeof(ca_policy_engine));
}

void ca_policy_engine_free(ca_policy_engine *pe) {
    if (!pe) return;
    for (size_t i = 0; i < pe->count; i++) {
        free(pe->rules[i].tool);
        free(pe->rules[i].reason);
    }
    free(pe->rules);
    free(pe);
}

static int parse_action(const char *action) {
    if (!action) return 1; /* default deny */
    if (strcmp(action, "allow") == 0) return 0;
    if (strcmp(action, "deny") == 0) return 1;
    if (strcmp(action, "ask") == 0) return 2;
    return 1;
}

void ca_policy_add_rule(ca_policy_engine *pe, const char *tool_name, const char *action,
                        const char *reason) {
    if (!pe || !tool_name) return;
    if (pe->count == pe->cap) {
        size_t cap = pe->cap ? pe->cap * 2 : 8;
        pe->rules = realloc(pe->rules, cap * sizeof(rule));
        pe->cap = cap;
    }
    rule *r = &pe->rules[pe->count++];
    r->tool = ca_strdup(tool_name);
    r->decision = parse_action(action);
    r->reason = reason ? ca_strdup(reason) : NULL;
}

ca_policy_decision ca_policy_check(ca_policy_engine *pe, const char *tool_name,
                                   const char *args_json, const char **reason) {
    /* most specific match: exact name beats wildcard; last rule wins */
    const rule *match = NULL;
    int wildcard = 0;
    for (size_t i = 0; i < pe->count; i++) {
        if (strcmp(pe->rules[i].tool, tool_name) == 0) { match = &pe->rules[i]; wildcard = 0; }
        else if (strcmp(pe->rules[i].tool, "*") == 0) { match = &pe->rules[i]; wildcard = 1; }
    }
    if (match) {
        if (reason) *reason = match->reason ? match->reason : "rule match";
        if (match->decision == 2) {
            if (pe->ask_cb && pe->ask_cb(tool_name, args_json, pe->ask_ud)) return CA_POLICY_ALLOW;
            if (reason) *reason = "ask denied";
            return CA_POLICY_DENY;
        }
        return (ca_policy_decision)match->decision;
    }
    (void)wildcard;
    /* default: allow if no rules at all, else ask */
    if (reason) *reason = "no rule";
    return pe->count == 0 ? CA_POLICY_ALLOW : CA_POLICY_ASK;
}

void ca_policy_set_ask_cb(ca_policy_engine *pe, ca_policy_ask_cb cb, void *ud) {
    pe->ask_cb = cb;
    pe->ask_ud = ud;
}

static int args_contain_dangerous(const char *args_json) {
    static const char *danger[] = {"rm ", "del ", "drop ", "format", "mkfs", "chmod 777",
                                   "shutdown", "reboot", "--force", "rmdir"};
    if (!args_json) return 0;
    for (size_t i = 0; i < sizeof(danger) / sizeof(char *); i++) {
        if (strstr(args_json, danger[i])) return 1;
    }
    return 0;
}

int ca_policy_risk(const char *tool_name, const char *args_json) {
    int base = 0;
    if (!tool_name) return 0;
    if (strcmp(tool_name, "shell") == 0) base = 70;
    else if (strcmp(tool_name, "file_write") == 0) base = 40;
    else if (strcmp(tool_name, "file_delete") == 0) base = 60;
    else if (strcmp(tool_name, "git") == 0) base = 30;
    else if (strcmp(tool_name, "mcp") == 0) base = 35;
    else if (strcmp(tool_name, "file_read") == 0) base = 5;
    else base = 25;

    if (args_contain_dangerous(args_json)) base += 25;
    if (base > 100) base = 100;
    return base;
}
