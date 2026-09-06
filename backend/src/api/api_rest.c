/* api_rest.c — REST API handlers wired to the cognitive-os-agent runtime context. */
#include "cognitive-os-agent/api/api_rest.h"
#include "cognitive-os-agent/api/http_server.h"
#include "cognitive-os-agent/api/web_ui.h"
#include "cognitive-os-agent/api/market.h"
#include "cognitive-os-agent/runtime/scheduler.h"
#include "cognitive-os-agent/runtime/flow.h"
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/memory/memory.h"
#include "cognitive-os-agent/snapshot/snapshot.h"
#include "cognitive-os-agent/infra/metrics.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/infra/config.h"
#include "cognitive-os-agent/infra/catalog.h"
#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/os/os_proc.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/os/os_socket.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "cJSON.h"

/* Copy the (not null-terminated) request body into a C string. */
static char *body_str(const coa_http_request *req) {
    if (!req->body || req->body_len == 0) return coa_strdup("");
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
static int authz_ok(coa_ctx *ctx, const coa_http_request *req, coa_http_response *resp) {
    if (!ctx->auth) return 1; /* auth not configured: open access */
    if (coa_auth_check_header(ctx->auth, req->authorization)) return 1;
    resp->status = 401;
    coa_http_resp_json(resp, "{\"error\":\"unauthorized\"}");
    return 0;
}

static const char *task_status_str(coa_task_status st) {
    switch (st) {
        case COA_TS_QUEUED:    return "QUEUED";
        case COA_TS_RUNNING:   return "RUNNING";
        case COA_TS_DONE:      return "DONE";
        case COA_TS_FAILED:    return "FAILED";
        case COA_TS_CANCELLED: return "CANCELLED";
        case COA_TS_TIMEOUT:   return "TIMEOUT";
        default:              return "UNKNOWN";
    }
}

static int h_task_create(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
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
        coa_http_resp_json(resp, "{\"error\":\"missing 'prompt' string\"}");
        return 0;
    }
    int64_t id = coa_scheduler_submit(ctx->scheduler, 0, prompt, NULL, 0);
    /* prompt borrows into the cJSON tree — copy before freeing it */
    char prompt_copy[512];
    snprintf(prompt_copy, sizeof(prompt_copy), "%s", prompt);
    cJSON_Delete(root);
    if (id < 0) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"scheduler submit failed\"}");
        return 0;
    }
    if (ctx->state) coa_state_store_task_set(ctx->state, id, "QUEUED", prompt_copy);
    coa_http_resp_appendf(resp, "{\"id\":%lld,\"status\":\"queued\"}", (long long)id);
    return 0;
}

static int h_task_get(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *suffix = req->path + strlen("/v1/tasks/");
    if (!*suffix) {
        resp->status = 404;
        coa_http_resp_json(resp, "{\"error\":\"missing task id\"}");
        return 0;
    }
    int64_t id = atoll(suffix);
    coa_task *t = coa_scheduler_get(ctx->scheduler, id);
    if (!t) {
        resp->status = 404;
        coa_http_resp_json(resp, "{\"error\":\"task not found\"}");
        return 0;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double)t->id);
    cJSON_AddStringToObject(o, "status", task_status_str(t->status));
    if (t->input) cJSON_AddStringToObject(o, "input", t->input);
    if (t->output) cJSON_AddStringToObject(o, "output", t->output);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) { coa_http_resp_append(resp, s); free(s); }
    return 0;
}

/* POST /v1/chat {"message":"..."} — conversational counterpart of task
 * creation: same async scheduler path, but semantically a chat turn (the
 * reasoning engine keeps the multi-turn context across calls). */
static int h_chat(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *msg = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (p && cJSON_IsString(p)) msg = p->valuestring;
    }
    if (!msg || !*msg) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"missing 'message' string\"}");
        return 0;
    }
    int64_t id = coa_scheduler_submit(ctx->scheduler, 0, msg, NULL, 0);
    cJSON_Delete(root);
    if (id < 0) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"scheduler submit failed\"}");
        return 0;
    }
    coa_http_resp_appendf(resp, "{\"id\":%lld,\"status\":\"queued\"}", (long long)id);
    return 0;
}

/* POST /v1/orchestrate {"task":"..."} — multi-agent orchestration: the task is
 * decomposed across registered agents (blackboard + merge), executed async via
 * the scheduler. Poll /v1/tasks/<id> for the final answer; live progress comes
 * over WebSocket (source "orchestrator"); steps land on the blackboard "orch/". */
static int h_orchestrate(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *task = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "task");
        if (p && cJSON_IsString(p)) task = p->valuestring;
    }
    if (!task || !*task) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"missing 'task' string\"}");
        return 0;
    }
    /* userdata = marker so the task runner routes to coa_orchestrate */
    int64_t id = coa_scheduler_submit(ctx->scheduler, 0, task, (void *)1, 0);
    cJSON_Delete(root);
    if (id < 0) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"scheduler submit failed\"}");
        return 0;
    }
    coa_http_resp_appendf(resp, "{\"id\":%lld,\"status\":\"queued\"}", (long long)id);
    return 0;
}

/* POST /v1/flows — compile + execute an explicit DAG flow (async via the
 * scheduler). Body: either the raw DAG {"nodes":[...],"edges":[...]} or
 * {"flow": <that object>}. Validated synchronously so 400s carry the reason. */
static int h_flow_run(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    cJSON *dag = NULL;
    if (root && cJSON_IsObject(root)) {
        dag = root;
        cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "flow");
        if (f && cJSON_IsObject(f)) dag = f;
        if (!cJSON_GetObjectItemCaseSensitive(dag, "nodes")) dag = NULL;
    }
    if (!dag) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need a flow object with 'nodes'\"}");
        return 0;
    }
    char *dag_json = cJSON_PrintUnformatted(dag);
    cJSON_Delete(root);
    if (!dag_json) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"serialize failed\"}");
        return 0;
    }
    char *verr = NULL;
    if (coa_flow_validate(dag_json, &verr) != 0) {
        coa_http_resp_appendf(resp, "{\"error\":\"invalid flow: %s\"}",
                             verr ? verr : "unknown");
        free(verr);
        free(dag_json);
        resp->status = 400;
        return 0;
    }
    free(verr);
    /* userdata marker 2 routes the task runner to coa_flow_run */
    int64_t id = coa_scheduler_submit(ctx->scheduler, 0, dag_json, (void *)2, 0);
    free(dag_json);
    if (id < 0) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"scheduler submit failed\"}");
        return 0;
    }
    coa_http_resp_appendf(resp, "{\"id\":%lld,\"status\":\"queued\"}", (long long)id);
    return 0;
}

/* POST /v1/flows/decompose {task} — compile a task into a Flow DAG via the
 * orchestrator's LLM decomposition WITHOUT executing it. Returns the DAG so
 * the client can inspect/modify it before POST /v1/flows. */
static int h_flow_decompose(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    cJSON *t = root ? cJSON_GetObjectItemCaseSensitive(root, "task") : NULL;
    if (!t || !cJSON_IsString(t) || !t->valuestring || !*t->valuestring) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"missing 'task' string\"}");
        return 0;
    }
    char *dag_json = NULL;
    int rc = coa_flow_decompose(ctx, t->valuestring, &dag_json);
    cJSON_Delete(root);
    if (rc != 0 || !dag_json) {
        resp->status = 422;
        coa_http_resp_json(resp,
            "{\"error\":\"no multi-agent plan (register agents or check LLM config)\"}");
        return 0;
    }
    cJSON *dag = cJSON_Parse(dag_json);
    free(dag_json);
    if (!dag) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"dag serialize failed\"}");
        return 0;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "dag", dag);
    char *js = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (js) { coa_http_resp_json(resp, js); free(js); }
    return 0;
}

/* ---------- policy rules: list / add / delete (persisted to policy.json) ---------- */

static void policy_path_of(coa_ctx *ctx, char *out, size_t cap) {
    coa_path_join(out, cap, ctx->state_root ? ctx->state_root : "state", "policy.json");
}

static int h_policy_rules(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    (void)req;
    cJSON *arr = cJSON_CreateArray();
    int n = ctx->policy ? coa_policy_rule_count(ctx->policy) : 0;
    for (int i = 0; i < n; i++) {
        const char *tool = NULL, *action = NULL, *reason = NULL;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", i);
        if (coa_policy_rule_get(ctx->policy, (size_t)i, &tool, &action, &reason) == 0) {
            cJSON_AddStringToObject(o, "tool", tool ? tool : "*");
            cJSON_AddStringToObject(o, "action", action ? action : "deny");
            cJSON_AddStringToObject(o, "reason", reason ? reason : "");
        }
        cJSON_AddItemToArray(arr, o);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "rules", arr);
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (js) { coa_http_resp_append(resp, js); free(js); }
    return 0;
}

static int h_policy_add(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    cJSON *t = root ? cJSON_GetObjectItemCaseSensitive(root, "tool") : NULL;
    cJSON *a = root ? cJSON_GetObjectItemCaseSensitive(root, "action") : NULL;
    cJSON *r = root ? cJSON_GetObjectItemCaseSensitive(root, "reason") : NULL;
    if (!t || !cJSON_IsString(t) || !t->valuestring || !*t->valuestring ||
        !a || !cJSON_IsString(a) || !a->valuestring) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need 'tool' and 'action' (allow|deny|ask)\"}");
        return 0;
    }
    coa_policy_add_rule(ctx->policy, t->valuestring, a->valuestring,
                       (r && cJSON_IsString(r)) ? r->valuestring : NULL);
    cJSON_Delete(root);
    char ppath[600];
    policy_path_of(ctx, ppath, sizeof(ppath));
    int saved = coa_policy_save_file(ctx->policy, ppath);
    coa_http_resp_appendf(resp, "{\"ok\":true,\"saved\":%s}",
                         saved == 0 ? "true" : "false");
    return 0;
}

