/* api_rest.c — REST API handlers wired to the c-agent runtime context. */
#include "cagent/api/api_rest.h"
#include "cagent/api/http_server.h"
#include "cagent/api/web_ui.h"
#include "cagent/runtime/scheduler.h"
#include "cagent/action/tools.h"
#include "cagent/memory/memory.h"
#include "cagent/snapshot/snapshot.h"
#include "cagent/infra/metrics.h"
#include "cagent/infra/util.h"
#include "cagent/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

/* Copy the (not null-terminated) request body into a C string. */
static char *body_str(const ca_http_request *req) {
    if (!req->body || req->body_len == 0) return ca_strdup("");
    char *s = malloc(req->body_len + 1);
    if (!s) return NULL;
    memcpy(s, req->body, req->body_len);
    s[req->body_len] = '\0';
    return s;
}

/* Extract an optional string / number field from a JSON object. */
static const char *json_str(cJSON *o, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}
static double json_dbl(cJSON *o, const char *key, double def) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : def;
}

/* If an auth key is configured (ctx->auth != NULL), require a valid bearer
 * token on /v1 routes. Returns 1 if allowed, 0 if denied (sets 401). */
static int authz_ok(cagent_ctx *ctx, const ca_http_request *req, ca_http_response *resp) {
    if (!ctx->auth) return 1; /* auth not configured: open access */
    if (ca_auth_check_header(ctx->auth, req->authorization)) return 1;
    resp->status = 401;
    ca_http_resp_json(resp, "{\"error\":\"unauthorized\"}");
    return 0;
}

static const char *task_status_str(ca_task_status st) {
    switch (st) {
        case CA_TS_QUEUED:    return "QUEUED";
        case CA_TS_RUNNING:   return "RUNNING";
        case CA_TS_DONE:      return "DONE";
        case CA_TS_FAILED:    return "FAILED";
        case CA_TS_CANCELLED: return "CANCELLED";
        case CA_TS_TIMEOUT:   return "TIMEOUT";
        default:              return "UNKNOWN";
    }
}

static int h_task_create(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *prompt = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "prompt");
        if (p && cJSON_IsString(p)) prompt = p->valuestring;
    }
    if (!prompt || !*prompt) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"missing 'prompt' string\"}");
        return 0;
    }
    int64_t id = ca_scheduler_submit(ctx->scheduler, 0, prompt, NULL, 0);
    cJSON_Delete(root);
    if (id < 0) {
        resp->status = 500;
        ca_http_resp_json(resp, "{\"error\":\"scheduler submit failed\"}");
        return 0;
    }
    ca_http_resp_appendf(resp, "{\"id\":%lld,\"status\":\"queued\"}", (long long)id);
    return 0;
}

static int h_task_get(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *suffix = req->path + strlen("/v1/tasks/");
    if (!*suffix) {
        resp->status = 404;
        ca_http_resp_json(resp, "{\"error\":\"missing task id\"}");
        return 0;
    }
    int64_t id = atoll(suffix);
    ca_task *t = ca_scheduler_get(ctx->scheduler, id);
    if (!t) {
        resp->status = 404;
        ca_http_resp_json(resp, "{\"error\":\"task not found\"}");
        return 0;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double)t->id);
    cJSON_AddStringToObject(o, "status", task_status_str(t->status));
    if (t->input) cJSON_AddStringToObject(o, "input", t->input);
    if (t->output) cJSON_AddStringToObject(o, "output", t->output);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) { ca_http_resp_append(resp, s); free(s); }
    return 0;
}

