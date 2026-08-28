/* api_rest.c — REST API handlers wired to the c-agent runtime context. */
#include "cagent/api/api_rest.h"
#include "cagent/api/http_server.h"
#include "cagent/api/web_ui.h"
#include "cagent/api/market.h"
#include "cagent/runtime/scheduler.h"
#include "cagent/action/tools.h"
#include "cagent/memory/memory.h"
#include "cagent/snapshot/snapshot.h"
#include "cagent/infra/metrics.h"
#include "cagent/infra/util.h"
#include "cagent/infra/catalog.h"
#include "cagent/llm/llm.h"
#include "cagent/os/os_time.h"
#include "cagent/os/os_proc.h"
#include "cagent/os/os_fs.h"
#include "cagent/os/os_socket.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
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
    const char *name = NULL, *role = NULL, *provider = NULL, *model = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "role");
        cJSON *prov = cJSON_GetObjectItemCaseSensitive(root, "provider");
        cJSON *mod = cJSON_GetObjectItemCaseSensitive(root, "model");
        if (n && cJSON_IsString(n)) name = n->valuestring;
        if (r && cJSON_IsString(r)) role = r->valuestring;
        provider = (prov && cJSON_IsString(prov)) ? prov->valuestring : NULL;
        model = (mod && cJSON_IsString(mod)) ? mod->valuestring : NULL;
        if (provider && !*provider) provider = NULL;
        if (model && !*model) model = NULL;
    }
    if (root) cJSON_Delete(root);
    if (!name || !*name) {
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"need 'name' string\"}");
        return 0;
    }
    int idx = ctx->agents ? ca_agent_pool_add_model(ctx->agents, name, role ? role : "",
                                                    provider, model) : -1;
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
    /* persist so configured routes survive restart */
    if (ctx->router && ctx->state_root) {
        char rpath[600];
        ca_path_join(rpath, sizeof(rpath), ctx->state_root, "routes.json");
        ca_router_save_file(ctx->router, rpath);
    }
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
    /* persist so configured routes survive restart */
    if (ctx->router && ctx->state_root) {
        char rpath[600];
        ca_path_join(rpath, sizeof(rpath), ctx->state_root, "routes.json");
        ca_router_save_file(ctx->router, rpath);
    }
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

/* Validate that a given provider/base_url/model/api_key actually answers a chat
 * request. Creates a throwaway LLM instance (never persisted, never made active)
 * and returns {ok, reply|error}. Lets the UI prove a config works before saving. */
static int h_config_llm_test(const ca_http_request *req, ca_http_response *resp, void *ud) {
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
    ca_llm *nl = ca_llm_create(provider, json_str(root, "base_url"),
                               json_str(root, "api_key"), json_str(root, "model"));
    if (!nl) {
        cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"ok\":false,\"error\":\"unknown provider (mock|openai|anthropic)\"}");
        return 0;
    }
    ca_llm_message msgs[2] = {
        {"system", "You are a concise assistant. Reply in at most a few words."},
        {"user",   "Reply with exactly the word: ok"}
    };
    ca_llm_request lreq = {0};
    lreq.messages = msgs;
    lreq.num_messages = 2;
    lreq.temperature = 0.2;
    lreq.max_tokens = 64;
    ca_llm_response lr = {0};
    int rc = ca_llm_chat(nl, &lreq, &lr);
    cJSON *o = cJSON_CreateObject();
    int ok = (rc == 0 && lr.content && *lr.content);
    cJSON_AddBoolToObject(o, "ok", ok);
    if (lr.content) cJSON_AddStringToObject(o, "reply", lr.content);
    else if (lr.error) cJSON_AddStringToObject(o, "error", lr.error);
    else cJSON_AddStringToObject(o, "error", "no response from provider (check base_url / model / api_key / network)");
    char *s = cJSON_PrintUnformatted(o);
    ca_http_resp_json(resp, s ? s : "{\"ok\":false}");
    free(s);
    cJSON_Delete(o);
    free(lr.content);
    free(lr.error);
    ca_llm_destroy(nl);
    cJSON_Delete(root);
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

/* FNV-1a 64-bit digest rendered as 16 hex chars (plugin artifact signature). */
static void fnv1a_hex(const char *s, char out[17]) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        h ^= *p; h *= 1099511628211ULL;
    }
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

/* ---- Skills marketplace ---- */

