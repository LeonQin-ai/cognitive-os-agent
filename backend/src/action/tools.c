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
    return ca_tool_register_ex(reg, tool, 0);
}

int ca_tool_register_ex(ca_tool_registry *reg, const ca_tool *tool, int replace) {
    if (!reg || !tool) return -1;
    int i = -1;
    for (size_t k = 0; k < reg->count; k++)
        if (strcmp(reg->tools[k]->name, tool->name) == 0) { i = (int)k; break; }
    if (i >= 0) {
        if (!replace) return -1; /* already registered */
        reg->tools[i] = tool;
        return 0;
    }
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
    ca_tool_register(reg, ca_tool_file_edit());
    ca_tool_register(reg, ca_tool_shell());
    ca_tool_register(reg, ca_tool_git());
    ca_tool_register(reg, ca_tool_mcp());
    ca_tool_register(reg, ca_tool_skill());
    ca_tool_register(reg, ca_tool_glob());
    ca_tool_register(reg, ca_tool_grep());
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

/* --- Lightweight JSON-Schema validation (subset) --- */

static int json_type_matches(const cJSON *v, const char *type) {
    if (!type) return 1;
    if (strcmp(type, "string") == 0) return cJSON_IsString(v);
    if (strcmp(type, "integer") == 0) return cJSON_IsNumber(v) && v->valuedouble == (double)(long long)v->valuedouble;
    if (strcmp(type, "number") == 0) return cJSON_IsNumber(v);
    if (strcmp(type, "boolean") == 0) return cJSON_IsBool(v);
    if (strcmp(type, "object") == 0) return cJSON_IsObject(v);
    if (strcmp(type, "array") == 0) return cJSON_IsArray(v);
    if (strcmp(type, "null") == 0) return cJSON_IsNull(v);
    return 1; /* unknown type keyword: don't reject */
}

int ca_tool_validate_args(const ca_tool *tool, const char *args_json, char **err_out) {
    if (err_out) *err_out = NULL;
    if (!tool || !tool->json_schema || !*tool->json_schema) return 0;

    cJSON *schema = cJSON_Parse(tool->json_schema);
    if (!schema) return 0; /* malformed schema: skip validation */
    int rc = 0;

    cJSON *stype = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (stype && cJSON_IsString(stype) && strcmp(stype->valuestring, "object") != 0) {
        /* only object roots are validated in this subset */
        cJSON_Delete(schema);
        return 0;
    }

    cJSON *args = cJSON_Parse(args_json && *args_json ? args_json : "{}");
    if (!args || !cJSON_IsObject(args)) {
        if (err_out) *err_out = ca_strdup("args is not a JSON object");
        rc = -1;
    }

    if (rc == 0) {
        cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
        if (cJSON_IsArray(required)) {
            cJSON *it;
            cJSON_ArrayForEach(it, required) {
                if (!cJSON_IsString(it)) continue;
                if (!cJSON_GetObjectItemCaseSensitive(args, it->valuestring)) {
                    ca_strbuf b;
                    ca_strbuf_init(&b);
                    ca_strbuf_appendf(&b, "missing required arg '%s'", it->valuestring);
                    if (err_out) *err_out = ca_strbuf_detach(&b);
                    rc = -1;
                    break;
                }
            }
        }
    }
    if (rc == 0) {
        cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
        if (cJSON_IsObject(props)) {
            const cJSON *child = NULL;
            cJSON_ArrayForEach(child, args) {
                cJSON *pspec = cJSON_GetObjectItemCaseSensitive(props, child->string);
                if (!pspec) continue; /* unspecified keys allowed */
                cJSON *ptype = cJSON_GetObjectItemCaseSensitive(pspec, "type");
                const char *tname = (ptype && cJSON_IsString(ptype)) ? ptype->valuestring : NULL;
                if (!json_type_matches(child, tname)) {
                    ca_strbuf b;
                    ca_strbuf_init(&b);
                    ca_strbuf_appendf(&b, "arg '%s' expected type %s",
                                      child->string ? child->string : "?",
                                      tname ? tname : "any");
                    if (err_out) *err_out = ca_strbuf_detach(&b);
                    rc = -1;
                    break;
                }
            }
        }
    }
    if (args) cJSON_Delete(args);
    cJSON_Delete(schema);
    return rc;
}

/* Cap tool output so a huge file/shell dump cannot blow up the LLM context
 * (mirrors Claude Code's maxResultSizeChars). */
static char *truncate_output(const char *out, size_t limit) {
    size_t n = strlen(out);
    if (n <= limit) return NULL;
    char *msg = (char *)malloc(limit + 96);
    if (!msg) return NULL;
    memcpy(msg, out, limit);
    msg[limit] = '\0';
    char tail[96];
    snprintf(tail, sizeof(tail), "\n...[truncated, 全长 %zu 字符]", n);
    strcat(msg, tail);
    return msg;
}

ca_tool_result *ca_tool_execute(ca_tool_registry *reg, const char *name,
                                const char *args_json, const ca_tool_ctx *ctx) {
    const ca_tool *tool = ca_tool_find(reg, name);
    if (!tool) {
        char msg[256];
        snprintf(msg, sizeof(msg), "unknown tool: %s", name);
        return ca_tool_result_new(0, msg);
    }

    /* lightweight args schema validation (fail fast, before policy/execute) */
    char *verr = NULL;
    if (ca_tool_validate_args(tool, args_json, &verr) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "args schema mismatch: %s", verr ? verr : "invalid");
        free(verr);
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

    ca_tool_result *r = tool->execute(tool, ctx, args_json);
    if (!r) r = ca_tool_result_new(0, "tool returned NULL");

    /* cap output size before it enters the LLM context */
    if (r->output) {
        char *capped = truncate_output(r->output, 8000);
        if (capped) {
            free(r->output);
            r->output = capped;
        }
    }

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