static int h_tools(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    cJSON *arr = cJSON_CreateArray();
    int n = ca_tool_registry_count(ctx->tools);
    for (int i = 0; i < n; i++) {
        const ca_tool *t = ca_tool_registry_get(ctx->tools, (size_t)i);
        if (!t) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", t->name);
        cJSON_AddStringToObject(o, "description", t->description ? t->description : "");
        cJSON_AddBoolToObject(o, "write", t->is_write ? 1 : 0);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_memory(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    cJSON *root = cJSON_CreateObject();
    if (ctx->memory) {
        char *w = ca_memory_working_json(ctx->memory);
        char *l = ca_memory_longterm_json(ctx->memory);
        cJSON *wj = w ? cJSON_Parse(w) : NULL;
        cJSON *lj = l ? cJSON_Parse(l) : NULL;
        cJSON_AddItemToObject(root, "working", wj ? wj : cJSON_CreateArray());
        cJSON_AddItemToObject(root, "longterm", lj ? lj : cJSON_CreateObject());
        free(w);
        free(l);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ca_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_snapshots(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->snapshot ? ca_snapshot_list(ctx->snapshot) : ca_strdup("[]");
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_snapshot_rollback(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    int rc = ctx->snapshot ? ca_snapshot_restore_latest(ctx->snapshot) : -1;
    ca_http_resp_appendf(resp, "{\"ok\":%s}", rc == 0 ? "true" : "false");
    return 0;
}

static int h_blackboard(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->blackboard ? ca_blackboard_snapshot_json(ctx->blackboard) : ca_strdup("{}");
    ca_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_blackboard_put(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *key = NULL, *val = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "key");
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (k && cJSON_IsString(k)) key = k->valuestring;
        if (v && cJSON_IsString(v)) val = v->valuestring;
    }
    if (!key || !*key || !val) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"need 'key' and 'value' strings\"}");
        return 0;
    }
    if (ctx->blackboard) ca_blackboard_put(ctx->blackboard, key, val);
    cJSON_Delete(root);
    ca_http_resp_appendf(resp, "{\"ok\":true}");
    return 0;
}

static int h_agents(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->agents ? ca_agent_pool_snapshot_json(ctx->agents) : ca_strdup("{}");
    ca_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_agent_add(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *name = NULL, *role = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "role");
        if (n && cJSON_IsString(n)) name = n->valuestring;
        if (r && cJSON_IsString(r)) role = r->valuestring;
    }
    if (root) cJSON_Delete(root);
    if (!name || !*name) {
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"need 'name' string\"}");
        return 0;
    }
    int idx = ctx->agents ? ca_agent_pool_add(ctx->agents, name, role ? role : "") : -1;
    if (idx < 0) { resp->status = 400; ca_http_resp_json(resp, "{\"error\":\"add agent failed (duplicate?)\"}"); return 0; }
    ca_http_resp_appendf(resp, "{\"ok\":true,\"index\":%d}", idx);
    return 0;
}

static int h_agent_post(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *rest = req->path + strlen("/v1/agents/");
    char name[128];
    snprintf(name, sizeof(name), "%s", rest);
    char *slash = strstr(name, "/post");
    if (slash) *slash = '\0';
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *key = NULL, *val = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "key");
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (k && cJSON_IsString(k)) key = k->valuestring;
        if (v && cJSON_IsString(v)) val = v->valuestring;
    }
    if (root) cJSON_Delete(root);
    if (!key || !*key || !val) {
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"need 'key' and 'value' strings\"}");
        return 0;
    }
    int rc = ctx->agents ? ca_agent_post(ctx->agents, name, key, val) : -1;
    ca_http_resp_appendf(resp, "{\"ok\":%s}", rc == 0 ? "true" : "false");
    return 0;
}

static int h_metrics(const ca_http_request *req, ca_http_response *resp, void *ud) {
    (void)req;
    cagent_ctx *ctx = (cagent_ctx *)ud;
    snprintf(resp->content_type, sizeof(resp->content_type), "text/plain; version=0.0.4");
    char *s = ctx->metrics ? ca_metrics_render(ctx->metrics) : ca_strdup("");
    ca_http_resp_append(resp, s ? s : "");
    free(s);
    return 0;
}

static int h_index(const ca_http_request *req, ca_http_response *resp, void *ud) {
    (void)req;
    (void)ud;
    snprintf(resp->content_type, sizeof(resp->content_type), "text/html; charset=utf-8");
    ca_http_resp_append(resp, ca_web_index_html);
    return 0;
}