static int h_policy_delete(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    int idx = atoi(req->path + strlen("/v1/policy/rules/"));
    if (idx < 0 || (size_t)idx >= (size_t)coa_policy_rule_count(ctx->policy)) {
        resp->status = 404;
        coa_http_resp_json(resp, "{\"error\":\"no such rule\"}");
        return 0;
    }
    coa_policy_remove_rule(ctx->policy, (size_t)idx);
    char ppath[600];
    policy_path_of(ctx, ppath, sizeof(ppath));
    coa_policy_save_file(ctx->policy, ppath);
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

/* GET /v1/hooks — list registered hooks [{"id":N,"event":"..."}]. */
static int h_hooks(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    (void)req;
    char *js = ctx->hooks ? coa_hook_registry_json(ctx->hooks) : coa_strdup("[]");
    coa_http_resp_appendf(resp, "{\"hooks\":%s}", js ? js : "[]");
    free(js);
    return 0;
}

/* POST /v1/hooks {"event":"agent.after_run","type":"log","file":"..."} —
 * register a hook from outside the process (web UI, plugins). type "log"
 * appends {"ts_ms","event","payload"} JSON lines to `file` (default
 * <state_root>/hooks-external.jsonl). Returns the hook id. */
static int h_hook_add(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->hooks) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"hook registry unavailable\"}");
        return 0;
    }
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    cJSON *ev = root ? cJSON_GetObjectItemCaseSensitive(root, "event") : NULL;
    if (!ev || !cJSON_IsString(ev) || !ev->valuestring || !*ev->valuestring) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp,
            "{\"error\":\"need 'event' (name or '*') and optional 'file', 'type'='log'\"}");
        return 0;
    }
    char fpath[600];
    cJSON *fj = cJSON_GetObjectItemCaseSensitive(root, "file");
    if (fj && cJSON_IsString(fj) && fj->valuestring && *fj->valuestring) {
        snprintf(fpath, sizeof(fpath), "%s", fj->valuestring);
    } else {
        coa_path_join(fpath, sizeof(fpath),
                     ctx->state_root ? ctx->state_root : "state",
                     "hooks-external.jsonl");
    }
    char *fpath_heap = coa_strdup(fpath);
    int id = fpath_heap
        ? coa_hook_register(ctx->hooks, ev->valuestring, coa_hook_audit_file, fpath_heap)
        : -1;
    cJSON_Delete(root);
    if (id < 0) {
        free(fpath_heap);
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"register failed\"}");
        return 0;
    }
    coa_http_resp_appendf(resp, "{\"ok\":true,\"id\":%d}", id);
    return 0;
}

/* DELETE /v1/hooks/<id> — unregister a hook (builtin audit hook id 1 stays). */
static int h_hook_delete(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    int id = atoi(req->path + strlen("/v1/hooks/"));
    if (id <= 0 || coa_hook_unregister(ctx->hooks, id) != 0) {
        resp->status = 404;
        coa_http_resp_json(resp, "{\"error\":\"no such hook\"}");
        return 0;
    }
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

/* GET /v1/chat/history — recent conversation turns (oldest first) for the
 * chat panel to backfill on open. */
static int h_chat_history(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    (void)req;
    char *turns = ctx->reasoning ? coa_reasoning_history_json(ctx->reasoning, 20)
                                 : coa_strdup("[]");
    coa_http_resp_appendf(resp, "{\"turns\":%s}", turns ? turns : "[]");
    free(turns);
    return 0;
}

/* Copy `in` (the ?name= parameter) into `out`, percent-decoding and replacing
 * unsafe characters so the result is a plain basename (no traversal). */
static void sanitize_upload_name(const char *in, char *out, size_t cap) {
    char dec[256];
    size_t di = 0;
    for (const char *p = in; *p && *p != '&' && di + 1 < sizeof(dec); p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            dec[di++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            dec[di++] = ' ';
        } else {
            dec[di++] = *p;
        }
    }
    dec[di] = '\0';
    size_t oi = 0;
    for (size_t k = 0; k < di && oi + 1 < cap; k++) {
        unsigned char c = (unsigned char)dec[k];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || c < 0x20)
            c = '_';
        out[oi++] = (char)c;
    }
    out[oi] = '\0';
    /* strip leading dots: no hidden entries, no "." / ".." */
    size_t strip = 0;
    while (out[strip] == '.') strip++;
    if (strip) memmove(out, out + strip, oi - strip + 1);
}

static void uploads_dir_of(const coa_ctx *ctx, char *dir, size_t cap) {
    coa_path_join(dir, cap, ctx->state_root, "uploads");
    coa_fs_mkdirs(dir);
}

/* POST /v1/upload?name=<filename> — raw-body upload for RAG. The file is
 * stored under <state_root>/uploads/<name>; text content is chunked into the
 * vector store so later prompts recall it via "## Retrieved context". */
static int h_upload(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char name[192];
    const char *nmp = strstr(req->query, "name=");
    sanitize_upload_name(nmp ? nmp + 5 : req->query, name, sizeof(name));
    if (!name[0] || !req->body || req->body_len == 0) {
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need ?name=<file> and a non-empty body\"}");
        return 0;
    }
    char dir[600], fpath[820];
    uploads_dir_of(ctx, dir, sizeof(dir));
    coa_path_join(fpath, sizeof(fpath), dir, name);
    if (coa_fs_write_file(fpath, req->body, req->body_len) != 0) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"write failed\"}");
        return 0;
    }
    /* text files (no NUL byte) go into the vector store */
    int chunks = 0;
    if (!memchr(req->body, 0, req->body_len) && ctx->memory) {
        char *text = malloc(req->body_len + 1);
        if (text) {
            memcpy(text, req->body, req->body_len);
            text[req->body_len] = '\0';
            char *clean = coa_str_utf8_sanitize(text); /* GBK/invalid bytes guard */
            if (clean) { free(text); text = clean; }
            char base[224];
            snprintf(base, sizeof(base), "upload:%s", name);
            chunks = coa_memory_index_text(ctx->memory, base, text);
            free(text);
        }
    }
    coa_http_resp_appendf(resp, "{\"ok\":true,\"name\":\"%s\",\"size\":%d,\"chunks\":%d}",
                         name, (int)req->body_len, chunks);
    return 0;
}

/* GET /v1/uploads — uploaded files with sizes. */
static int h_uploads(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    (void)req;
    char dir[600];
    uploads_dir_of(ctx, dir, sizeof(dir));
    coa_dir_list dl;
    cJSON *arr = cJSON_CreateArray();
    if (coa_fs_list_dir(dir, &dl) == 0) {
        for (size_t i = 0; i < dl.count; i++) {
            if (dl.items[i].is_dir) continue;
            char fpath[820];
            coa_path_join(fpath, sizeof(fpath), dir, dl.items[i].name);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", dl.items[i].name);
            cJSON_AddNumberToObject(o, "size", (double)coa_fs_file_size(fpath));
            cJSON_AddItemToArray(arr, o);
        }
        coa_fs_list_free(&dl);
    }
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    coa_http_resp_appendf(resp, "{\"files\":%s}", s ? s : "[]");
    free(s);
    return 0;
}

/* DELETE /v1/uploads/<name> — remove an uploaded file (its vectors stay until
 * the next startup rebuild, which scans the directory). */
static int h_upload_delete(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char name[192];
    sanitize_upload_name(req->path + strlen("/v1/uploads/"), name, sizeof(name));
    if (!name[0]) {
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"missing file name\"}");
        return 0;
    }
    char dir[600], fpath[820];
    coa_path_join(dir, sizeof(dir), ctx->state_root, "uploads");
    snprintf(fpath, sizeof(fpath), "%s/%s", dir, name);
    if (coa_fs_remove(fpath) != 0) {
        resp->status = 404;
        coa_http_resp_json(resp, "{\"error\":\"not found\"}");
        return 0;
    }
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

