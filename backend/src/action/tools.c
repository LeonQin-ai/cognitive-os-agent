#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/runtime/policy_engine.h"
#include "cognitive-os-agent/runtime/event_bus.h"
#include "cognitive-os-agent/infra/metrics.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct tool_registry {
    const tool **tools;
    size_t count, cap;
};

tool_registry *tool_registry_new(void) {
    return calloc(1, sizeof(tool_registry));
}

void tool_registry_free(tool_registry *reg) {
    if (!reg)
        return;
    free(reg->tools);
    free(reg);
}

int tool_register(tool_registry *reg, const tool *tool) {
    return tool_register_ex(reg, tool, 0);
}

int tool_register_ex(tool_registry *reg, const tool *t, int replace) {
    int i = -1;

    if (!reg || !t)
        return -1;
    for (size_t k = 0; k < reg->count; k++)
        if (strcmp(reg->tools[k]->name, t->name) == 0) {
            i = (int)k;
            break;
        }

    if (i >= 0) {
        if (!replace)
            return -1; /* already registered */
        reg->tools[i] = t;
        return 0;
    }

    if (reg->count == reg->cap) {
        size_t cap = reg->cap ? reg->cap * 2 : 8;
        const tool **nt = realloc(reg->tools, cap * sizeof(tool *));
        if (!nt)
            return -1;
        reg->tools = nt;
        reg->cap = cap;
    }

    reg->tools[reg->count++] = t;
    return 0;
}

const tool *tool_find(tool_registry *reg, const char *name) {
    if (!reg)
        return NULL;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->tools[i]->name, name) == 0)
            return reg->tools[i];
    }

    return NULL;
}

int tool_unregister(tool_registry *reg, const char *name) {
    if (!reg || !name)
        return 0;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->tools[i]->name, name) == 0) {
            if (i < reg->count - 1)
                memmove(reg->tools + i, reg->tools + i + 1,
                        (reg->count - i - 1) * sizeof(tool *));
            reg->count--;
            return 1;
        }
    }

    return 0;
}

int tool_registry_count(const tool_registry *reg) {
    return reg ? (int)reg->count : 0;
}

const tool *tool_registry_get(const tool_registry *reg, size_t i) {
    return (reg && i < reg->count) ? reg->tools[i] : NULL;
}

void tool_register_builtins(tool_registry *reg) {
    tool_register(reg, tool_file_read());
    tool_register(reg, tool_file_write());
    tool_register(reg, tool_file_edit());
    tool_register(reg, tool_shell());
    tool_register(reg, tool_git());
    tool_register(reg, tool_mcp());
    tool_register(reg, tool_skill());
    tool_register(reg, tool_glob());
    tool_register(reg, tool_grep());
}

tool_result *tool_result_new(int ok, const char *output) {
    tool_result *r = calloc(1, sizeof(tool_result));
    if (!r)
        return NULL;
    r->ok = ok;
    r->output = xstrdup(output ? output : "");
    return r;
}

void tool_result_free(tool_result *r) {
    if (!r)
        return;
    free(r->output);
    free(r);
}

/* --- Lightweight JSON-Schema validation (subset) --- */

static int json_type_matches(const cJSON *v, const char *type) {
    if (!type)
        return 1;
    if (strcmp(type, "string") == 0)
        return cJSON_IsString(v);
    if (strcmp(type, "integer") == 0)
        return cJSON_IsNumber(v) && v->valuedouble == (double)(long long)v->valuedouble;
    if (strcmp(type, "number") == 0)
        return cJSON_IsNumber(v);
    if (strcmp(type, "boolean") == 0)
        return cJSON_IsBool(v);
    if (strcmp(type, "object") == 0)
        return cJSON_IsObject(v);
    if (strcmp(type, "array") == 0)
        return cJSON_IsArray(v);
    if (strcmp(type, "null") == 0)
        return cJSON_IsNull(v);
    return 1; /* unknown type keyword: don't reject */
}