static int h_favicon(const ca_http_request *req, ca_http_response *resp, void *ud) {
    (void)req;
    (void)ud;
    resp->status = 204;
    return 0;
}

static int h_trace(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->trace ? ca_trace_json(ctx->trace) : ca_strdup("[]");
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_routes(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->router ? ca_router_json(ctx->router) : ca_strdup("[]");
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_route_add(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"body must be a JSON object\"}");
        return 0;
    }
    const char *name = json_str(root, "name");
    const char *provider = json_str(root, "provider");
    if (!name || !*name || !provider || !*provider) {
        cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"need 'name' and 'provider' strings\"}");
        return 0;
    }
    if (ctx->router)
        ca_router_add(ctx->router, name, provider, json_str(root, "base_url"),
                      json_str(root, "api_key"), json_str(root, "model"),
                      json_dbl(root, "weight", 1.0));
    cJSON_Delete(root);
    char *s = ctx->router ? ca_router_json(ctx->router) : ca_strdup("[]");
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_route_delete(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *name = req->path + strlen("/v1/routes/");
    int removed = ctx->router ? ca_router_remove(ctx->router, name) : 0;
    char *s = ctx->router ? ca_router_json(ctx->router) : ca_strdup("[]");
    ca_http_resp_appendf(resp, "{\"removed\":%s,\"routes\":", removed ? "true" : "false");
    ca_http_resp_append(resp, s ? s : "[]");
    ca_http_resp_append(resp, "}");
    free(s);
    return 0;
}

static int h_config_llm_get(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    (void)req;
    /* active LLM as persisted in config (set by cagent_set_llm / env / defaults) */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "provider",
        ca_config_get_str(ctx->config, "llm.provider", "mock"));
    cJSON_AddStringToObject(o, "model",
        ca_config_get_str(ctx->config, "llm.model", ""));
    cJSON_AddStringToObject(o, "base_url",
        ca_config_get_str(ctx->config, "llm.base_url", ""));
    const char *ak = ca_config_get_str(ctx->config, "llm.api_key", NULL);
    cJSON_AddBoolToObject(o, "api_key_set", ak && *ak);
    char *s = cJSON_PrintUnformatted(o);
    ca_http_resp_json(resp, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
    return 0;
}

static int h_config_llm(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"body must be a JSON object\"}");
        return 0;
    }
    const char *provider = json_str(root, "provider");
    if (!provider || !*provider) {
        cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"need 'provider' string\"}");
        return 0;
    }
    const char *base_url = json_str(root, "base_url");
    const char *model = json_str(root, "model");
    const char *api_key = json_str(root, "api_key");
    /* cagent_set_llm dups every value; keep the JSON alive until after the call
     * so the pointers above stay valid (they live inside `root`) */
    int rc = cagent_set_llm(ctx, provider, base_url, model, api_key);
    cJSON_Delete(root);
    if (rc != 0) {
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"unknown provider (mock|openai|anthropic)\"}");
        return 0;
    }
    char *s = ctx->router ? ca_router_json(ctx->router) : ca_strdup("[]");
    ca_http_resp_appendf(resp, "{\"ok\":true,\"routes\":");
    ca_http_resp_append(resp, s ? s : "[]");
    ca_http_resp_append(resp, "}");
    free(s);
    return 0;
}

static int h_usage(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->usage ? ca_usage_json(ctx->usage) : ca_strdup("{}");
    ca_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_plugins(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->registry ? ca_plugin_registry_json(ctx->registry) : ca_strdup("{}");
    ca_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_plugin_generate(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *description = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "description");
        if (d && cJSON_IsString(d)) description = d->valuestring;
    }
    if (root) cJSON_Delete(root);
    if (!description || !*description) {
        resp->status = 400;
        ca_http_resp_json(resp, "{\"ok\":false,\"error\":\"need 'description' string\"}");
        return 0;
    }
    char *s = ca_plugin_generate(ctx, description);
    ca_http_resp_json(resp, s ? s : "{\"ok\":false,\"error\":\"pipeline failed\"}");
    free(s);
    return 0;
}