typedef struct { const char *name, *description, *kind, *body; } skill_tmpl;
static const skill_tmpl SKILL_TMPLS[] = {
    {"hello_world", "打印问候语",                    "shell",   "echo hello from c-agent"},
    {"list_dir",    "列出当前目录文件",              "shell",   "ls -la"},
    {"sys_info",    "显示内核/系统信息",             "shell",   "uname -a"},
    {"disk_usage",  "显示磁盘占用",                  "shell",   "df -h"},
    {"py_now",      "Python 打印当前时间",           "python",  "import datetime; print(datetime.datetime.now())"},
};
#define N_SKILL_TMPLS (int)(sizeof(SKILL_TMPLS)/sizeof(SKILL_TMPLS[0]))

/* Append items from a remote market JSON array into `dest`, tagging each as
 * source=remote so the UI can show where an entry came from. */
static void market_merge_remote(cJSON *dest, cJSON *remote) {
    if (!dest || !remote) return;
    cJSON *it;
    cJSON_ArrayForEach(it, remote) {
        if (!cJSON_IsObject(it)) continue;
        cJSON *o = cJSON_Duplicate(it, 1);
        if (!o) continue;
        cJSON *src = cJSON_GetObjectItemCaseSensitive(o, "source");
        if (src) cJSON_DeleteItemFromObject(o, "source");
        cJSON_AddStringToObject(o, "source", "remote");
        cJSON_AddItemToArray(dest, o);
    }
}

/* Fetch a remote market catalog for a path and return its parsed JSON root
 * (or NULL when no market is configured or it is unreachable). */
static cJSON *market_fetch_root(cagent_ctx *ctx, const char *path) {
    if (!ctx->market_url || !*ctx->market_url) return NULL;
    char *body = ca_market_fetch(ctx->market_url, path, 4000);
    if (!body) return NULL;
    cJSON *root = cJSON_Parse(body);
    free(body);
    return root;
}

/* Merge a remote market's array field into the local one. Returns 1 if online. */
static int market_merge_field(cJSON *local, cJSON *remote_root, const char *field) {
    cJSON *arr = remote_root ? cJSON_GetObjectItemCaseSensitive(remote_root, field) : NULL;
    if (!arr || !cJSON_IsArray(arr)) return 0;
    market_merge_remote(local, arr);
    return 1;
}