static int h_tools(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    cJSON *arr = cJSON_CreateArray();
    int n = coa_tool_registry_count(ctx->tools);
    for (int i = 0; i < n; i++) {
        const coa_tool *t = coa_tool_registry_get(ctx->tools, (size_t)i);
        if (!t) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", t->name);
        cJSON_AddStringToObject(o, "description", t->description ? t->description : "");
        cJSON_AddBoolToObject(o, "write", t->is_write ? 1 : 0);
        if (t->json_schema && *t->json_schema) {
            cJSON *sc = cJSON_Parse(t->json_schema);
            if (sc) cJSON_AddItemToObject(o, "schema", sc);
            else cJSON_AddStringToObject(o, "schema", t->json_schema);
        }
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_memory(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    cJSON *root = cJSON_CreateObject();
    if (ctx->memory) {
        char *w = coa_memory_working_json(ctx->memory);
        char *l = coa_memory_longterm_json(ctx->memory);
        cJSON *wj = w ? cJSON_Parse(w) : NULL;
        cJSON *lj = l ? cJSON_Parse(l) : NULL;
        cJSON_AddItemToObject(root, "working", wj ? wj : cJSON_CreateArray());
        cJSON_AddItemToObject(root, "longterm", lj ? lj : cJSON_CreateObject());
        free(w);
        free(l);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_snapshots(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->snapshot ? coa_snapshot_list(ctx->snapshot) : coa_strdup("[]");
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_snapshot_rollback(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    int rc = ctx->snapshot ? coa_snapshot_restore_latest(ctx->snapshot) : -1;
    coa_http_resp_appendf(resp, "{\"ok\":%s}", rc == 0 ? "true" : "false");
    return 0;
}

static int h_blackboard(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->blackboard ? coa_blackboard_snapshot_json(ctx->blackboard) : coa_strdup("{}");
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_blackboard_put(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
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
        coa_http_resp_json(resp, "{\"error\":\"need 'key' and 'value' strings\"}");
        return 0;
    }
    if (ctx->blackboard) coa_blackboard_put(ctx->blackboard, key, val);
    cJSON_Delete(root);
    coa_http_resp_appendf(resp, "{\"ok\":true}");
    return 0;
}

static int h_agents(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->agents ? coa_agent_pool_snapshot_json(ctx->agents) : coa_strdup("{}");
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_agent_add(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *name = NULL, *role = NULL, *provider = NULL, *model = NULL;
    char name_buf[128], role_buf[256], prov_buf[128], model_buf[128];
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
        /* copy out of the cJSON tree — the borrowed valuestrings must stay
         * valid after cJSON_Delete below */
        snprintf(name_buf, sizeof(name_buf), "%s", name ? name : "");
        snprintf(role_buf, sizeof(role_buf), "%s", role ? role : "");
        snprintf(prov_buf, sizeof(prov_buf), "%s", provider ? provider : "");
        snprintf(model_buf, sizeof(model_buf), "%s", model ? model : "");
        name = name_buf; role = role_buf;
        provider = *prov_buf ? prov_buf : NULL;
        model = *model_buf ? model_buf : NULL;
    }
    if (root) cJSON_Delete(root);
    if (!name || !*name) {
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need 'name' string\"}");
        return 0;
    }
    int idx = ctx->agents ? coa_agent_pool_add_model(ctx->agents, name, role ? role : "",
                                                    provider, model) : -1;
    if (idx < 0) { resp->status = 400; coa_http_resp_json(resp, "{\"error\":\"add agent failed (duplicate?)\"}"); return 0; }
    coa_agent_pool_save(ctx->agents, ctx->state_root); /* roster persists across restarts */
    if (ctx->state) coa_state_store_agent_set(ctx->state, name, role, "idle");
    coa_http_resp_appendf(resp, "{\"ok\":true,\"index\":%d}", idx);
    return 0;
}

static int h_agent_run(const coa_http_request *req, coa_http_response *resp, void *ud);

static int h_agent_post(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    /* dispatch: "<name>/run" executes a task through the reasoning engine */
    {
        const char *p = req->path + strlen("/v1/agents/");
        size_t plen = strlen(p);
        if (plen >= 4 && strcmp(p + plen - 4, "/run") == 0)
            return h_agent_run(req, resp, ud);
    }
    const char *rest = req->path + strlen("/v1/agents/");
    char name[128];
    snprintf(name, sizeof(name), "%s", rest);
    char *slash = strstr(name, "/post");
    if (slash) *slash = '\0';
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *key = NULL, *val = NULL;
    char key_buf[160], val_buf[2048];
    if (root && cJSON_IsObject(root)) {
        cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "key");
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (k && cJSON_IsString(k)) key = k->valuestring;
        if (v && cJSON_IsString(v)) val = v->valuestring;
        /* copy out before cJSON_Delete — borrowed valuestrings */
        snprintf(key_buf, sizeof(key_buf), "%s", key ? key : "");
        snprintf(val_buf, sizeof(val_buf), "%s", val ? val : "");
        key = key_buf; val = val_buf;
    }
    if (root) cJSON_Delete(root);
    if (!key || !*key || !val) {
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need 'key' and 'value' strings\"}");
        return 0;
    }
    int rc = ctx->agents ? coa_agent_post(ctx->agents, name, key, val) : -1;
    coa_http_resp_appendf(resp, "{\"ok\":%s}", rc == 0 ? "true" : "false");
    return 0;
}

/* POST /v1/agents/<name>/run  {"task": "..."} — execute a task as the agent
 * through the reasoning engine; the result is published on the shared
 * blackboard and returned here. (Dispatched from h_agent_post for /run.) */
static int h_agent_run(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *rest = req->path + strlen("/v1/agents/");
    char name[128];
    snprintf(name, sizeof(name), "%s", rest);
    char *slash = strstr(name, "/run");
    if (slash) *slash = '\0';
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *task = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "task");
        if (t && cJSON_IsString(t)) task = t->valuestring;
    }
    if (!task || !*task) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need 'task' string\"}");
        return 0;
    }
    char *answer = NULL;
    int rc = coa_agent_run(ctx, name, task, &answer);
    cJSON_Delete(root);
    if (rc == -2) {
        free(answer);
        resp->status = 404;
        coa_http_resp_json(resp, "{\"error\":\"unknown agent\"}");
        return 0;
    }
    if (rc != 0) {
        free(answer);
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"agent run failed\"}");
        return 0;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", 1);
    cJSON_AddStringToObject(out, "agent", name);
    cJSON_AddStringToObject(out, "answer", answer ? answer : "");
    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    free(answer);
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_metrics(const coa_http_request *req, coa_http_response *resp, void *ud) {
    (void)req;
    coa_ctx *ctx = (coa_ctx *)ud;
    snprintf(resp->content_type, sizeof(resp->content_type), "text/plain; version=0.0.4");
    char *s = ctx->metrics ? coa_metrics_render(ctx->metrics) : coa_strdup("");
    coa_http_resp_append(resp, s ? s : "");
    free(s);
    return 0;
}

static int h_index(const coa_http_request *req, coa_http_response *resp, void *ud) {
    (void)req;
    (void)ud;
    snprintf(resp->content_type, sizeof(resp->content_type), "text/html; charset=utf-8");
    coa_http_resp_append(resp, coa_web_index_html);
    return 0;
}

static int h_favicon(const coa_http_request *req, coa_http_response *resp, void *ud) {
    (void)req;
    (void)ud;
    resp->status = 204;
    return 0;
}

static int h_trace(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->trace ? coa_trace_json(ctx->trace) : coa_strdup("[]");
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_routes(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->router ? coa_router_json(ctx->router) : coa_strdup("[]");
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_route_add(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"body must be a JSON object\"}");
        return 0;
    }
    const char *name = json_str(root, "name");
    const char *provider = json_str(root, "provider");
    if (!name || !*name || !provider || !*provider) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need 'name' and 'provider' strings\"}");
        return 0;
    }
    if (ctx->router)
        coa_router_add_ex(ctx->router, name, provider, json_str(root, "base_url"),
                         json_str(root, "api_key"), json_str(root, "model"),
                         json_dbl(root, "weight", 1.0),
                         (int)json_dbl(root, "cost_rank", 0),
                         (int)json_dbl(root, "latency_ms", 0),
                         json_str(root, "caps"));
    cJSON_Delete(root);
    char *s = ctx->router ? coa_router_json(ctx->router) : coa_strdup("[]");
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    /* persist so configured routes survive restart */
    if (ctx->router && ctx->state_root) {
        char rpath[600];
        coa_path_join(rpath, sizeof(rpath), ctx->state_root, "routes.json");
        coa_router_save_file(ctx->router, rpath);
    }
    return 0;
}

/* POST /v1/routes/policy {"policy":"cost|latency|round_robin|capability:<tag>"} */
static int h_route_policy(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    cJSON *p = root ? cJSON_GetObjectItemCaseSensitive(root, "policy") : NULL;
    if (!p || !cJSON_IsString(p) || !p->valuestring) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp,
            "{\"error\":\"need 'policy': cost|latency|round_robin|capability:<tag>\"}");
        return 0;
    }
    int rc = coa_router_set_policy(ctx->router, p->valuestring);
    cJSON_Delete(root);
    if (rc != 0) {
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"unknown policy\"}");
        return 0;
    }
    coa_http_resp_appendf(resp, "{\"ok\":true,\"policy\":\"%s\"}",
                         coa_router_policy(ctx->router));
    return 0;
}

static int h_route_delete(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *name = req->path + strlen("/v1/routes/");
    int removed = ctx->router ? coa_router_remove(ctx->router, name) : 0;
    char *s = ctx->router ? coa_router_json(ctx->router) : coa_strdup("[]");
    coa_http_resp_appendf(resp, "{\"removed\":%s,\"routes\":", removed ? "true" : "false");
    coa_http_resp_append(resp, s ? s : "[]");
    coa_http_resp_append(resp, "}");
    free(s);
    /* persist so configured routes survive restart */
    if (ctx->router && ctx->state_root) {
        char rpath[600];
        coa_path_join(rpath, sizeof(rpath), ctx->state_root, "routes.json");
        coa_router_save_file(ctx->router, rpath);
    }
    return 0;
}

static int h_config_llm_get(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    (void)req;
    /* active LLM as persisted in config (set by coa_set_llm / env / defaults) */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "provider",
        coa_config_get_str(ctx->config, "llm.provider", "mock"));
    cJSON_AddStringToObject(o, "model",
        coa_config_get_str(ctx->config, "llm.model", ""));
    cJSON_AddStringToObject(o, "base_url",
        coa_config_get_str(ctx->config, "llm.base_url", ""));
    const char *ak = coa_config_get_str(ctx->config, "llm.api_key", NULL);
    cJSON_AddBoolToObject(o, "api_key_set", ak && *ak);
    char *s = cJSON_PrintUnformatted(o);
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
    return 0;
}

static int h_config_llm(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"body must be a JSON object\"}");
        return 0;
    }
    const char *provider = json_str(root, "provider");
    if (!provider || !*provider) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need 'provider' string\"}");
        return 0;
    }
    const char *base_url = json_str(root, "base_url");
    const char *model = json_str(root, "model");
    const char *api_key = json_str(root, "api_key");
    /* coa_set_llm dups every value; keep the JSON alive until after the call
     * so the pointers above stay valid (they live inside `root`) */
    int rc = coa_set_llm(ctx, provider, base_url, model, api_key);
    cJSON_Delete(root);
    if (rc != 0) {
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"unknown provider (mock|openai|anthropic)\"}");
        return 0;
    }
    char *s = ctx->router ? coa_router_json(ctx->router) : coa_strdup("[]");
    coa_http_resp_appendf(resp, "{\"ok\":true,\"routes\":");
    coa_http_resp_append(resp, s ? s : "[]");
    coa_http_resp_append(resp, "}");
    free(s);
    return 0;
}