static int h_skills(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->skills ? ca_skill_list_json(ctx->skills) : ca_strdup("[]");
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_skill_run(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *name = NULL, *args = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "args");
        if (n && cJSON_IsString(n)) name = n->valuestring;
        if (a && cJSON_IsString(a)) args = a->valuestring;
    }
    if (!name || !*name) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"missing 'name' string\"}");
        return 0;
    }
    ca_skill_result *r = ctx->skills
        ? ca_skill_execute(ctx->skills, name, args, ctx->workspace, 10000) : NULL;
    cJSON_Delete(root);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", r ? (r->ok ? 1 : 0) : 0);
    cJSON_AddStringToObject(o, "output", (r && r->output) ? r->output : "");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (r) ca_skill_result_free(r);
    ca_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_mcp(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->mcp ? ca_mcp_manager_json(ctx->mcp) : ca_strdup("[]");
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_mcp_add(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *name = NULL, *url = NULL, *token = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "url");
        cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "token");
        if (n && cJSON_IsString(n)) name = n->valuestring;
        if (u && cJSON_IsString(u)) url = u->valuestring;
        if (t && cJSON_IsString(t)) token = t->valuestring;
    }
    if (root) cJSON_Delete(root);
    if (!name || !*name || !url || !*url) {
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"need 'name' and 'url' strings\"}");
        return 0;
    }
    int rc = ctx->mcp ? ca_mcp_manager_add(ctx->mcp, name, url, token) : -1;
    if (rc != 0) { resp->status = 400; ca_http_resp_json(resp, "{\"error\":\"add failed\"}"); return 0; }
    char *s = ctx->mcp ? ca_mcp_manager_json(ctx->mcp) : ca_strdup("[]");
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_mcp_delete(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *name = req->path + strlen("/v1/mcp/");
    int rc = ctx->mcp ? ca_mcp_manager_remove(ctx->mcp, name) : -1;
    ca_http_resp_appendf(resp, "{\"removed\":%s}", rc == 0 ? "true" : "false");
    return 0;
}

static int h_cluster(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->cluster ? ca_cluster_json(ctx->cluster) : ca_strdup("[]");
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

/* ================= IM (instant messaging) ================= */

/* Push a new IM message to every WebSocket client + record an experience. */
static void im_push(cagent_ctx *ctx, int64_t session_id, int64_t msg_id,
                    const char *role, const char *sender, const char *content) {
    if (!ctx || !content) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "im.message");
    cJSON_AddNumberToObject(o, "session_id", (double)session_id);
    cJSON_AddNumberToObject(o, "id", (double)msg_id);
    cJSON_AddStringToObject(o, "role", role ? role : "user");
    if (sender && *sender) cJSON_AddStringToObject(o, "sender", sender);
    cJSON_AddStringToObject(o, "content", content);
    cJSON_AddNumberToObject(o, "ts_ms", (double)ca_time_now_ms());
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (js) {
        if (ctx->http) ca_http_server_ws_broadcast(ctx->http, js);
        free(js);
    }
    if (ctx->memory) {
        char buf[384];
        snprintf(buf, sizeof(buf), "im session %lld (%s%s%s)",
                 (long long)session_id, role ? role : "user",
                 (sender && *sender) ? " by " : "", (sender && *sender) ? sender : "");
        ca_memory_record_experience(ctx->memory, buf, content);
    }
}

/* Inbound WebSocket text protocol:
 *   {"type":"im.send","session_id":N,"content":"...","sender":"..."} -> IM message
 *   {"type":"im.ping"}                                  -> server replies pong
 */