static int h_skills_market(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    cJSON *root = cJSON_CreateObject();
    cJSON *tmpl = cJSON_CreateArray();
    for (int i = 0; i < N_SKILL_TMPLS; i++) {
        const skill_tmpl *t = &SKILL_TMPLS[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", t->name);
        cJSON_AddStringToObject(o, "description", t->description);
        cJSON_AddStringToObject(o, "kind", t->kind);
        cJSON_AddStringToObject(o, "body", t->body);
        cJSON_AddBoolToObject(o, "installed",
            ctx->skills && ca_skill_find(ctx->skills, t->name) != NULL);
        cJSON_AddItemToArray(tmpl, o);
    }
    cJSON_AddItemToObject(root, "templates", tmpl);
    /* GitHub 热门应用（可安装为 skill 的开源工具/仓库，附仓库链接） */
    cJSON *gh = cJSON_CreateArray();
    static const struct { const char *name, *desc, *repo, *kind; } GH_SKILLS[] = {
        { "jq", "命令行 JSON 处理工具（解析/转换 JSON）", "https://github.com/jqlang/jq", "shell" },
        { "ripgrep", "极速递归正则搜索（rg）", "https://github.com/BurntSushi/ripgrep", "shell" },
        { "yt-dlp", "视频/音频下载器（支持大量站点）", "https://github.com/yt-dlp/yt-dlp", "shell" },
        { "pandoc", "万能文档格式转换（markdown/HTML/PDF…）", "https://github.com/jgm/pandoc", "shell" },
        { "ffmpeg", "音视频处理工具箱", "https://github.com/FFmpeg/FFmpeg", "shell" },
        { "gh", "GitHub 官方命令行（Issue/PR/Release）", "https://github.com/cli/cli", "shell" },
        { "fd", "更友好的 find 替代", "https://github.com/sharkdp/fd", "shell" },
        { "bat", "带语法高亮的 cat 替代", "https://github.com/sharkdp/bat", "shell" },
    };
    for (size_t i = 0; i < sizeof(GH_SKILLS) / sizeof(GH_SKILLS[0]); i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", GH_SKILLS[i].name);
        cJSON_AddStringToObject(o, "description", GH_SKILLS[i].desc);
        cJSON_AddStringToObject(o, "repo", GH_SKILLS[i].repo);
        cJSON_AddStringToObject(o, "kind", GH_SKILLS[i].kind);
        cJSON_AddItemToArray(gh, o);
    }
    cJSON_AddItemToObject(root, "github", gh);

    /* networked marketplace: merge remote templates + github apps when reachable */
    int market_online = 0;
    if (ctx->market_url && *ctx->market_url) {
        cJSON *remote = market_fetch_root(ctx, "/v1/skills/market");
        if (remote) {
            market_online = market_merge_field(tmpl, remote, "templates");
            market_merge_field(gh, remote, "github");
            cJSON_Delete(remote);
        }
    }
    cJSON_AddBoolToObject(root, "market_online", market_online);
    if (ctx->market_url && *ctx->market_url)
        cJSON_AddStringToObject(root, "market_url", ctx->market_url);

    char *mine = ctx->skills ? ca_skill_list_json(ctx->skills) : NULL;
    cJSON *inst = mine ? cJSON_Parse(mine) : NULL;
    free(mine);
    cJSON_AddItemToObject(root, "installed", inst ? inst : cJSON_CreateArray());
    char *s = cJSON_PrintUnformatted(root);
    ca_http_resp_json(resp, s ? s : "{}");
    free(s);
    cJSON_Delete(root);
    return 0;
}

static int h_skills_publish(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *name = json_str(root, "name");
    const char *kind = json_str(root, "kind");
    if (!name || !*name) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"need 'name' string\"}");
        return 0;
    }
    ca_skill sk; memset(&sk, 0, sizeof(sk));
    sk.name = name;
    sk.description = json_str(root, "description") ? json_str(root, "description") : "";
    sk.kind = (kind && *kind) ? kind : "shell";
    sk.body = json_str(root, "body") ? json_str(root, "body") : "";
    int rc = ctx->skills ? ca_skill_register(ctx->skills, &sk) : -1;
    if (rc != 0) {
        cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"error\":\"register failed (duplicate name or invalid kind)\"}");
        return 0;
    }
    if (ctx->state_root) ca_skill_registry_persist(ctx->skills, ctx->state_root);
    /* best-effort push to a networked marketplace */
    int pushed = 0;
    if (ctx->market_url && *ctx->market_url && root) {
        char *payload = cJSON_PrintUnformatted(root);
        if (payload) {
            pushed = ca_market_publish(ctx->market_url, "/v1/skills/publish", payload, 4000) == 0;
            free(payload);
        }
    }
    char *s = ctx->skills ? ca_skill_list_json(ctx->skills) : ca_strdup("[]");
    ca_http_resp_appendf(resp, "{\"ok\":true,\"pushed_to_market\":%s,\"skills\":",
                         pushed ? "true" : "false");
    ca_http_resp_append(resp, s ? s : "[]");
    ca_http_resp_append(resp, "}");
    free(s);
    cJSON_Delete(root);
    return 0;
}