/* Snapshot capture size limit (bytes; 0 = unlimited). UI-configurable
 * override of the built-in 64MB default / COA_SNAPSHOT_MAX_FILE env. */
static int h_config_snapshot_get(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    (void)req;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "max_file",
        (double)(ctx->snapshot ? coa_snapshot_get_max_file(ctx->snapshot) : -1));
    char *s = cJSON_PrintUnformatted(o);
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
    return 0;
}

static int h_config_snapshot(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    cJSON *mf = root ? cJSON_GetObjectItemCaseSensitive(root, "max_file") : NULL;
    if (!mf || !cJSON_IsNumber(mf) || mf->valuedouble < 0.0) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp,
            "{\"error\":\"need 'max_file' number >= 0 (bytes; 0 = unlimited)\"}");
        return 0;
    }
    long long v = (long long)mf->valuedouble;
    cJSON_Delete(root);
    if (ctx->snapshot) coa_snapshot_set_max_file(ctx->snapshot, v);
    coa_config_set_int(ctx->config, "snapshot.max_file", v);
    if (ctx->state_root) {
        char cfgfile[600];
        coa_path_join(cfgfile, sizeof(cfgfile), ctx->state_root, "cognitive-os-agent.json");
        coa_config_save_file(ctx->config, cfgfile);
    }
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

/* Validate that a given provider/base_url/model/api_key actually answers a chat
 * request. Creates a throwaway LLM instance (never persisted, never made active)
 * and returns {ok, reply|error}. Lets the UI prove a config works before saving. */
static int h_config_llm_test(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"body must be a JSON object\"}");
        return 0;
    }
    const char *provider = json_str(root, "provider");
    if (!provider || !*provider) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need 'provider' string\"}");
        return 0;
    }
    coa_llm *nl = coa_llm_create(provider, json_str(root, "base_url"),
                               json_str(root, "api_key"), json_str(root, "model"));
    if (!nl) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"ok\":false,\"error\":\"unknown provider (mock|openai|anthropic)\"}");
        return 0;
    }
    coa_llm_message msgs[2] = {
        {"system", "You are a concise assistant. Reply in at most a few words."},
        {"user",   "Reply with exactly the word: ok"}
    };
    coa_llm_request lreq = {0};
    lreq.messages = msgs;
    lreq.num_messages = 2;
    lreq.temperature = 0.2;
    lreq.max_tokens = 64;
    coa_llm_response lr = {0};
    int rc = coa_llm_chat(nl, &lreq, &lr);
    cJSON *o = cJSON_CreateObject();
    int ok = (rc == 0 && lr.content && *lr.content);
    cJSON_AddBoolToObject(o, "ok", ok);
    if (lr.content) cJSON_AddStringToObject(o, "reply", lr.content);
    else if (lr.error) cJSON_AddStringToObject(o, "error", lr.error);
    else cJSON_AddStringToObject(o, "error", "no response from provider (check base_url / model / api_key / network)");
    char *s = cJSON_PrintUnformatted(o);
    coa_http_resp_json(resp, s ? s : "{\"ok\":false}");
    free(s);
    cJSON_Delete(o);
    free(lr.content);
    free(lr.error);
    coa_llm_destroy(nl);
    cJSON_Delete(root);
    return 0;
}

static int h_usage(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->usage ? coa_usage_json(ctx->usage) : coa_strdup("{}");
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_plugins(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->registry ? coa_plugin_registry_json(ctx->registry) : coa_strdup("{}");
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_plugin_generate(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
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
        coa_http_resp_json(resp, "{\"ok\":false,\"error\":\"need 'description' string\"}");
        return 0;
    }
    char *s = coa_plugin_generate(ctx, description);
    coa_http_resp_json(resp, s ? s : "{\"ok\":false,\"error\":\"pipeline failed\"}");
    free(s);
    return 0;
}

/* POST /v1/plugins/native/load — load a native shared-library plugin
 * (.dll/.so) and probe its entry symbol. Body: {"path": "...",
 * "entry": "coa_plugin_main" (optional, defaults to coa_plugin_main)}.
 * The library is unloaded after the probe; tests exercise the error paths. */
static int h_plugin_native_load(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    cJSON *pj = root ? cJSON_GetObjectItemCaseSensitive(root, "path") : NULL;
    cJSON *ej = root ? cJSON_GetObjectItemCaseSensitive(root, "entry") : NULL;
    const char *path = (pj && cJSON_IsString(pj)) ? pj->valuestring : NULL;
    const char *entry = (ej && cJSON_IsString(ej) && *ej->valuestring)
                            ? ej->valuestring : "coa_plugin_main";
    if (root) cJSON_Delete(root);
    if (!path || !*path) {
        resp->status = 400;
        coa_http_resp_json(resp, "{\"ok\":false,\"error\":\"need 'path' string\"}");
        return 0;
    }
    coa_plugin *p = coa_plugin_load(path);
    if (!p) {
        resp->status = 400;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", 0);
        cJSON_AddStringToObject(e, "error", coa_plugin_error());
        char *s = cJSON_PrintUnformatted(e);
        cJSON_Delete(e);
        coa_http_resp_json(resp, s ? s : "{\"ok\":false,\"error\":\"load failed\"}");
        free(s);
        return 0;
    }
    void *sym = coa_plugin_symbol(p, entry);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddStringToObject(o, "path", path);
    cJSON_AddStringToObject(o, "entry", entry);
    cJSON_AddBoolToObject(o, "entry_found", sym ? 1 : 0);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    coa_plugin_unload(p);
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_skills(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->skills ? coa_skill_list_json(ctx->skills) : coa_strdup("[]");
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_skill_run(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
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
        coa_http_resp_json(resp, "{\"error\":\"missing 'name' string\"}");
        return 0;
    }
    coa_skill_result *r = ctx->skills
        ? coa_skill_execute(ctx->skills, name, args, ctx->workspace, 10000) : NULL;
    cJSON_Delete(root);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", r ? (r->ok ? 1 : 0) : 0);
    cJSON_AddStringToObject(o, "output", (r && r->output) ? r->output : "");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (r) coa_skill_result_free(r);
    coa_http_resp_json(resp, s ? s : "{}");
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
    {"hello_world", "打印问候语",                    "shell",   "echo hello from cognitive-os-agent"},
    {"list_dir",    "列出当前目录文件",              "shell",   "ls -la"},
    {"sys_info",    "显示内核/系统信息",             "shell",   "uname -a"},
    {"disk_usage",  "显示磁盘占用",                  "shell",   "df -h"},
    {"py_now",      "Python 打印当前时间",           "python",  "import datetime; print(datetime.datetime.now())"},
    {"greet",       "参数化问候（args: {\"who\":\"名字\"}）", "shell",  "echo hello {{who}}"},
    {"py_sum",      "Python 求和（args: {\"a\":1,\"b\":2}）", "python", "print({{a}} + {{b}})"},
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
static cJSON *market_fetch_root(coa_ctx *ctx, const char *path) {
    if (!ctx->market_url || !*ctx->market_url) return NULL;
    char *body = coa_market_fetch(ctx->market_url, path, 4000);
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

/* GitHub 热门应用（可安装为 skill 的开源工具/仓库，附仓库链接）。
 * winget_id: Windows 下一键安装用的 winget 包 ID（"" = 未收录）。 */
static const struct { const char *name, *desc, *repo, *kind, *winget_id; } GH_SKILLS[] = {
    { "jq",      "命令行 JSON 处理工具（解析/转换 JSON）",       "https://github.com/jqlang/jq",          "shell", "jqlang.jq" },
    { "ripgrep", "极速递归正则搜索（rg）",                       "https://github.com/BurntSushi/ripgrep", "shell", "BurntSushi.ripgrep.MSVC" },
    { "yt-dlp",  "视频/音频下载器（支持大量站点）",              "https://github.com/yt-dlp/yt-dlp",      "shell", "yt-dlp.yt-dlp" },
    { "pandoc",  "万能文档格式转换（markdown/HTML/PDF…）",       "https://github.com/jgm/pandoc",         "shell", "JohnMacFarlane.Pandoc" },
    { "ffmpeg",  "音视频处理工具箱",                             "https://github.com/FFmpeg/FFmpeg",      "shell", "Gyan.FFmpeg" },
    { "gh",      "GitHub 官方命令行（Issue/PR/Release）",        "https://github.com/cli/cli",            "shell", "GitHub.cli" },
    { "fd",      "更友好的 find 替代",                           "https://github.com/sharkdp/fd",         "shell", "sharkdp.fd" },
    { "bat",     "带语法高亮的 cat 替代",                        "https://github.com/sharkdp/bat",        "shell", "sharkdp.bat" },
};

static int h_skills_market(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
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
            ctx->skills && coa_skill_find(ctx->skills, t->name) != NULL);
        cJSON_AddItemToArray(tmpl, o);
    }
    cJSON_AddItemToObject(root, "templates", tmpl);
    /* GitHub 热门应用（可安装为 skill 的开源工具/仓库，附仓库链接） */
    cJSON *gh = cJSON_CreateArray();
    for (size_t i = 0; i < sizeof(GH_SKILLS) / sizeof(GH_SKILLS[0]); i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", GH_SKILLS[i].name);
        cJSON_AddStringToObject(o, "description", GH_SKILLS[i].desc);
        cJSON_AddStringToObject(o, "repo", GH_SKILLS[i].repo);
        cJSON_AddStringToObject(o, "kind", GH_SKILLS[i].kind);
        cJSON_AddStringToObject(o, "winget_id", GH_SKILLS[i].winget_id);
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

    char *mine = ctx->skills ? coa_skill_list_json(ctx->skills) : NULL;
    cJSON *inst = mine ? cJSON_Parse(mine) : NULL;
    free(mine);
    cJSON_AddItemToObject(root, "installed", inst ? inst : cJSON_CreateArray());
    char *s = cJSON_PrintUnformatted(root);
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    cJSON_Delete(root);
    return 0;
}

/* One-click install of a GitHub 热门应用 native tool via winget. The install
 * runs DETACHED (winget can take minutes) — the single-threaded HTTP server
 * must never block inside a handler. */
static int h_gh_install(const coa_http_request *req, coa_http_response *resp, void *ud) {
    (void)ud;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *name = json_str(root, "name");
    char namebuf[128] = "";
    if (name && *name) snprintf(namebuf, sizeof(namebuf), "%s", name);
    cJSON_Delete(root);
    name = namebuf;
    const char *winget = NULL;
    if (name && *name) {
        for (size_t i = 0; i < sizeof(GH_SKILLS) / sizeof(GH_SKILLS[0]); i++)
            if (strcmp(GH_SKILLS[i].name, name) == 0) { winget = GH_SKILLS[i].winget_id; break; }
    }
    if (!winget || !*winget) {
        resp->status = 404;
        coa_http_resp_json(resp, "{\"error\":\"unknown tool or no winget package\"}");
        return 0;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "winget install --id %s -e --silent --accept-package-agreements "
             "--accept-source-agreements --disable-interactivity", winget);
    if (coa_proc_spawn_detached(cmd) != 0) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"failed to start winget (is it installed?)\"}");
        return 0;
    }
    coa_http_resp_appendf(resp,
        "{\"ok\":true,\"started\":true,\"tool\":\"%s\",\"winget_id\":\"%s\","
        "\"hint\":\"后台安装已启动，稍后在 shell 里运行该命令验证\"}",
        name, winget);
    return 0;
}

static int h_skills_publish(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *name = json_str(root, "name");
    const char *kind = json_str(root, "kind");
    if (!name || !*name) {
        if (root) cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need 'name' string\"}");
        return 0;
    }
    coa_skill sk; memset(&sk, 0, sizeof(sk));
    sk.name = name;
    sk.description = json_str(root, "description") ? json_str(root, "description") : "";
    sk.kind = (kind && *kind) ? kind : "shell";
    sk.body = json_str(root, "body") ? json_str(root, "body") : "";
    /* Upsert semantics: the market "install" button re-publishes templates,
     * so an existing skill with the same name is updated, not rejected. */
    int rc = ctx->skills ? coa_skill_register_ex(ctx->skills, &sk, 1) : -1;
    if (rc != 0) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"register failed (invalid name or kind)\"}");
        return 0;
    }
    if (ctx->state_root) coa_skill_registry_persist(ctx->skills, ctx->state_root);
    /* best-effort push to a networked marketplace */
    int pushed = 0;
    if (ctx->market_url && *ctx->market_url && root) {
        char *payload = cJSON_PrintUnformatted(root);
        if (payload) {
            pushed = coa_market_publish(ctx->market_url, "/v1/skills/publish", payload, 4000) == 0;
            free(payload);
        }
    }
    char *s = ctx->skills ? coa_skill_list_json(ctx->skills) : coa_strdup("[]");
    coa_http_resp_appendf(resp, "{\"ok\":true,\"pushed_to_market\":%s,\"skills\":",
                         pushed ? "true" : "false");
    coa_http_resp_append(resp, s ? s : "[]");
    coa_http_resp_append(resp, "}");
    free(s);
    cJSON_Delete(root);
    return 0;
}