static void on_ws_msg(const char *text, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!ctx || !text) return;
    cJSON *root = cJSON_Parse(text);
    if (!root) return;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *t = type && cJSON_IsString(type) ? type->valuestring : "";
    if (strcmp(t, "im.send") == 0) {
        cJSON *sid = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
        cJSON *sender = cJSON_GetObjectItemCaseSensitive(root, "sender");
        if (sid && cJSON_IsNumber(sid) && content && cJSON_IsString(content) && ctx->im) {
            const char *snd = (sender && cJSON_IsString(sender)) ? sender->valuestring : NULL;
            int64_t id = ca_im_send_ex(ctx->im, (int64_t)sid->valuedouble, "user",
                                       content->valuestring, snd);
            if (id > 0) im_push(ctx, (int64_t)sid->valuedouble, id, "user", snd, content->valuestring);
        }
    } else if (strcmp(t, "im.ping") == 0) {
        if (ctx->http) ca_http_server_ws_broadcast(ctx->http, "{\"type\":\"pong\"}");
    }
    cJSON_Delete(root);
}

static int h_im_sessions(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->im ? ca_im_sessions_json(ctx->im) : ca_strdup("{}");
    ca_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_im_session_create(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *name = NULL, *kind = NULL;
    const char *members[64];
    size_t n_members = 0;
    if (root && cJSON_IsObject(root)) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "kind");
        cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "members");
        if (n && cJSON_IsString(n)) name = n->valuestring;
        if (k && cJSON_IsString(k)) kind = k->valuestring;
        if (m && cJSON_IsArray(m)) {
            int mn = cJSON_GetArraySize(m);
            for (int i = 0; i < mn && n_members < 64; i++) {
                cJSON *mv = cJSON_GetArrayItem(m, i);
                if (mv && cJSON_IsString(mv)) members[n_members++] = mv->valuestring;
            }
        }
    }
    if (!ctx->im) { if (root) cJSON_Delete(root); resp->status = 500; ca_http_resp_json(resp, "{\"error\":\"im disabled\"}"); return 0; }
    int64_t id = ca_im_create_session_ex(ctx->im, name ? name : "",
                                         kind ? kind : "direct", members, n_members);
    if (root) cJSON_Delete(root);
    if (id < 0) { resp->status = 500; ca_http_resp_json(resp, "{\"error\":\"create failed\"}"); return 0; }
    ca_http_resp_appendf(resp, "{\"id\":%lld}", (long long)id);
    return 0;
}

/* Handles GET/POST/DELETE on /v1/im/sessions/{id} and /v1/im/sessions/{id}/messages */
static int h_im_session_route(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->im) { resp->status = 500; ca_http_resp_json(resp, "{\"error\":\"im disabled\"}"); return 0; }
    const char *rest = req->path + strlen("/v1/im/sessions/");
    int is_messages = 0;
    size_t rest_len = strlen(rest);
    if (rest_len > strlen("/messages") &&
        strcmp(rest + rest_len - strlen("/messages"), "/messages") == 0) {
        is_messages = 1;
        rest_len -= strlen("/messages");
    }
    char idbuf[64];
    if (rest_len >= sizeof(idbuf)) rest_len = sizeof(idbuf) - 1;
    memcpy(idbuf, rest, rest_len);
    idbuf[rest_len] = '\0';
    if (!idbuf[0]) { resp->status = 404; ca_http_resp_json(resp, "{\"error\":\"missing session id\"}"); return 0; }
    int64_t session_id = atoll(idbuf);

    if (strcmp(req->method, "DELETE") == 0) {
        int ok = ca_im_delete_session(ctx->im, session_id);
        ca_http_resp_appendf(resp, "{\"deleted\":%s}", ok ? "true" : "false");
        return 0;
    }
    if (strcmp(req->method, "POST") == 0) {
        if (!is_messages) { resp->status = 404; ca_http_resp_json(resp, "{\"error\":\"POST expects .../messages\"}"); return 0; }
        char *b = body_str(req);
        cJSON *root = b ? cJSON_Parse(b) : NULL;
        free(b);
        const char *role = NULL, *content = NULL, *sender = NULL;
        if (root && cJSON_IsObject(root)) {
            cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "role");
            cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "content");
            cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "sender");
            if (r && cJSON_IsString(r)) role = r->valuestring;
            if (c && cJSON_IsString(c)) content = c->valuestring;
            if (s && cJSON_IsString(s)) sender = s->valuestring;
        }
        if (root) cJSON_Delete(root);
        if (!content || !*content) { resp->status = 400; ca_http_resp_json(resp, "{\"error\":\"need 'content' string\"}"); return 0; }
        if (!role || !*role) role = "user";
        int64_t id = ca_im_send_ex(ctx->im, session_id, role, content, sender);
        if (id < 0) { resp->status = 404; ca_http_resp_json(resp, "{\"error\":\"session not found\"}"); return 0; }
        im_push(ctx, session_id, id, role, sender, content);
        ca_http_resp_appendf(resp, "{\"id\":%lld,\"ok\":true}", (long long)id);
        return 0;
    }
    /* GET */
    size_t n = 0;
    ca_im_message *msgs = ca_im_messages(ctx->im, session_id, &n);
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", (double)msgs[i].id);
        cJSON_AddStringToObject(o, "role", msgs[i].role);
        if (msgs[i].sender) cJSON_AddStringToObject(o, "sender", msgs[i].sender);
        cJSON_AddStringToObject(o, "content", msgs[i].content);
        cJSON_AddNumberToObject(o, "ts_ms", (double)msgs[i].ts_ms);
        cJSON_AddItemToArray(arr, o);
    }
    ca_im_messages_free(msgs, n);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

