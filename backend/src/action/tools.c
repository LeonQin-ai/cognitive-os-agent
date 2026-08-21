#include "cagent/action/tools.h"
#include "cagent/runtime/policy_engine.h"
#include "cagent/runtime/event_bus.h"
#include "cagent/infra/metrics.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct ca_tool_registry {
    const ca_tool **tools;
    size_t count, cap;
};

ca_tool_registry *ca_tool_registry_new(void) {
    return calloc(1, sizeof(ca_tool_registry));
}

void ca_tool_registry_free(ca_tool_registry *reg) {
    if (!reg) return;
    free(reg->tools);
    free(reg);
}

int ca_tool_register(ca_tool_registry *reg, const ca_tool *tool) {
    if (!reg || !tool) return -1;
    if (ca_tool_find(reg, tool->name)) return -1; /* already registered */
    if (reg->count == reg->cap) {
        size_t cap = reg->cap ? reg->cap * 2 : 8;
        const ca_tool **nt = realloc(reg->tools, cap * sizeof(ca_tool *));
        if (!nt) return -1;
        reg->tools = nt;
        reg->cap = cap;
    }
    reg->tools[reg->count++] = tool;
    return 0;
}

const ca_tool *ca_tool_find(ca_tool_registry *reg, const char *name) {
    if (!reg) return NULL;
    for (size_t i = 0; i < reg->count; i++)
        if (strcmp(reg->tools[i]->name, name) == 0) return reg->tools[i];
    return NULL;
}

int ca_tool_registry_count(const ca_tool_registry *reg) {
    return reg ? (int)reg->count : 0;
}

const ca_tool *ca_tool_registry_get(const ca_tool_registry *reg, size_t i) {
    return (reg && i < reg->count) ? reg->tools[i] : NULL;
}

void ca_tool_register_builtins(ca_tool_registry *reg) {
    ca_tool_register(reg, ca_tool_file_read());
    ca_tool_register(reg, ca_tool_file_write());
    ca_tool_register(reg, ca_tool_shell());
    ca_tool_register(reg, ca_tool_git());
    ca_tool_register(reg, ca_tool_mcp());
}

ca_tool_result *ca_tool_result_new(int ok, const char *output) {
    ca_tool_result *r = calloc(1, sizeof(ca_tool_result));
    if (!r) return NULL;
    r->ok = ok;
    r->output = ca_strdup(output ? output : "");
    return r;
}

void ca_tool_result_free(ca_tool_result *r) {
    if (!r) return;
    free(r->output);
    free(r);
}

ca_tool_result *ca_tool_execute(ca_tool_registry *reg, const char *name,
                                const char *args_json, const ca_tool_ctx *ctx) {
    const ca_tool *tool = ca_tool_find(reg, name);
    if (!tool) {
        char msg[256];
        snprintf(msg, sizeof(msg), "unknown tool: %s", name);
        return ca_tool_result_new(0, msg);
    }

    /* policy check */
    if (ctx && ctx->policy) {
        const char *reason = NULL;
        ca_policy_decision d = ca_policy_check(ctx->policy, name, args_json, &reason);
        if (d != CA_POLICY_ALLOW) {
            char msg[512];
            snprintf(msg, sizeof(msg), "denied by policy (%s): %s", reason ? reason : "no reason", name);
            if (ctx->metrics) ca_metrics_inc(ctx->metrics, "tools.denied");
            return ca_tool_result_new(0, msg);
        }
    }

    ca_tool_result *r = tool->execute(ctx, args_json);
    if (!r) r = ca_tool_result_new(0, "tool returned NULL");

    if (ctx && ctx->metrics) {
        ca_metrics_inc(ctx->metrics, "tools.executed");
        char mname[128];
        snprintf(mname, sizeof(mname), "tools.%s", name);
        ca_metrics_inc(ctx->metrics, mname);
    }
    if (ctx && ctx->bus) {
        cJSON *ev = cJSON_CreateObject();
        cJSON_AddStringToObject(ev, "tool", name);
        cJSON_AddBoolToObject(ev, "ok", r->ok ? 1 : 0);
        const char *out = r->output ? r->output : "";
        cJSON_AddStringToObject(ev, "output", strlen(out) > 300 ? (out + strlen(out) - 300) : out);
        ca_event_bus_publish(ctx->bus, CA_EV_TOOL, "tools", ev);
    }
    return r;
}