static int h_skill_delete(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *name = req->path + strlen("/v1/skills/");
    int rc = ctx->skills ? coa_skill_unregister(ctx->skills, name) : -1;
    if (rc != 0) {
        resp->status = 404;
        coa_http_resp_json(resp, "{\"error\":\"skill not found\"}");
        return 0;
    }
    if (ctx->state_root) coa_skill_registry_persist(ctx->skills, ctx->state_root);
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

/* ---- Plugin marketplace (publish user-built standardized plugins) ---- */

static int h_plugins_market(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *reg = ctx->registry ? coa_plugin_registry_json(ctx->registry) : coa_strdup("{}");
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
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    cJSON_Delete(root);
    return 0;
}

static int h_plugins_publish(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
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
        coa_http_resp_json(resp, "{\"ok\":false,\"error\":\"need 'name' and 'body'\"}");
        return 0;
    }
    /* make it runnable as a skill */
    coa_skill sk; memset(&sk, 0, sizeof(sk));
    sk.name = name;
    sk.description = json_str(root, "description") ? json_str(root, "description") : "";
    sk.kind = (kind && *kind) ? kind : "shell";
    sk.body = body;
    coa_skill_register(ctx->skills, &sk); /* best-effort; skip if duplicate */

    char sig[17]; fnv1a_hex(body, sig);
    coa_plugin_meta m; memset(&m, 0, sizeof(m));
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
            m.caps[i] = (ci && cJSON_IsString(ci)) ? coa_strdup(ci->valuestring) : coa_strdup("");
        }
    }
    int rc = ctx->registry ? coa_plugin_registry_register(ctx->registry, &m) : -1;
    for (size_t i = 0; i < m.n_caps; i++) free(m.caps[i]);
    free(m.caps);
    if (rc != 0) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"ok\":false,\"error\":\"publish failed (duplicate version?)\"}");
        return 0;
    }
    if (ctx->state_root) {
        coa_plugin_registry_persist(ctx->registry, ctx->state_root);
        coa_skill_registry_persist(ctx->skills, ctx->state_root);
    }
    /* best-effort push to a networked marketplace */
    int pushed = 0;
    if (ctx->market_url && *ctx->market_url && root) {
        char *payload = cJSON_PrintUnformatted(root);
        if (payload) {
            pushed = coa_market_publish(ctx->market_url, "/v1/plugins/publish", payload, 4000) == 0;
            free(payload);
        }
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddBoolToObject(o, "pushed_to_market", pushed);
    char *s = cJSON_PrintUnformatted(o);
    coa_http_resp_json(resp, s ? s : "{\"ok\":true}");
    free(s);
    cJSON_Delete(o);
    cJSON_Delete(root);
    return 0;
}

static int h_plugin_market_delete(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *name = req->path + strlen("/v1/plugins/market/");
    int rc = ctx->registry ? coa_plugin_registry_unregister(ctx->registry, name) : -1;
    if (rc != 0) {
        resp->status = 404;
        coa_http_resp_json(resp, "{\"error\":\"plugin not found\"}");
        return 0;
    }
    if (ctx->skills) coa_skill_unregister(ctx->skills, name);
    if (ctx->state_root) {
        coa_plugin_registry_persist(ctx->registry, ctx->state_root);
        coa_skill_registry_persist(ctx->skills, ctx->state_root);
    }
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

static int h_mcp(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->mcp ? coa_mcp_manager_json(ctx->mcp) : coa_strdup("[]");
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_mcp_add(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    coa_mcp_conn c;
    memset(&c, 0, sizeof(c));
    int ok_body = 0;
    if (root && cJSON_IsObject(root)) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *tr = cJSON_GetObjectItemCaseSensitive(root, "transport");
        cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "url");
        cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "token");
        cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
        cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "args");
        if (n && cJSON_IsString(n)) c.name = n->valuestring;
        if (tr && cJSON_IsString(tr)) c.transport = tr->valuestring;
        if (u && cJSON_IsString(u)) c.url = u->valuestring;
        if (t && cJSON_IsString(t)) c.token = t->valuestring;
        if (cmd && cJSON_IsString(cmd)) c.command = cmd->valuestring;
        if (a && cJSON_IsString(a)) c.args_csv = a->valuestring;
        ok_body = c.name && *c.name;
    }
    if (root) cJSON_Delete(root);
    int rc = ok_body ? (ctx->mcp ? coa_mcp_manager_add_ex(ctx->mcp, &c) : -1) : -1;
    if (rc != 0) {
        resp->status = 400;
        coa_http_resp_json(resp,
            "{\"error\":\"add failed: need 'name' plus 'url' (http) or "
            "'command' (stdio), and a valid transport\"}");
        return 0;
    }
    /* re-discover tools for the (possibly new) server and persist */
    if (ctx->mcp) {
        if (ctx->tools) coa_mcp_manager_sync_tools(ctx->mcp, ctx->tools);
        coa_mcp_manager_persist(ctx->mcp, ctx->state_root);
    }
    char *s = ctx->mcp ? coa_mcp_manager_json(ctx->mcp) : coa_strdup("[]");
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

/* Re-discover tools on every connection (manual refresh). */
static int h_mcp_sync(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    int n = (ctx->mcp && ctx->tools) ? coa_mcp_manager_sync_tools(ctx->mcp, ctx->tools) : -1;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "registered", n);
    cJSON_AddBoolToObject(o, "ok", n >= 0);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_mcp_delete(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *name = req->path + strlen("/v1/mcp/");
    int rc = ctx->mcp ? coa_mcp_manager_remove(ctx->mcp, name) : -1;
    if (rc == 0 && ctx->mcp) coa_mcp_manager_persist(ctx->mcp, ctx->state_root);
    coa_http_resp_appendf(resp, "{\"removed\":%s}", rc == 0 ? "true" : "false");
    return 0;
}