static int h_skill_delete(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *name = req->path + strlen("/v1/skills/");
    int rc = ctx->skills ? ca_skill_unregister(ctx->skills, name) : -1;
    if (rc != 0) {
        resp->status = 404;
        ca_http_resp_json(resp, "{\"error\":\"skill not found\"}");
        return 0;
    }
    if (ctx->state_root) ca_skill_registry_persist(ctx->skills, ctx->state_root);
    ca_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

/* ---- Plugin marketplace (publish user-built standardized plugins) ---- */

static int h_plugins_market(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *reg = ctx->registry ? ca_plugin_registry_json(ctx->registry) : ca_strdup("{}");
    cJSON *root = cJSON_Parse(reg ? reg : "{}");
    free(reg);
    if (!root) root = cJSON_CreateObject();
    cJSON *tmpl = cJSON_CreateArray();
    static const char *tp[] = {"text-summarizer", "log-analyzer",
                               "qemu-crash-analyzer", "web-fetcher"};
    for (int i = 0; i < 4; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", tp[i]);
        cJSON_AddStringToObject(o, "description", "示例插件模板（AI 生成后可发布到市场）");
        cJSON_AddItemToArray(tmpl, o);
    }
    cJSON_AddItemToObject(root, "templates", tmpl);
    /* GitHub 热门应用（作为标准化插件来源的知名开源项目） */
    cJSON *gh = cJSON_CreateArray();
    static const struct { const char *name, *desc, *repo, *kind; } GH_PLUGINS[] = {
        { "shellcheck", "shell 脚本静态检查（生成/发布前自动审计）", "https://github.com/koalaman/shellcheck", "linter" },
        { "hadolint", "Dockerfile linter", "https://github.com/hadolint/hadolint", "linter" },
        { "semgrep", "轻量静态分析/安全扫描", "https://github.com/semgrep/semgrep", "security" },
        { "trufflehog", "密钥/敏感信息泄漏扫描", "https://github.com/trufflesecurity/trufflehog", "security" },
        { "tokei", "代码行数统计", "https://github.com/XAMPPRocky/tokei", "utility" },
        { "scc", "更快地统计代码量", "https://github.com/boyter/scc", "utility" },
        { "gitleaks", "Git 仓库密钥泄漏检测", "https://github.com/gitleaks/gitleaks", "security" },
        { "zstd", "Zstandard 压缩工具", "https://github.com/facebook/zstd", "utility" },
    };
    for (size_t i = 0; i < sizeof(GH_PLUGINS) / sizeof(GH_PLUGINS[0]); i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", GH_PLUGINS[i].name);
        cJSON_AddStringToObject(o, "description", GH_PLUGINS[i].desc);
        cJSON_AddStringToObject(o, "repo", GH_PLUGINS[i].repo);
        cJSON_AddStringToObject(o, "kind", GH_PLUGINS[i].kind);
        cJSON_AddItemToArray(gh, o);
    }
    cJSON_AddItemToObject(root, "github", gh);

    /* networked marketplace: merge remote templates + github apps when reachable */
    int market_online = 0;
    if (ctx->market_url && *ctx->market_url) {
        cJSON *remote = market_fetch_root(ctx, "/v1/plugins/market");
        if (remote) {
            market_online = market_merge_field(tmpl, remote, "templates");
            market_merge_field(gh, remote, "github");
            cJSON_Delete(remote);
        }
    }
    cJSON_AddBoolToObject(root, "market_online", market_online);
    if (ctx->market_url && *ctx->market_url)
        cJSON_AddStringToObject(root, "market_url", ctx->market_url);

    char *s = cJSON_PrintUnformatted(root);
    ca_http_resp_json(resp, s ? s : "{}");
    free(s);
    cJSON_Delete(root);
    return 0;
}

static int h_plugins_publish(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *name = json_str(root, "name");
    const char *kind = json_str(root, "kind");
    const char *body = json_str(root, "body");
    if (!name || !*name || !body || !*body) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"ok\":false,\"error\":\"need 'name' and 'body'\"}");
        return 0;
    }
    /* make it runnable as a skill */
    ca_skill sk; memset(&sk, 0, sizeof(sk));
    sk.name = name;
    sk.description = json_str(root, "description") ? json_str(root, "description") : "";
    sk.kind = (kind && *kind) ? kind : "shell";
    sk.body = body;
    ca_skill_register(ctx->skills, &sk); /* best-effort; skip if duplicate */

    char sig[17]; fnv1a_hex(body, sig);
    ca_plugin_meta m; memset(&m, 0, sizeof(m));
    m.name = (char *)name;
    m.version = "1.0.0";
    m.signature = sig;
    m.description = (char *)(json_str(root, "description") ? json_str(root, "description") : "");
    m.enabled = 1;
    m.built_ms = (int64_t)time(NULL) * 1000LL;
    cJSON *caps = cJSON_GetObjectItemCaseSensitive(root, "capabilities");
    if (caps && cJSON_IsArray(caps)) {
        m.n_caps = (size_t)cJSON_GetArraySize(caps);
        m.caps = (char **)calloc(m.n_caps ? m.n_caps : 1, sizeof(char *));
        for (size_t i = 0; i < m.n_caps; i++) {
            cJSON *ci = cJSON_GetArrayItem(caps, i);
            m.caps[i] = (ci && cJSON_IsString(ci)) ? ca_strdup(ci->valuestring) : ca_strdup("");
        }
    }
    int rc = ctx->registry ? ca_plugin_registry_register(ctx->registry, &m) : -1;
    for (size_t i = 0; i < m.n_caps; i++) free(m.caps[i]);
    free(m.caps);
    if (rc != 0) {
        cJSON_Delete(root);
        resp->status = 400;
        ca_http_resp_json(resp, "{\"ok\":false,\"error\":\"publish failed (duplicate version?)\"}");
        return 0;
    }
    if (ctx->state_root) {
        ca_plugin_registry_persist(ctx->registry, ctx->state_root);
        ca_skill_registry_persist(ctx->skills, ctx->state_root);
    }
    /* best-effort push to a networked marketplace */
    int pushed = 0;
    if (ctx->market_url && *ctx->market_url && root) {
        char *payload = cJSON_PrintUnformatted(root);
        if (payload) {
            pushed = ca_market_publish(ctx->market_url, "/v1/plugins/publish", payload, 4000) == 0;
            free(payload);
        }
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddBoolToObject(o, "pushed_to_market", pushed);
    char *s = cJSON_PrintUnformatted(o);
    ca_http_resp_json(resp, s ? s : "{\"ok\":true}");
    free(s);
    cJSON_Delete(o);
    cJSON_Delete(root);
    return 0;
}