int tool_validate_args(const tool *tool, const char *args_json, char **err_out) {
    cJSON *schema;
    int rc = 0;
    cJSON *stype;
    cJSON *args;

    if (err_out)
        *err_out = NULL;
    if (!tool || !tool->json_schema || !*tool->json_schema)
        return 0;

    schema = cJSON_Parse(tool->json_schema);
    if (!schema)
        return 0; /* malformed schema: skip validation */

    stype = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (stype && cJSON_IsString(stype) && strcmp(stype->valuestring, "object") != 0) {
        /* only object roots are validated in this subset */
        cJSON_Delete(schema);
        return 0;
    }

    args = cJSON_Parse(args_json && *args_json ? args_json : "{}");
    if (!args || !cJSON_IsObject(args)) {
        if (err_out)
            *err_out = xstrdup("args is not a JSON object");
        rc = -1;
    }

    if (rc == 0) {
        cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
        if (cJSON_IsArray(required)) {
            cJSON *it;
            cJSON_ArrayForEach(it, required) {
                if (!cJSON_IsString(it))
                    continue;
                if (!cJSON_GetObjectItemCaseSensitive(args, it->valuestring)) {
                    strbuf b;
                    strbuf_init(&b);
                    strbuf_appendf(&b, "missing required arg '%s'", it->valuestring);
                    if (err_out)
                        *err_out = strbuf_detach(&b);
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
    cJSON *ptype;

                if (!pspec)
                    continue; /* unspecified keys allowed */
                ptype = cJSON_GetObjectItemCaseSensitive(pspec, "type");
                const char *tname = (ptype && cJSON_IsString(ptype)) ? ptype->valuestring : NULL;
                if (!json_type_matches(child, tname)) {
                    strbuf b;
                    strbuf_init(&b);
                    strbuf_appendf(&b, "arg '%s' expected type %s", child->string ? child->string : "?",
                                       tname ? tname : "any");
                    if (err_out)
                        *err_out = strbuf_detach(&b);
                    rc = -1;
                    break;
                }
            }
        }
    }

    if (args)
        cJSON_Delete(args);
    cJSON_Delete(schema);
    return rc;
}

/* Cap tool output so a huge file/shell dump cannot blow up the LLM context
 * (mirrors Claude Code's maxResultSizeChars). */
static char *truncate_output(const char *out, size_t limit) {
    size_t n = strlen(out);
    char *msg;
    char tail[96];

    if (n <= limit)
        return NULL;
    msg = (char *)malloc(limit + 96);
    if (!msg)
        return NULL;
    memcpy(msg, out, limit);
    msg[limit] = '\0';
    snprintf(tail, sizeof(tail), "\n...[truncated, 全长 %zu 字符]", n);
    strcat(msg, tail);
    return msg;
}

tool_result *tool_execute(tool_registry *reg, const char *name, const char *args_json,
                                  const tool_ctx *ctx) {
    const tool *tool = tool_find(reg, name);
    /* lightweight args schema validation (fail fast, before policy/execute) */
    char *verr = NULL;
    tool_result *r;

    if (!tool) {
        char msg[256];
        snprintf(msg, sizeof(msg), "unknown tool: %s", name);
        return tool_result_new(0, msg);
    }

    if (tool_validate_args(tool, args_json, &verr) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "args schema mismatch: %s", verr ? verr : "invalid");
        free(verr);
        return tool_result_new(0, msg);
    }

    /* policy check */
    if (ctx && ctx->policy) {
        const char *reason = NULL;
        policy_decision d = policy_check(ctx->policy, name, args_json, &reason);
        if (d != POLICY_ALLOW) {
            char msg[512];
            snprintf(msg, sizeof(msg), "denied by policy (%s): %s", reason ? reason : "no reason", name);
            if (ctx->metrics)
                metrics_inc(ctx->metrics, "tools.denied");
            return tool_result_new(0, msg);
        }
    }

    r = tool->execute(tool, ctx, args_json);
    if (!r)
        r = tool_result_new(0, "tool returned NULL");

    /* cap output size before it enters the LLM context */
    if (r->output) {
        char *capped = truncate_output(r->output, 8000);
        if (capped) {
            free(r->output);
            r->output = capped;
        }
    }

    if (ctx && ctx->metrics) {
        metrics_inc(ctx->metrics, "tools.executed");
        char mname[128];
        snprintf(mname, sizeof(mname), "tools.%s", name);
        metrics_inc(ctx->metrics, mname);
    }

    if (ctx && ctx->bus) {
        cJSON *ev = cJSON_CreateObject();
        cJSON_AddStringToObject(ev, "tool", name);
        cJSON_AddBoolToObject(ev, "ok", r->ok ? 1 : 0);
        const char *out = r->output ? r->output : "";
        cJSON_AddStringToObject(ev, "output", strlen(out) > 300 ? (out + strlen(out) - 300) : out);
        event_bus_publish(ctx->bus, EV_TOOL, "tools", ev);
    }

    return r;
}