static int h_cluster(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->cluster ? coa_cluster_json(ctx->cluster) : coa_strdup("[]");
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

/* POST /v1/cluster/join — register a node {id, host, port, role, caps}.
 * Capability tags are comma-separated; the node must heartbeat to stay "up". */
static int h_cluster_join(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    cJSON *jid = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
    cJSON *jhost = root ? cJSON_GetObjectItemCaseSensitive(root, "host") : NULL;
    cJSON *jport = root ? cJSON_GetObjectItemCaseSensitive(root, "port") : NULL;
    cJSON *jrole = root ? cJSON_GetObjectItemCaseSensitive(root, "role") : NULL;
    cJSON *jcaps = root ? cJSON_GetObjectItemCaseSensitive(root, "caps") : NULL;
    if (!cJSON_IsString(jid) || !*jid->valuestring ||
        !cJSON_IsString(jhost) || !*jhost->valuestring ||
        !cJSON_IsNumber(jport)) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need 'id', 'host' and numeric 'port'\"}");
        return 0;
    }
    int rc = coa_cluster_upsert_ex(ctx->cluster, jid->valuestring, jhost->valuestring,
                                  (uint16_t)jport->valuedouble,
                                  cJSON_IsString(jrole) ? jrole->valuestring : NULL,
                                  cJSON_IsString(jcaps) ? jcaps->valuestring : NULL);
    cJSON_Delete(root);
    if (rc != 0) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"upsert failed\"}");
        return 0;
    }
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

/* POST /v1/cluster/heartbeat {id} — refresh a node's liveness. */
static int h_cluster_heartbeat(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    cJSON *jid = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
    if (!cJSON_IsString(jid) || !*jid->valuestring) {
        cJSON_Delete(root);
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need 'id'\"}");
        return 0;
    }
    int rc = coa_cluster_heartbeat(ctx->cluster, jid->valuestring);
    cJSON_Delete(root);
    if (rc != 0) {
        resp->status = 404;
        coa_http_resp_json(resp, "{\"error\":\"unknown node\"}");
        return 0;
    }
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

/* DELETE /v1/cluster/nodes/<id> — leave the cluster. */
static int h_cluster_leave(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    const char *id = req->path + strlen("/v1/cluster/nodes/");
    if (!*id) {
        resp->status = 400;
        coa_http_resp_json(resp, "{\"error\":\"need node id in path\"}");
        return 0;
    }
    if (coa_cluster_remove(ctx->cluster, id) != 0) {
        resp->status = 404;
        coa_http_resp_json(resp, "{\"error\":\"unknown node\"}");
        return 0;
    }
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

/* ================= catalogs (MCP plaza + free models) ================= */

/* GET /v1/market/status — report networked marketplace configuration + reachability. */
static int h_market_status(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    int configured = (ctx->market_url && *ctx->market_url) ? 1 : 0;
    int online = 0;
    if (configured) {
        char *body = coa_market_fetch(ctx->market_url, "/v1/market/ping", 3000);
        online = body ? 1 : 0;
        free(body);
    }
    coa_http_resp_appendf(resp,
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
    coa_socket *s = coa_sock_connect(host, (uint16_t)port, 300);
    if (!s)
        return 0;
    char req[320];
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    int ok = 0;
    if (coa_sock_send(s, req, (int)strlen(req)) > 0 &&
        coa_sock_wait_readable(s, 300) > 0) {
        char buf[256];
        int n = coa_sock_recv(s, buf, (int)sizeof buf - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (strncmp(buf, "HTTP/", 5) == 0 && strstr(buf, " 200 "))
                ok = 1;
        }
    }
    coa_sock_close(s);
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
    coa_proc_result *r = coa_proc_run(cmd, 4000);
    int found = r && r->exit_code == 0;
    coa_proc_result_free(r);
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
            if (coa_fs_exists(p)) { snprintf(out, cap, "\"%s\" serve", p); return 1; }
        }
        const char *pf = getenv("ProgramFiles");
        if (pf) {
            snprintf(p, sizeof p, "%s\\Ollama\\ollama.exe", pf);
            if (coa_fs_exists(p)) { snprintf(out, cap, "\"%s\" serve", p); return 1; }
        }
    }
#endif
    return 0;
}

/* Base URL of the llama.cpp server to probe. The default (8081) deliberately
 * avoids port 8080 where this HTTP server itself listens: the bundled server is
 * single-threaded, so probing our own port would wait on a request we can never
 * serve — a self-deadlock that freezes every other API call. */
static void llamacpp_probe_url(const coa_config *cfg, char *out, size_t cap) {
    long port = (long)coa_config_get_int(cfg, "local.llamacpp_port", 8081);
    snprintf(out, cap, "http://127.0.0.1:%ld", port);
}

/* GET /v1/local/status — report local free runtimes (Ollama / llama.cpp).
 * Probing costs real I/O (and on some Windows boxes connecting to the
 * Ollama/llama.cpp ports is silently dropped, costing the full timeout).
 * Because the bundled HTTP server is single-threaded, we cache the result for
 * a few seconds so page loads / UI polling don't re-pay it every request. */
static char *g_local_status_cache = NULL;
static int64_t g_local_status_at = 0;

static int h_local_status(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    int64_t now = coa_time_now_ms();
    if (g_local_status_cache && now - g_local_status_at < 3000) {
        coa_http_resp_append(resp, g_local_status_cache);
        return 0;
    }
    int o_running = local_probe("http://127.0.0.1:11434", "/api/version");
    char lp_url[128];
    llamacpp_probe_url(ctx->config, lp_url, sizeof lp_url);
    int l_running = local_probe(lp_url, "/v1/models");
    char cmdbuf[512];
    int o_installed = ollama_start_cmd(cmdbuf, sizeof cmdbuf);
    coa_http_resp_appendf(resp,
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
static int h_local_start(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *engine = root ? json_str(root, "engine") : NULL;
    /* engine borrows into the cJSON tree — copy before freeing it */
    char engine_buf[64];
    if (engine && *engine) {
        snprintf(engine_buf, sizeof(engine_buf), "%s", engine);
        engine = engine_buf;
    } else {
        engine = "ollama";
    }
    if (root) cJSON_Delete(root);

    if (strcmp(engine, "ollama") == 0) {
        if (local_probe("http://127.0.0.1:11434", "/api/version")) {
            coa_http_resp_json(resp, "{\"ok\":true,\"engine\":\"ollama\",\"already_running\":true}");
            return 0;
        }
        char cmd[512];
        if (!ollama_start_cmd(cmd, sizeof cmd)) {
            coa_http_resp_json(resp,
                "{\"ok\":false,\"engine\":\"ollama\",\"error\":\"未找到 ollama，请先到 ollama.com 安装并确保它在 PATH\"}");
            return 0;
        }
        int rc = coa_proc_spawn_detached(cmd);
        if (rc != 0) {
            coa_http_resp_json(resp,
                "{\"ok\":false,\"engine\":\"ollama\",\"error\":\"启动失败（无法创建进程）\"}");
            return 0;
        }
        coa_http_resp_json(resp,
            "{\"ok\":true,\"engine\":\"ollama\",\"spawned\":true,"
            "\"note\":\"已启动 ollama serve，首次运行需拉取模型，请稍候刷新状态\"}");
        return 0;
    }
    if (strcmp(engine, "llamacpp") == 0 || strcmp(engine, "llama") == 0) {
        char lp_url[128];
        llamacpp_probe_url(ctx->config, lp_url, sizeof lp_url);
        if (local_probe(lp_url, "/v1/models")) {
            coa_http_resp_json(resp, "{\"ok\":true,\"engine\":\"llamacpp\",\"already_running\":true}");
            return 0;
        }
        const char *cmd = coa_config_get_str(ctx->config, "local.llamacpp_cmd", NULL);
        if (!cmd || !*cmd) {
            coa_http_resp_json(resp,
                "{\"ok\":false,\"engine\":\"llamacpp\","
                "\"error\":\"未配置启动命令，请在 config 中设置 local.llamacpp_cmd（如 server 可执行文件路径）\"}");
            return 0;
        }
        int rc = coa_proc_spawn_detached(cmd);
        if (rc != 0) {
            coa_http_resp_json(resp,
                "{\"ok\":false,\"engine\":\"llamacpp\",\"error\":\"启动失败（无法创建进程）\"}");
            return 0;
        }
        coa_http_resp_json(resp,
            "{\"ok\":true,\"engine\":\"llamacpp\",\"spawned\":true,"
            "\"note\":\"已启动，请稍候刷新状态\"}");
        return 0;
    }
    coa_http_resp_json(resp,
        "{\"ok\":false,\"engine\":\"unknown\",\"error\":\"unknown engine (ollama|llamacpp)\"}");
    return 0;
}

static int h_catalog_mcp(const coa_http_request *req, coa_http_response *resp, void *ud) {
    (void)req;
    (void)ud;
    char *s = coa_catalog_mcp_json();
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

static int h_catalog_models(const coa_http_request *req, coa_http_response *resp, void *ud) {
    (void)req;
    (void)ud;
    char *s = coa_catalog_models_json();
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

/* ================= IM (instant messaging) ================= */

/* Push a new IM message to every WebSocket client + record an experience. */
static void im_push(coa_ctx *ctx, int64_t session_id, int64_t msg_id,
                    const char *role, const char *sender, const char *content) {
    if (!ctx || !content) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "im.message");
    cJSON_AddNumberToObject(o, "session_id", (double)session_id);
    cJSON_AddNumberToObject(o, "id", (double)msg_id);
    cJSON_AddStringToObject(o, "role", role ? role : "user");
    if (sender && *sender) cJSON_AddStringToObject(o, "sender", sender);
    cJSON_AddStringToObject(o, "content", content);
    cJSON_AddNumberToObject(o, "ts_ms", (double)coa_time_now_ms());
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (js) {
        if (ctx->http) coa_http_server_ws_broadcast(ctx->http, js);
        free(js);
    }
    if (ctx->memory) {
        char buf[384];
        snprintf(buf, sizeof(buf), "im session %lld (%s%s%s)",
                 (long long)session_id, role ? role : "user",
                 (sender && *sender) ? " by " : "", (sender && *sender) ? sender : "");
        coa_memory_record_experience(ctx->memory, buf, content);
    }
}

/* Fire-and-forget forward a console-originated message to its linked external
 * channel (best-effort: drops the response). Inbound channel messages are
 * injected directly via channel_ingest and never reach here, so there is no
 * echo. */
static void im_forward_to_channel(coa_ctx *ctx, int64_t session_id,
                                  const char *content) {
    if (!ctx || !ctx->channels || !ctx->im || !content) return;
    const char *chn = coa_im_session_channel(ctx->im, session_id);
    if (!chn || !*chn) return;
    char *r = coa_im_channel_send(ctx->channels, chn, content);
    if (r) free(r);
}

/* Inbound WebSocket text protocol:
 *   {"type":"im.send","session_id":N,"content":"...","sender":"..."} -> IM message
 *   {"type":"im.ping"}                                  -> server replies pong
 */
static void on_ws_msg(const char *text, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
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
            int64_t id = coa_im_send_ex(ctx->im, (int64_t)sid->valuedouble, "user",
                                       content->valuestring, snd);
            if (id > 0) {
                im_push(ctx, (int64_t)sid->valuedouble, id, "user", snd, content->valuestring);
                im_forward_to_channel(ctx, (int64_t)sid->valuedouble, content->valuestring);
            }
        }
    } else if (strcmp(t, "im.ping") == 0) {
        if (ctx->http) coa_http_server_ws_broadcast(ctx->http, "{\"type\":\"pong\"}");
    }
    cJSON_Delete(root);
}

static int h_im_sessions(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->im ? coa_im_sessions_json(ctx->im) : coa_strdup("{}");
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

static int h_im_session_create(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
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
    if (!ctx->im) { if (root) cJSON_Delete(root); resp->status = 500; coa_http_resp_json(resp, "{\"error\":\"im disabled\"}"); return 0; }
    int64_t id = coa_im_create_session_ex(ctx->im, name ? name : "",
                                         kind ? kind : "direct", members, n_members);
    cJSON_Delete(root);
    if (id < 0) { resp->status = 500; coa_http_resp_json(resp, "{\"error\":\"create failed\"}"); return 0; }
    if (id > 0 && ctx->channels && channel && *channel)
        coa_im_session_set_channel(ctx->im, id, channel);
    coa_http_resp_appendf(resp, "{\"id\":%lld}", (long long)id);
    return 0;
}

/* Channel bridge: external messaging channels (feishu/wecom/generic/telegram). */
static int h_im_channels(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->channels) { resp->status = 500; coa_http_resp_json(resp, "{\"error\":\"channels disabled\"}"); return 0; }
    char *s = coa_im_channels_json(ctx->channels);
    coa_http_resp_json(resp, s ? s : "{\"channels\":[]}");
    free(s);
    return 0;
}

static int h_im_channel_add(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->channels) { resp->status = 500; coa_http_resp_json(resp, "{\"error\":\"channels disabled\"}"); return 0; }
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
        coa_im_channel ch;
        memset(&ch, 0, sizeof(ch));
        ch.name = (char *)name;
        ch.type = (char *)type;
        ch.endpoint = endpoint && *endpoint ? (char *)endpoint : NULL;
        ch.token = token && *token ? (char *)token : NULL;
        ch.target = target && *target ? (char *)target : NULL;
        ch.enabled = enabled;
        rc = coa_im_channel_register(ctx->channels, &ch);
    }
    if (root) cJSON_Delete(root);
    if (rc != 0) { resp->status = 400; coa_http_resp_json(resp, "{\"error\":\"need 'name' and 'type' (feishu|wecom|generic|telegram)\"}"); return 0; }
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