static int h_plugin_market_delete(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *name = req->path + strlen("/v1/plugins/market/");
    int rc = ctx->registry ? ca_plugin_registry_unregister(ctx->registry, name) : -1;
    if (rc != 0) {
        resp->status = 404;
        ca_http_resp_json(resp, "{\"error\":\"plugin not found\"}");
        return 0;
    }
    if (ctx->skills) ca_skill_unregister(ctx->skills, name);
    if (ctx->state_root) {
        ca_plugin_registry_persist(ctx->registry, ctx->state_root);
        ca_skill_registry_persist(ctx->skills, ctx->state_root);
    }
    ca_http_resp_json(resp, "{\"ok\":true}");
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

/* ================= catalogs (MCP plaza + free models) ================= */

/* GET /v1/market/status — report networked marketplace configuration + reachability. */
static int h_market_status(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    int configured = (ctx->market_url && *ctx->market_url) ? 1 : 0;
    int online = 0;
    if (configured) {
        char *body = ca_market_fetch(ctx->market_url, "/v1/market/ping", 3000);
        online = body ? 1 : 0;
        free(body);
    }
    ca_http_resp_appendf(resp,
        "{\"configured\":%s,\"url\":\"%s\",\"online\":%s}",
        configured ? "true" : "false",
        ctx->market_url ? ctx->market_url : "",
        online ? "true" : "false");
    return 0;
}

/* ================= local model runtimes (free, no key) ================= */

/* Probe an HTTP endpoint; returns 1 when a 2xx response is received.
 * Deliberately uses the raw socket layer (nonblocking connect + select) rather
 * than the WinHTTP client: the bundled HTTP server is single-threaded, so a
 * probe must fail in a small, deterministic budget. WinHTTP's connect can
 * silently retry and stretch well past its nominal timeout, and on some
 * Windows installs the well-known Ollama/llama.cpp ports are dropped (not
 * refused), so a hard socket deadline is what actually bounds the freeze. */
static int local_probe(const char *base_url, const char *path) {
    char host[64];
    int port = 80;
    if (sscanf(base_url, "http://%63[^:]:%d", host, &port) != 2)
        return 0;
    ca_socket *s = ca_sock_connect(host, (uint16_t)port, 300);
    if (!s)
        return 0;
    char req[320];
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    int ok = 0;
    if (ca_sock_send(s, req, (int)strlen(req)) > 0 &&
        ca_sock_wait_readable(s, 300) > 0) {
        char buf[256];
        int n = ca_sock_recv(s, buf, (int)sizeof buf - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (strncmp(buf, "HTTP/", 5) == 0 && strstr(buf, " 200 "))
                ok = 1;
        }
    }
    ca_sock_close(s);
    return ok;
}

/* Is `name` resolvable on PATH (via the platform shell)? */
static int tool_exists(const char *name) {
    char cmd[300];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "where %s >nul 2>nul", name);
#else
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
#endif
    ca_proc_result *r = ca_proc_run(cmd, 4000);
    int found = r && r->exit_code == 0;
    ca_proc_result_free(r);
    return found;
}

/* Build the `ollama serve` command, preferring PATH, else common install dirs. */
static int ollama_start_cmd(char *out, size_t cap) {
    if (tool_exists("ollama")) { snprintf(out, cap, "ollama serve"); return 1; }
#if defined(_WIN32)
    {
        char p[600];
        const char *la = getenv("LOCALAPPDATA");
        if (la) {
            snprintf(p, sizeof p, "%s\\Programs\\Ollama\\ollama.exe", la);
            if (ca_fs_exists(p)) { snprintf(out, cap, "\"%s\" serve", p); return 1; }
        }
        const char *pf = getenv("ProgramFiles");
        if (pf) {
            snprintf(p, sizeof p, "%s\\Ollama\\ollama.exe", pf);
            if (ca_fs_exists(p)) { snprintf(out, cap, "\"%s\" serve", p); return 1; }
        }
    }
#endif
    return 0;
}

/* Base URL of the llama.cpp server to probe. The default (8081) deliberately
 * avoids port 8080 where this HTTP server itself listens: the bundled server is
 * single-threaded, so probing our own port would wait on a request we can never
 * serve — a self-deadlock that freezes every other API call. */
static void llamacpp_probe_url(const ca_config *cfg, char *out, size_t cap) {
    long port = (long)ca_config_get_int(cfg, "local.llamacpp_port", 8081);
    snprintf(out, cap, "http://127.0.0.1:%ld", port);
}