/* GET /v1/im/search?q=term  — history search across all sessions */
static int h_im_search(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->im) { resp->status = 500; ca_http_resp_json(resp, "{\"error\":\"im disabled\"}"); return 0; }
    const char *q = strstr(req->query, "q=");
    const char *query = q ? q + 2 : "";
    char *s = ca_im_search(ctx->im, query, 200);
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

int cagent_api_attach(cagent_ctx *ctx) {
    if (!ctx || ctx->http_port == 0) return 0;
    if (!ctx->http) {
        ctx->http = ca_http_server_new_bind(ctx->http_bind, ctx->http_port);
        if (!ctx->http) return -1;
    }
    ca_http_server_route(ctx->http, "POST", "/v1/tasks", h_task_create, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/tasks/", h_task_get, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/tools", h_tools, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/memory", h_memory, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/blackboard", h_blackboard, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/blackboard", h_blackboard_put, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/agents", h_agents, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/agents", h_agent_add, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/agents/", h_agent_post, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/snapshots", h_snapshots, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/snapshots/rollback", h_snapshot_rollback, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/trace", h_trace, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/routes", h_routes, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/routes", h_route_add, ctx);
    ca_http_server_route(ctx->http, "DELETE", "/v1/routes/", h_route_delete, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/config/llm", h_config_llm_get, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/config/llm", h_config_llm, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/usage", h_usage, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/plugins", h_plugins, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/plugins/generate", h_plugin_generate, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/skills", h_skills, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/skills/run", h_skill_run, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/mcp", h_mcp, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/mcp", h_mcp_add, ctx);
    ca_http_server_route(ctx->http, "DELETE", "/v1/mcp/", h_mcp_delete, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/cluster", h_cluster, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/im/sessions", h_im_sessions, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/im/sessions", h_im_session_create, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/im/search", h_im_search, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/im/sessions/", h_im_session_route, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/im/sessions/", h_im_session_route, ctx);
    ca_http_server_route(ctx->http, "DELETE", "/v1/im/sessions/", h_im_session_route, ctx);
    ca_http_server_ws_route(ctx->http, "/ws", on_ws_msg, ctx);
    ca_http_server_route(ctx->http, "GET", "/metrics", h_metrics, ctx);
    ca_http_server_route(ctx->http, "GET", "/", h_index, ctx);
    ca_http_server_route(ctx->http, "GET", "/favicon.ico", h_favicon, ctx);
    return 0;
}