static int h_im_channel_remove(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->channels) { resp->status = 500; coa_http_resp_json(resp, "{\"error\":\"channels disabled\"}"); return 0; }
    const char *name = req->path + strlen("/v1/im/channels/");
    int rc = coa_im_channel_remove(ctx->channels, name);
    if (rc == 0) {
        /* unbind any session that pointed at this channel */
        if (ctx->im) {
            size_t n = 0; coa_im_session *ss = coa_im_list_sessions(ctx->im, &n);
            for (size_t i = 0; i < n; i++)
                if (ss[i].channel && strcmp(ss[i].channel, name) == 0)
                    coa_im_session_set_channel(ctx->im, ss[i].id, NULL);
            coa_im_sessions_free(ss, n);
        }
    }
    coa_http_resp_appendf(resp, "{\"removed\":%s}", rc == 0 ? "true" : "false");
    return 0;
}

static int h_im_channel_send(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->channels) { resp->status = 500; coa_http_resp_json(resp, "{\"error\":\"channels disabled\"}"); return 0; }
    const char *base = req->path + strlen("/v1/im/channels/");
    size_t blen = strlen(base);
    const char *send = "/send";
    if (blen > strlen(send) && strcmp(base + blen - strlen(send), send) == 0)
        blen -= strlen(send);
    char name[128];
    if (blen >= sizeof(name)) blen = sizeof(name) - 1;
    memcpy(name, base, blen);
    name[blen] = '\0';
    if (!*name) { resp->status = 404; coa_http_resp_json(resp, "{\"error\":\"missing channel name\"}"); return 0; }
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *text = NULL;
    if (root && cJSON_IsObject(root)) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (t && cJSON_IsString(t)) text = t->valuestring;
    }
    if (root) cJSON_Delete(root);
    if (!text || !*text) { resp->status = 400; coa_http_resp_json(resp, "{\"error\":\"need 'text' string\"}"); return 0; }
    char *r = coa_im_channel_send(ctx->channels, name, text);
    coa_http_resp_json(resp, r ? r : "{\"ok\":false,\"error\":\"channel not found\"}");
    free(r);
    return 0;
}

/* Handles GET/POST/DELETE on /v1/im/sessions/{id} and /v1/im/sessions/{id}/messages */
static int h_im_session_route(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->im) { resp->status = 500; coa_http_resp_json(resp, "{\"error\":\"im disabled\"}"); return 0; }
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
    if (!idbuf[0]) { resp->status = 404; coa_http_resp_json(resp, "{\"error\":\"missing session id\"}"); return 0; }
    int64_t session_id = atoll(idbuf);

    if (strcmp(req->method, "DELETE") == 0) {
        int ok = coa_im_delete_session(ctx->im, session_id);
        coa_http_resp_appendf(resp, "{\"deleted\":%s}", ok ? "true" : "false");
        return 0;
    }
    if (strcmp(req->method, "POST") == 0) {
        if (!is_messages) { resp->status = 404; coa_http_resp_json(resp, "{\"error\":\"POST expects .../messages\"}"); return 0; }
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
        if (!content || !*content) { resp->status = 400; coa_http_resp_json(resp, "{\"error\":\"need 'content' string\"}"); return 0; }
        if (!role || !*role) role = "user";
        int64_t id = coa_im_send_ex(ctx->im, session_id, role, content, sender);
        if (id < 0) { resp->status = 404; coa_http_resp_json(resp, "{\"error\":\"session not found\"}"); return 0; }
        im_push(ctx, session_id, id, role, sender, content);
        im_forward_to_channel(ctx, session_id, content);
        coa_http_resp_appendf(resp, "{\"id\":%lld,\"ok\":true}", (long long)id);
        return 0;
    }
    /* GET */
    size_t n = 0;
    coa_im_message *msgs = coa_im_messages(ctx->im, session_id, &n);
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
    coa_im_messages_free(msgs, n);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

/* GET /v1/im/search?q=term  — history search across all sessions */
static int h_im_search(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->im) { resp->status = 500; coa_http_resp_json(resp, "{\"error\":\"im disabled\"}"); return 0; }
    const char *q = strstr(req->query, "q=");
    const char *query = q ? q + 2 : "";
    char *s = coa_im_search(ctx->im, query, 200);
    coa_http_resp_json(resp, s ? s : "[]");
    free(s);
    return 0;
}

/* ---------- Context layer: unified KV/Task/Agent state (/v1/state) ---------- */

/* GET /v1/state — the whole store */
static int h_state_all(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = ctx->state ? coa_state_store_json(ctx->state) : coa_strdup("{}");
    coa_http_resp_appendf(resp, "{\"ok\":true,\"count\":%d,\"state\":",
                         ctx->state ? coa_state_store_count(ctx->state) : 0);
    coa_http_resp_append(resp, s ? s : "{}");
    coa_http_resp_append(resp, "}");
    free(s);
    return 0;
}