/* GET /v1/local/status — report local free runtimes (Ollama / llama.cpp).
 * Probing costs real I/O (and on some Windows boxes connecting to the
 * Ollama/llama.cpp ports is silently dropped, costing the full timeout).
 * Because the bundled HTTP server is single-threaded, we cache the result for
 * a few seconds so page loads / UI polling don't re-pay it every request. */
static char *g_local_status_cache = NULL;
static int64_t g_local_status_at = 0;

static int h_local_status(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    int64_t now = ca_time_now_ms();
    if (g_local_status_cache && now - g_local_status_at < 3000) {
        ca_http_resp_append(resp, g_local_status_cache);
        return 0;
    }
    int o_running = local_probe("http://127.0.0.1:11434", "/api/version");
    char lp_url[128];
    llamacpp_probe_url(ctx->config, lp_url, sizeof lp_url);
    int l_running = local_probe(lp_url, "/v1/models");
    char cmdbuf[512];
    int o_installed = ollama_start_cmd(cmdbuf, sizeof cmdbuf);
    ca_http_resp_appendf(resp,
        "{\"ollama\":{\"running\":%s,\"installed\":%s},"
        "\"llamacpp\":{\"running\":%s}}",
        o_running ? "true" : "false", o_installed ? "true" : "false",
        l_running ? "true" : "false");
    if (resp->body.buf && resp->body.len > 0) {
        free(g_local_status_cache);
        g_local_status_cache = (char *)malloc(resp->body.len + 1);
        if (g_local_status_cache) {
            memcpy(g_local_status_cache, resp->body.buf, resp->body.len);
            g_local_status_cache[resp->body.len] = '\0';
            g_local_status_at = now;
        }
    }
    return 0;
}

/* POST /v1/local/start — spawn a local runtime (non-blocking: returns right
 * after launch; the UI polls /v1/local/status for readiness). */
static int h_local_start(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *engine = root ? json_str(root, "engine") : NULL;
    if (!engine || !*engine) engine = "ollama";
    if (root) cJSON_Delete(root);

    if (strcmp(engine, "ollama") == 0) {
        if (local_probe("http://127.0.0.1:11434", "/api/version")) {
            ca_http_resp_json(resp, "{\"ok\":true,\"engine\":\"ollama\",\"already_running\":true}");
            return 0;
        }
        char cmd[512];
        if (!ollama_start_cmd(cmd, sizeof cmd)) {
            ca_http_resp_json(resp,
                "{\"ok\":false,\"engine\":\"ollama\",\"error\":\"未找到 ollama，请先到 ollama.com 安装并确保它在 PATH\"}");
            return 0;
        }
        int rc = ca_proc_spawn_detached(cmd);
        if (rc != 0) {
            ca_http_resp_json(resp,
                "{\"ok\":false,\"engine\":\"ollama\",\"error\":\"启动失败（无法创建进程）\"}");
            return 0;
        }
        ca_http_resp_json(resp,
            "{\"ok\":true,\"engine\":\"ollama\",\"spawned\":true,"
            "\"note\":\"已启动 ollama serve，首次运行需拉取模型，请稍候刷新状态\"}");
        return 0;
    }
    if (strcmp(engine, "llamacpp") == 0 || strcmp(engine, "llama") == 0) {
        char lp_url[128];
        llamacpp_probe_url(ctx->config, lp_url, sizeof lp_url);
        if (local_probe(lp_url, "/v1/models")) {
            ca_http_resp_json(resp, "{\"ok\":true,\"engine\":\"llamacpp\",\"already_running\":true}");
            return 0;
        }
        const char *cmd = ca_config_get_str(ctx->config, "local.llamacpp_cmd", NULL);
        if (!cmd || !*cmd) {
            ca_http_resp_json(resp,
                "{\"ok\":false,\"engine\":\"llamacpp\","
                "\"error\":\"未配置启动命令，请在 config 中设置 local.llamacpp_cmd（如 server 可执行文件路径）\"}");
            return 0;
        }
        int rc = ca_proc_spawn_detached(cmd);
        if (rc != 0) {
            ca_http_resp_json(resp,
                "{\"ok\":false,\"engine\":\"llamacpp\",\"error\":\"启动失败（无法创建进程）\"}");
            return 0;
        }
        ca_http_resp_json(resp,
            "{\"ok\":true,\"engine\":\"llamacpp\",\"spawned\":true,"
            "\"note\":\"已启动，请稍候刷新状态\"}");
        return 0;
    }
    ca_http_resp_json(resp,
        "{\"ok\":false,\"engine\":\"unknown\",\"error\":\"unknown engine (ollama|llamacpp)\"}");
    return 0;
}

