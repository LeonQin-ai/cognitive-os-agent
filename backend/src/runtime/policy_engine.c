#include "cagent/runtime/policy_engine.h"
#include "cagent/infra/util.h"
#include "cagent/os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "cJSON.h"

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
    /* exact-name rules beat wildcard rules regardless of order; within the
     * same specificity the LAST matching rule wins */
    const rule *exact = NULL, *wild = NULL;
    for (size_t i = 0; i < pe->count; i++) {
        if (strcmp(pe->rules[i].tool, tool_name) == 0) exact = &pe->rules[i];
        else if (strcmp(pe->rules[i].tool, "*") == 0) wild = &pe->rules[i];
    }
    const rule *match = exact ? exact : wild;
    if (match) {
        if (reason) *reason = match->reason ? match->reason : "rule match";
        if (match->decision == 2) {
            if (pe->ask_cb && pe->ask_cb(tool_name, args_json, pe->ask_ud)) return CA_POLICY_ALLOW;
            if (reason) *reason = "ask denied";
            return CA_POLICY_DENY;
        }
        return (ca_policy_decision)match->decision;
    }
    /* default: allow if no rules at all, else ask */
    if (reason) *reason = "no rule";
    return pe->count == 0 ? CA_POLICY_ALLOW : CA_POLICY_ASK;
}

/* ---------- rule management + persistence ---------- */

int ca_policy_rule_count(const ca_policy_engine *pe) { return pe ? (int)pe->count : 0; }

static const char *decision_str(int d) {
    return d == 0 ? "allow" : d == 1 ? "deny" : "ask";
}

int ca_policy_rule_get(const ca_policy_engine *pe, size_t index, const char **tool,
                       const char **action, const char **reason) {
    if (!pe || index >= pe->count) return -1;
    if (tool) *tool = pe->rules[index].tool;
    if (action) *action = decision_str(pe->rules[index].decision);
    if (reason) *reason = pe->rules[index].reason;
    return 0;
}

void ca_policy_remove_rule(ca_policy_engine *pe, size_t index) {
    if (!pe || index >= pe->count) return;
    free(pe->rules[index].tool);
    free(pe->rules[index].reason);
    if (index + 1 < pe->count)
        memmove(&pe->rules[index], &pe->rules[index + 1],
                (pe->count - index - 1) * sizeof(rule));
    pe->count--;
}

int ca_policy_save_file(const ca_policy_engine *pe, const char *path) {
    if (!pe || !path) return -1;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return -1;
    for (size_t i = 0; i < pe->count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "tool", pe->rules[i].tool ? pe->rules[i].tool : "*");
        cJSON_AddStringToObject(o, "action", decision_str(pe->rules[i].decision));
        cJSON_AddStringToObject(o, "reason", pe->rules[i].reason ? pe->rules[i].reason : "");
        cJSON_AddItemToArray(arr, o);
    }
    char *js = cJSON_Print(arr); /* formatted: human-editable */
    cJSON_Delete(arr);
    if (!js) return -1;
    int rc = ca_fs_write_file(path, js, strlen(js)) == 0 ? 0 : -1;
    free(js);
    return rc;
}

int ca_policy_load_file(ca_policy_engine *pe, const char *path) {
    if (!pe || !path) return -1;
    char *txt = ca_fs_read_file(path);
    if (!txt) return -1; /* no file yet: not an error for callers */
    cJSON *arr = cJSON_Parse(txt);
    free(txt);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return -1; }
    int n = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsObject(it)) continue;
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "tool");
        cJSON *a = cJSON_GetObjectItemCaseSensitive(it, "action");
        cJSON *r = cJSON_GetObjectItemCaseSensitive(it, "reason");
        if (!t || !cJSON_IsString(t) || !t->valuestring) continue;
        ca_policy_add_rule(pe, t->valuestring,
                           (a && cJSON_IsString(a)) ? a->valuestring : "deny",
                           (r && cJSON_IsString(r)) ? r->valuestring : NULL);
        n++;
    }
    cJSON_Delete(arr);
    return n;
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