/* GET /v1/state/<ns>[/<key>] — one namespace or one entry (borrowed value) */
static int h_state_get(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->state) { resp->status = 500; coa_http_resp_json(resp, "{\"error\":\"state store disabled\"}"); return 0; }
    const char *rest = req->path + strlen("/v1/state/");
    char ns[128], key[256];
    snprintf(ns, sizeof(ns), "%s", rest);
    char *slash = strchr(ns, '/');
    if (slash) { *slash = '\0'; snprintf(key, sizeof(key), "%s", slash + 1); }
    else key[0] = '\0';
    if (!*ns) { resp->status = 404; coa_http_resp_json(resp, "{\"error\":\"missing namespace\"}"); return 0; }
    if (*key) {
        const char *v = coa_state_store_get(ctx->state, ns, key);
        if (!v) { resp->status = 404; coa_http_resp_json(resp, "{\"error\":\"not found\"}"); return 0; }
        char *esc = cJSON_PrintUnformatted(cJSON_CreateString(v));
        coa_http_resp_appendf(resp, "{\"ok\":true,\"ns\":\"%s\",\"key\":\"%s\",\"value\":%s}",
                             ns, key, esc ? esc : "\"\"");
        free(esc);
        return 0;
    }
    /* whole namespace */
    cJSON *o = cJSON_CreateObject();
    cJSON *sub = cJSON_CreateObject();
    char *all = coa_state_store_json(ctx->state);
    cJSON *root = all ? cJSON_Parse(all) : NULL;
    free(all);
    cJSON *nsobj = root ? cJSON_GetObjectItemCaseSensitive(root, ns) : NULL;
    if (nsobj && cJSON_IsObject(nsobj)) {
        cJSON *it;
        cJSON_ArrayForEach(it, nsobj) {
            if (it->string && cJSON_IsString(it))
                cJSON_AddStringToObject(sub, it->string, it->valuestring);
        }
    }
    if (root) cJSON_Delete(root);
    cJSON_AddItemToObject(o, ns, sub);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    coa_http_resp_json(resp, s ? s : "{}");
    free(s);
    return 0;
}

/* PUT /v1/state/<ns>/<key> — body is the raw value, or {"value":"..."} */
static int h_state_put(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->state) { resp->status = 500; coa_http_resp_json(resp, "{\"error\":\"state store disabled\"}"); return 0; }
    const char *rest = req->path + strlen("/v1/state/");
    char ns[128], key[256];
    snprintf(ns, sizeof(ns), "%s", rest);
    char *slash = strchr(ns, '/');
    if (!slash) { resp->status = 400; coa_http_resp_json(resp, "{\"error\":\"need <ns>/<key>\"}"); return 0; }
    *slash = '\0';
    snprintf(key, sizeof(key), "%s", slash + 1);
    if (!*ns || !*key) { resp->status = 400; coa_http_resp_json(resp, "{\"error\":\"need <ns>/<key>\"}"); return 0; }
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL; /* JSON body {"value":...}? */
    const char *val = (root && cJSON_IsObject(root)) ? json_str(root, "value") : NULL;
    if (!val) val = b ? b : "";
    coa_state_store_set(ctx->state, ns, key, val);
    if (root) cJSON_Delete(root);
    free(b);
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

/* DELETE /v1/state/<ns>/<key> */
static int h_state_del(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    if (!ctx->state) { resp->status = 500; coa_http_resp_json(resp, "{\"error\":\"state store disabled\"}"); return 0; }
    const char *rest = req->path + strlen("/v1/state/");
    char ns[128], key[256];
    snprintf(ns, sizeof(ns), "%s", rest);
    char *slash = strchr(ns, '/');
    if (!slash) { resp->status = 400; coa_http_resp_json(resp, "{\"error\":\"need <ns>/<key>\"}"); return 0; }
    *slash = '\0';
    snprintf(key, sizeof(key), "%s", slash + 1);
    int removed = *ns && *key && coa_state_store_get(ctx->state, ns, key) != NULL;
    coa_state_store_remove(ctx->state, ns, key);
    coa_http_resp_appendf(resp, "{\"ok\":true,\"removed\":%s}",
                         removed ? "true" : "false");
    return 0;
}

/* POST /v1/state/snapshot — full runtime state export to <state_root>/snapshot.json */
static int h_state_snapshot(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *p = (root && cJSON_IsObject(root)) ? json_str(root, "path") : NULL;
    char path[600];
    if (p && *p)
        snprintf(path, sizeof(path), "%s", p);
    else
        coa_path_join(path, sizeof(path), ctx->state_root, "snapshot.json");
    if (root) cJSON_Delete(root);
    if (coa_state_export(ctx, path) != 0) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"export failed\"}");
        return 0;
    }
    coa_http_resp_appendf(resp, "{\"ok\":true,\"path\":\"%s\"}", path);
    return 0;
}

/* POST /v1/state/restore — import <state_root>/snapshot.json back */
static int h_state_restore(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);
    const char *p = (root && cJSON_IsObject(root)) ? json_str(root, "path") : NULL;
    char path[600];
    if (p && *p)
        snprintf(path, sizeof(path), "%s", p);
    else
        coa_path_join(path, sizeof(path), ctx->state_root, "snapshot.json");
    if (root) cJSON_Delete(root);
    if (coa_state_import(ctx, path) != 0) {
        resp->status = 500;
        coa_http_resp_json(resp, "{\"error\":\"restore failed\"}");
        return 0;
    }
    coa_http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

/* GET /v1/memory/service — Memory Service interface: backend + per-type stats */
static int h_memory_service(const coa_http_request *req, coa_http_response *resp, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!authz_ok(ctx, req, resp)) return 0;
    char *s = NULL;
    if (ctx->memsvc) coa_memory_service_stats(ctx->memsvc, &s);
    coa_http_resp_appendf(resp, "{\"ok\":true,\"backend\":\"%s\",\"types\":",
                         ctx->memsvc ? coa_memory_service_backend(ctx->memsvc) : "none");
    coa_http_resp_append(resp, s ? s : "[]");
    coa_http_resp_append(resp, "}");
    free(s);
    return 0;
}

int coa_api_attach(coa_ctx *ctx) {
    if (!ctx || ctx->http_port == 0) return 0;
    if (!ctx->http) {
        ctx->http = coa_http_server_new_bind(ctx->http_bind, ctx->http_port);
        if (!ctx->http) return -1;
    }
    coa_http_server_route(ctx->http, "POST", "/v1/tasks", h_task_create, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/tasks/", h_task_get, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/tools", h_tools, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/memory", h_memory, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/blackboard", h_blackboard, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/blackboard", h_blackboard_put, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/agents", h_agents, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/agents", h_agent_add, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/agents/", h_agent_post, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/snapshots", h_snapshots, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/snapshots/rollback", h_snapshot_rollback, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/trace", h_trace, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/market/status", h_market_status, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/local/status", h_local_status, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/local/start", h_local_start, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/routes", h_routes, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/routes", h_route_add, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/routes/policy", h_route_policy, ctx);
    coa_http_server_route(ctx->http, "DELETE", "/v1/routes/", h_route_delete, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/config/llm", h_config_llm_get, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/config/llm", h_config_llm, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/config/llm/test", h_config_llm_test, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/config/snapshot", h_config_snapshot_get, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/config/snapshot", h_config_snapshot, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/chat", h_chat, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/chat/history", h_chat_history, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/policy/rules", h_policy_rules, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/policy/rules", h_policy_add, ctx);
    coa_http_server_route(ctx->http, "DELETE", "/v1/policy/rules/", h_policy_delete, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/orchestrate", h_orchestrate, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/flows", h_flow_run, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/flows/decompose", h_flow_decompose, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/hooks", h_hooks, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/hooks", h_hook_add, ctx);
    coa_http_server_route(ctx->http, "DELETE", "/v1/hooks/", h_hook_delete, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/upload", h_upload, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/uploads", h_uploads, ctx);
    coa_http_server_route(ctx->http, "DELETE", "/v1/uploads/", h_upload_delete, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/usage", h_usage, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/state", h_state_all, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/state/", h_state_get, ctx);
    coa_http_server_route(ctx->http, "PUT", "/v1/state/", h_state_put, ctx);
    coa_http_server_route(ctx->http, "DELETE", "/v1/state/", h_state_del, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/state/snapshot", h_state_snapshot, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/state/restore", h_state_restore, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/memory/service", h_memory_service, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/plugins", h_plugins, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/plugins/generate", h_plugin_generate, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/plugins/native/load", h_plugin_native_load, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/plugins/market", h_plugins_market, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/plugins/publish", h_plugins_publish, ctx);
    coa_http_server_route(ctx->http, "DELETE", "/v1/plugins/market/", h_plugin_market_delete, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/skills", h_skills, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/skills/run", h_skill_run, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/skills/market", h_skills_market, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/skills/publish", h_skills_publish, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/tools/gh-install", h_gh_install, NULL);
    coa_http_server_route(ctx->http, "DELETE", "/v1/skills/", h_skill_delete, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/mcp", h_mcp, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/mcp", h_mcp_add, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/mcp/sync", h_mcp_sync, ctx);
    coa_http_server_route(ctx->http, "DELETE", "/v1/mcp/", h_mcp_delete, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/cluster", h_cluster, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/cluster/join", h_cluster_join, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/cluster/heartbeat", h_cluster_heartbeat, ctx);
    coa_http_server_route(ctx->http, "DELETE", "/v1/cluster/nodes/", h_cluster_leave, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/im/sessions", h_im_sessions, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/im/sessions", h_im_session_create, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/im/search", h_im_search, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/im/channels", h_im_channels, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/im/channels", h_im_channel_add, ctx);
    coa_http_server_route(ctx->http, "DELETE", "/v1/im/channels/", h_im_channel_remove, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/im/channels/", h_im_channel_send, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/im/sessions/", h_im_session_route, ctx);
    coa_http_server_route(ctx->http, "POST", "/v1/im/sessions/", h_im_session_route, ctx);
    coa_http_server_route(ctx->http, "DELETE", "/v1/im/sessions/", h_im_session_route, ctx);
    coa_http_server_ws_route(ctx->http, "/ws", on_ws_msg, ctx);
    coa_http_server_route(ctx->http, "GET", "/metrics", h_metrics, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/catalog/mcp", h_catalog_mcp, ctx);
    coa_http_server_route(ctx->http, "GET", "/v1/catalog/models", h_catalog_models, ctx);
    coa_http_server_route(ctx->http, "GET", "/", h_index, ctx);
    coa_http_server_route(ctx->http, "GET", "/favicon.ico", h_favicon, ctx);
    return 0;
}