static int h_catalog_mcp(const ca_http_request *req, ca_http_response *resp, void *ud) {
    (void)req;
    (void)ud;
    char *s = ca_catalog_mcp_json();
    ca_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_catalog_models(const ca_http_request *req, ca_http_response *resp, void *ud) {
    (void)req;
    (void)ud;
    char *s = ca_catalog_models_json();
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

/* Fire-and-forget forward a console-originated message to its linked external
 * channel (best-effort: drops the response). Inbound channel messages are
 * injected directly via channel_ingest and never reach here, so there is no
 * echo. */
static void im_forward_to_channel(cagent_ctx *ctx, int64_t session_id,
                                  const char *content) {
    if (!ctx || !ctx->channels || !ctx->im || !content) return;
    const char *chn = ca_im_session_channel(ctx->im, session_id);
    if (!chn || !*chn) return;
    char *r = ca_im_channel_send(ctx->channels, chn, content);
    if (r) free(r);
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
            if (id > 0) {
                im_push(ctx, (int64_t)sid->valuedouble, id, "user", snd, content->valuestring);
                im_forward_to_channel(ctx, (int64_t)sid->valuedouble, content->valuestring);
            }
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
    const char *name = NULL, *kind = NULL, *channel = NULL;
    const char *members[64];
    size_t n_members = 0;
    if (root && cJSON_IsObject(root)) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "kind");
        cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "members");
        cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "channel");
        if (n && cJSON_IsString(n)) name = n->valuestring;
        if (k && cJSON_IsString(k)) kind = k->valuestring;
        if (c && cJSON_IsString(c)) channel = c->valuestring;
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
    cJSON_Delete(root);
    if (id < 0) { resp->status = 500; ca_http_resp_json(resp, "{\"error\":\"create failed\"}"); return 0; }
    if (id > 0 && ctx->channels && channel && *channel)
        ca_im_session_set_channel(ctx->im, id, channel);
    ca_http_resp_appendf(resp, "{\"id\":%lld}", (long long)id);
    return 0;
}

/* Channel bridge: external messaging channels (feishu/wecom/generic/telegram). */
static int h_im_channels(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->channels) { resp->status = 500; ca_http_resp_json(resp, "{\"error\":\"channels disabled\"}"); return 0; }
    char *s = ca_im_channels_json(ctx->channels);
    ca_http_resp_json(resp, s ? s : "{\"channels\":[]}");
    free(s);
    return 0;
}

static int h_im_channel_add(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->channels) { resp->status = 500; ca_http_resp_json(resp, "{\"error\":\"channels disabled\"}"); return 0; }
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *name = NULL, *type = NULL, *endpoint = NULL, *token = NULL, *target = NULL;
    int enabled = 1;
    if (root && cJSON_IsObject(root)) {
        cJSON *o;
        o = cJSON_GetObjectItemCaseSensitive(root, "name"); if (o && cJSON_IsString(o)) name = o->valuestring;
        o = cJSON_GetObjectItemCaseSensitive(root, "type"); if (o && cJSON_IsString(o)) type = o->valuestring;
        o = cJSON_GetObjectItemCaseSensitive(root, "endpoint"); if (o && cJSON_IsString(o)) endpoint = o->valuestring;
        o = cJSON_GetObjectItemCaseSensitive(root, "token"); if (o && cJSON_IsString(o)) token = o->valuestring;
        o = cJSON_GetObjectItemCaseSensitive(root, "target"); if (o && cJSON_IsString(o)) target = o->valuestring;
        o = cJSON_GetObjectItemCaseSensitive(root, "enabled"); if (o && cJSON_IsBool(o)) enabled = cJSON_IsTrue(o);
    }
    int rc = -1;
    if (name && *name && type && *type) {
        ca_im_channel ch;
        memset(&ch, 0, sizeof(ch));
        ch.name = (char *)name;
        ch.type = (char *)type;
        ch.endpoint = endpoint && *endpoint ? (char *)endpoint : NULL;
        ch.token = token && *token ? (char *)token : NULL;
        ch.target = target && *target ? (char *)target : NULL;
        ch.enabled = enabled;
        rc = ca_im_channel_register(ctx->channels, &ch);
    }
    if (root) cJSON_Delete(root);
    if (rc != 0) { resp->status = 400; ca_http_resp_json(resp, "{\"error\":\"need 'name' and 'type' (feishu|wecom|generic|telegram)\"}"); return 0; }
    ca_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

