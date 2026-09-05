/* anthropic.c — Claude Messages API adapter (streaming + non-streaming). */
#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/llm/sse.h"
#include "cognitive-os-agent/os/http.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/infra/logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define ANTHROPIC_DEFAULT_BASE "http://localhost:8000"
#define ANTHROPIC_PATH "/v1/messages"

typedef struct {
    char *base_url;
} anthropic_impl;

static anthropic_impl *impl_of(coa_llm *llm) { return (anthropic_impl *)llm->impl; }

static void anthropic_destroy(coa_llm *llm) {
    anthropic_impl *im = impl_of(llm);
    free(im->base_url);
    free(im);
    free(llm->base_url);
    free(llm->api_key);
    free(llm->model);
    free(llm);
}

/* Anthropic puts the system prompt at top level and only user/assistant in messages. */
static char *build_request_body(const coa_llm_request *req, const char *model, int stream) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model ? model : "claude-sonnet-4-6");
    cJSON_AddNumberToObject(root, "max_tokens", req->max_tokens > 0 ? req->max_tokens : 1024);
    if (req->temperature > 0) cJSON_AddNumberToObject(root, "temperature", req->temperature);
    if (stream) cJSON_AddBoolToObject(root, "stream", 1);

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    for (size_t i = 0; i < req->num_messages; i++) {
        if (strcmp(req->messages[i].role, "system") == 0) {
            cJSON_AddStringToObject(root, "system", req->messages[i].content);
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "role", req->messages[i].role);
        cJSON_AddStringToObject(o, "content", req->messages[i].content);
        cJSON_AddItemToArray(msgs, o);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static void set_error(coa_llm_response *resp, const char *msg) {
    resp->error = coa_strdup(msg);
}

static coa_strmap *anthropic_headers(coa_llm *llm) {
    if (!llm->api_key) return NULL;
    coa_strmap *hdrs = calloc(1, sizeof(coa_strmap));
    if (!hdrs) return NULL;
    coa_strmap_set(hdrs, "x-api-key", llm->api_key);
    coa_strmap_set(hdrs, "anthropic-version", "2023-06-01");
    return hdrs;
}

static int anthropic_chat(coa_llm *llm, const coa_llm_request *req, coa_llm_response *resp) {
    char *body = build_request_body(req, llm->model, 0);
    if (!body) { set_error(resp, "request build failed"); return -1; }
    coa_strmap *hdrs = anthropic_headers(llm);

    coa_http_response *r = coa_http_post(impl_of(llm)->base_url, ANTHROPIC_PATH, body,
                                       "application/json", hdrs, 60000);
    free(body);
    if (hdrs) { coa_strmap_free(hdrs); free(hdrs); }
    if (!r) { set_error(resp, "http request failed"); return -1; }
    if (r->status != 200) {
        char err[512];
        snprintf(err, sizeof(err), "anthropic http %d: %s", r->status,
                 r->body && r->body[0] ? r->body : "(empty)");
        set_error(resp, err);
        coa_http_response_free(r);
        return -1;
    }

    cJSON *root = cJSON_Parse(r->body);
    coa_http_response_free(r);
    if (!root) { set_error(resp, "anthropic: invalid JSON response"); return -1; }

    coa_strbuf sb;
    coa_strbuf_init(&sb);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    if (content && cJSON_IsArray(content)) {
        cJSON *it;
        cJSON_ArrayForEach(it, content) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(it, "text");
            if (text && cJSON_IsString(text)) coa_strbuf_append(&sb, text->valuestring);
        }
    }
    resp->content = coa_strbuf_detach(&sb);
    if (!resp->content || !*resp->content) {
        cJSON *err_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
        const char *em = err_obj && cJSON_IsObject(err_obj) ? err_obj->string : "no content in response";
        set_error(resp, em);
        free(resp->content);
        resp->content = NULL;
    }
    cJSON_Delete(root);
    return resp->error ? -1 : 0;
}

static int anthropic_stream(coa_llm *llm, const coa_llm_request *req, coa_llm_stream_cb cb, void *ud) {
    char *body = build_request_body(req, llm->model, 1);
    if (!body) return -1;
    coa_strmap *hdrs = anthropic_headers(llm);

    coa_sse *s = coa_sse_start(impl_of(llm)->base_url, ANTHROPIC_PATH, body,
                             "application/json", hdrs, 60000);
    free(body);
    if (hdrs) { coa_strmap_free(hdrs); free(hdrs); }
    if (!s) return -1;
    if (coa_sse_status(s) != 200) {
        coa_log_warn("anthropic stream: http status %d", coa_sse_status(s));
        coa_sse_close(s);
        return -1;
    }

    char line[32768];
    while (coa_sse_next(s, line, sizeof(line)) == 1) {
        if (llm->cancel) { coa_sse_close(s); return -1; }
        cJSON *root = cJSON_Parse(line);
        if (!root) continue;
        /* content_block_delta -> delta.text */
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
        cJSON *text = delta ? cJSON_GetObjectItemCaseSensitive(delta, "text") : NULL;
        if (text && cJSON_IsString(text) && text->valuestring)
            cb(text->valuestring, ud);
        cJSON_Delete(root);
    }
    coa_sse_close(s);
    return 0;
}

coa_llm *coa_anthropic_create(const char *base_url, const char *api_key, const char *model) {
    coa_llm *llm = calloc(1, sizeof(coa_llm));
    anthropic_impl *im = calloc(1, sizeof(anthropic_impl));
    if (!llm || !im) { free(llm); free(im); return NULL; }
    static const coa_llm_vtable vt = {anthropic_destroy, anthropic_chat, anthropic_stream};
    llm->vt = &vt;
    llm->provider = coa_strdup("anthropic");
    llm->base_url = coa_strdup(base_url && *base_url ? base_url : ANTHROPIC_DEFAULT_BASE);
    llm->api_key = api_key ? coa_strdup(api_key) : NULL;
    llm->model = coa_strdup(model ? model : "claude-sonnet-4-6");
    llm->impl = im;
    im->base_url = coa_strdup(llm->base_url);
    return llm;
}
