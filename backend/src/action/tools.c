#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/runtime/policy_engine.h"
#include "cognitive-os-agent/runtime/event_bus.h"
#include "cognitive-os-agent/infra/metrics.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct coa_tool_registry {
    const coa_tool **tools;
    size_t count, cap;
};

coa_tool_registry *coa_tool_registry_new(void) {
    return calloc(1, sizeof(coa_tool_registry));
}

void coa_tool_registry_free(coa_tool_registry *reg) {
    if (!reg) return;
    free(reg->tools);
    free(reg);
}

int coa_tool_register(coa_tool_registry *reg, const coa_tool *tool) {
    return coa_tool_register_ex(reg, tool, 0);
}

int coa_tool_register_ex(coa_tool_registry *reg, const coa_tool *tool, int replace) {
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
        const coa_tool **nt = realloc(reg->tools, cap * sizeof(coa_tool *));
        if (!nt) return -1;
        reg->tools = nt;
        reg->cap = cap;
    }
    reg->tools[reg->count++] = tool;
    return 0;
}

const coa_tool *coa_tool_find(coa_tool_registry *reg, const char *name) {
    if (!reg) return NULL;
    for (size_t i = 0; i < reg->count; i++)
        if (strcmp(reg->tools[i]->name, name) == 0) return reg->tools[i];
    return NULL;
}

int coa_tool_registry_count(const coa_tool_registry *reg) {
    return reg ? (int)reg->count : 0;
}

const coa_tool *coa_tool_registry_get(const coa_tool_registry *reg, size_t i) {
    return (reg && i < reg->count) ? reg->tools[i] : NULL;
}

void coa_tool_register_builtins(coa_tool_registry *reg) {
    coa_tool_register(reg, coa_tool_file_read());
    coa_tool_register(reg, coa_tool_file_write());
    coa_tool_register(reg, coa_tool_file_edit());
    coa_tool_register(reg, coa_tool_shell());
    coa_tool_register(reg, coa_tool_git());
    coa_tool_register(reg, coa_tool_mcp());
    coa_tool_register(reg, coa_tool_skill());
    coa_tool_register(reg, coa_tool_glob());
    coa_tool_register(reg, coa_tool_grep());
}

coa_tool_result *coa_tool_result_new(int ok, const char *output) {
    coa_tool_result *r = calloc(1, sizeof(coa_tool_result));
    if (!r) return NULL;
    r->ok = ok;
    r->output = coa_strdup(output ? output : "");
    return r;
}

void coa_tool_result_free(coa_tool_result *r) {
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

int coa_tool_validate_args(const coa_tool *tool, const char *args_json, char **err_out) {
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
        if (err_out) *err_out = coa_strdup("args is not a JSON object");
        rc = -1;
    }

    if (rc == 0) {
        cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
        if (cJSON_IsArray(required)) {
            cJSON *it;
            cJSON_ArrayForEach(it, required) {
                if (!cJSON_IsString(it)) continue;
                if (!cJSON_GetObjectItemCaseSensitive(args, it->valuestring)) {
                    coa_strbuf b;
                    coa_strbuf_init(&b);
                    coa_strbuf_appendf(&b, "missing required arg '%s'", it->valuestring);
                    if (err_out) *err_out = coa_strbuf_detach(&b);
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
                    coa_strbuf b;
                    coa_strbuf_init(&b);
                    coa_strbuf_appendf(&b, "arg '%s' expected type %s",
                                      child->string ? child->string : "?",
                                      tname ? tname : "any");
                    if (err_out) *err_out = coa_strbuf_detach(&b);
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

coa_tool_result *coa_tool_execute(coa_tool_registry *reg, const char *name,
                                const char *args_json, const coa_tool_ctx *ctx) {
    const coa_tool *tool = coa_tool_find(reg, name);
    if (!tool) {
        char msg[256];
        snprintf(msg, sizeof(msg), "unknown tool: %s", name);
        return coa_tool_result_new(0, msg);
    }

    /* lightweight args schema validation (fail fast, before policy/execute) */
    char *verr = NULL;
    if (coa_tool_validate_args(tool, args_json, &verr) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "args schema mismatch: %s", verr ? verr : "invalid");
        free(verr);
        return coa_tool_result_new(0, msg);
    }

    /* policy check */
    if (ctx && ctx->policy) {
        const char *reason = NULL;
        coa_policy_decision d = coa_policy_check(ctx->policy, name, args_json, &reason);
        if (d != COA_POLICY_ALLOW) {
            char msg[512];
            snprintf(msg, sizeof(msg), "denied by policy (%s): %s", reason ? reason : "no reason", name);
            if (ctx->metrics) coa_metrics_inc(ctx->metrics, "tools.denied");
            return coa_tool_result_new(0, msg);
        }
    }

    coa_tool_result *r = tool->execute(tool, ctx, args_json);
    if (!r) r = coa_tool_result_new(0, "tool returned NULL");

    /* cap output size before it enters the LLM context */
    if (r->output) {
        char *capped = truncate_output(r->output, 8000);
        if (capped) {
            free(r->output);
            r->output = capped;
        }
    }

    if (ctx && ctx->metrics) {
        coa_metrics_inc(ctx->metrics, "tools.executed");
        char mname[128];
        snprintf(mname, sizeof(mname), "tools.%s", name);
        coa_metrics_inc(ctx->metrics, mname);
    }
    if (ctx && ctx->bus) {
        cJSON *ev = cJSON_CreateObject();
        cJSON_AddStringToObject(ev, "tool", name);
        cJSON_AddBoolToObject(ev, "ok", r->ok ? 1 : 0);
        const char *out = r->output ? r->output : "";
        cJSON_AddStringToObject(ev, "output", strlen(out) > 300 ? (out + strlen(out) - 300) : out);
        coa_event_bus_publish(ctx->bus, COA_EV_TOOL, "tools", ev);
    }
    return r;
}