static int h_im_channel_remove(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->channels) { resp->status = 500; ca_http_resp_json(resp, "{\"error\":\"channels disabled\"}"); return 0; }
    const char *name = req->path + strlen("/v1/im/channels/");
    int rc = ca_im_channel_remove(ctx->channels, name);
    if (rc == 0) {
        /* unbind any session that pointed at this channel */
        if (ctx->im) {
            size_t n = 0; ca_im_session *ss = ca_im_list_sessions(ctx->im, &n);
            for (size_t i = 0; i < n; i++)
                if (ss[i].channel && strcmp(ss[i].channel, name) == 0)
                    ca_im_session_set_channel(ctx->im, ss[i].id, NULL);
            ca_im_sessions_free(ss, n);
        }
    }
    ca_http_resp_appendf(resp, "{\"removed\":%s}", rc == 0 ? "true" : "false");
    return 0;
}

static int h_im_channel_send(const ca_http_request *req, ca_http_response *resp, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->channels) { resp->status = 500; ca_http_resp_json(resp, "{\"error\":\"channels disabled\"}"); return 0; }
    const char *base = req->path + strlen("/v1/im/channels/");
    size_t blen = strlen(base);
    const char *send = "/send";
    if (blen > strlen(send) && strcmp(base + blen - strlen(send), send) == 0)
        blen -= strlen(send);
    char name[128];
    if (blen >= sizeof(name)) blen = sizeof(name) - 1;
    memcpy(name, base, blen);
    name[blen] = '\0';
    if (!*name) { resp->status = 404; ca_http_resp_json(resp, "{\"error\":\"missing channel name\"}"); return 0; }
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *text = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (t && cJSON_IsString(t)) text = t->valuestring;
    }
    if (root) cJSON_Delete(root);
    if (!text || !*text) { resp->status = 400; ca_http_resp_json(resp, "{\"error\":\"need 'text' string\"}"); return 0; }
    char *r = ca_im_channel_send(ctx->channels, name, text);
    ca_http_resp_json(resp, r ? r : "{\"ok\":false,\"error\":\"channel not found\"}");
    free(r);
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
        im_forward_to_channel(ctx, session_id, content);
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
    ca_http_server_route(ctx->http, "GET", "/v1/market/status", h_market_status, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/local/status", h_local_status, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/local/start", h_local_start, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/routes", h_routes, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/routes", h_route_add, ctx);
    ca_http_server_route(ctx->http, "DELETE", "/v1/routes/", h_route_delete, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/config/llm", h_config_llm_get, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/config/llm", h_config_llm, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/config/llm/test", h_config_llm_test, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/usage", h_usage, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/plugins", h_plugins, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/plugins/generate", h_plugin_generate, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/plugins/market", h_plugins_market, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/plugins/publish", h_plugins_publish, ctx);
    ca_http_server_route(ctx->http, "DELETE", "/v1/plugins/market/", h_plugin_market_delete, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/skills", h_skills, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/skills/run", h_skill_run, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/skills/market", h_skills_market, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/skills/publish", h_skills_publish, ctx);
    ca_http_server_route(ctx->http, "DELETE", "/v1/skills/", h_skill_delete, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/mcp", h_mcp, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/mcp", h_mcp_add, ctx);
    ca_http_server_route(ctx->http, "DELETE", "/v1/mcp/", h_mcp_delete, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/cluster", h_cluster, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/im/sessions", h_im_sessions, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/im/sessions", h_im_session_create, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/im/search", h_im_search, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/im/channels", h_im_channels, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/im/channels", h_im_channel_add, ctx);
    ca_http_server_route(ctx->http, "DELETE", "/v1/im/channels/", h_im_channel_remove, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/im/channels/", h_im_channel_send, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/im/sessions/", h_im_session_route, ctx);
    ca_http_server_route(ctx->http, "POST", "/v1/im/sessions/", h_im_session_route, ctx);
    ca_http_server_route(ctx->http, "DELETE", "/v1/im/sessions/", h_im_session_route, ctx);
    ca_http_server_ws_route(ctx->http, "/ws", on_ws_msg, ctx);
    ca_http_server_route(ctx->http, "GET", "/metrics", h_metrics, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/catalog/mcp", h_catalog_mcp, ctx);
    ca_http_server_route(ctx->http, "GET", "/v1/catalog/models", h_catalog_models, ctx);
    ca_http_server_route(ctx->http, "GET", "/", h_index, ctx);
    ca_http_server_route(ctx->http, "GET", "/favicon.ico", h_favicon, ctx);
    return 0;
}
