/* openai.c — OpenAI-compatible chat/completions adapter (also works with
 * Ollama, llama.cpp, vLLM, DeepSeek, Qwen-compatible gateways). */
#include "cagent/llm/llm.h"
#include "cagent/llm/sse.h"
#include "cagent/os/http.h"
#include "cagent/infra/util.h"
#include "cagent/infra/logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define OPENAI_DEFAULT_BASE "http://localhost:11434" /* Ollama OpenAI-compatible endpoint */
#define OPENAI_PATH "/v1/chat/completions"

typedef struct {
    char *base_url;
} openai_impl;

static openai_impl *impl_of(ca_llm *llm) { return (openai_impl *)llm->impl; }

static void openai_destroy(ca_llm *llm) {
    openai_impl *im = impl_of(llm);
    free(im->base_url);
    free(im);
    free(llm->base_url);
    free(llm->api_key);
    free(llm->model);
    free(llm);
}

static char *build_request_body(const ca_llm_request *req, const char *model, int stream) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model ? model : "default");
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    for (size_t i = 0; i < req->num_messages; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "role", req->messages[i].role);
        cJSON_AddStringToObject(o, "content", req->messages[i].content);
        cJSON_AddItemToArray(msgs, o);
    }
    cJSON_AddNumberToObject(root, "temperature", req->temperature);
    if (req->max_tokens > 0) cJSON_AddNumberToObject(root, "max_tokens", req->max_tokens);
    if (stream) cJSON_AddBoolToObject(root, "stream", 1);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static void set_error(ca_llm_response *resp, const char *msg) {
    resp->error = ca_strdup(msg);
}

static char *normalize_base(const char *base, const char **path_out) {
    /* The fixed OPENAI_PATH already starts with /v1; if the user supplied a
     * base_url that already ends in /v1 (e.g. https://api.deepseek.com/v1),
     * strip it to avoid a doubled /v1/v1/... path. A base ending in any other
     * /vN (e.g. Volcengine Ark Coding Plan "https://ark.../api/coding/v3")
     * keeps its own version, so only "/chat/completions" is appended. */
    *path_out = OPENAI_PATH;
    size_t n = base ? strlen(base) : 0;
    if (n >= 3 && strcmp(base + n - 3, "/v1") == 0) {
        char *s = ca_strdup(base);
        s[n - 3] = '\0';
        return s;
    }
    if (n >= 3 && base[n - 3] == '/' && base[n - 2] == 'v' &&
        base[n - 1] >= '0' && base[n - 1] <= '9') {
        char *s = ca_strdup(base);
        *path_out = "/chat/completions";
        return s;
    }
    return ca_strdup(base);
}

static int openai_chat(ca_llm *llm, const ca_llm_request *req, ca_llm_response *resp) {
    char *body = build_request_body(req, llm->model, 0);
    if (!body) { set_error(resp, "request build failed"); return -1; }

    const char *path;
    char *base = normalize_base(impl_of(llm)->base_url, &path);
    ca_strmap *hdrs = NULL;
    if (llm->api_key) {
        hdrs = (ca_strmap *)calloc(1, sizeof(ca_strmap));
        char auth[2048];
        snprintf(auth, sizeof(auth), "Bearer %s", llm->api_key);
        ca_strmap_set(hdrs, "Authorization", auth);
    }

    ca_http_response *r = ca_http_post(base, path, body, "application/json", hdrs, 60000);
    free(body);
    free(base);
    if (hdrs) { ca_strmap_free(hdrs); free(hdrs); }
    if (!r) { set_error(resp, "http request failed"); return -1; }

    if (r->status != 200) {
        char err[512];
        snprintf(err, sizeof(err), "openai http %d: %s", r->status,
                 r->body && r->body[0] ? r->body : "(empty)");
        set_error(resp, err);
        ca_http_response_free(r);
        return -1;
    }

    cJSON *root = cJSON_Parse(r->body);
    ca_http_response_free(r);
    if (!root) { set_error(resp, "openai: invalid JSON response"); return -1; }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *msg = choices && choices->child
        ? cJSON_GetObjectItemCaseSensitive(choices->child, "message") : NULL;
    cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
    if (content && cJSON_IsString(content)) {
        resp->content = ca_strdup(content->valuestring);
    } else {
        cJSON *err_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
        const char *em = err_obj && cJSON_IsObject(err_obj)
            ? (err_obj->valuestring ? err_obj->valuestring : "unknown") : "no content in response";
        set_error(resp, em);
    }
    cJSON_Delete(root);
    return resp->error ? -1 : 0;
}

static int openai_stream(ca_llm *llm, const ca_llm_request *req, ca_llm_stream_cb cb, void *ud) {
    char *body = build_request_body(req, llm->model, 1);
    if (!body) return -1;

    const char *path;
    char *base = normalize_base(impl_of(llm)->base_url, &path);
    ca_strmap *hdrs = NULL;
    if (llm->api_key) {
        hdrs = (ca_strmap *)calloc(1, sizeof(ca_strmap));
        char auth[2048];
        snprintf(auth, sizeof(auth), "Bearer %s", llm->api_key);
        ca_strmap_set(hdrs, "Authorization", auth);
    }

    ca_sse *s = ca_sse_start(base, path, body, "application/json", hdrs, 60000);
    free(body);
    free(base);
    if (hdrs) { ca_strmap_free(hdrs); free(hdrs); }
    if (!s) return -1;
    if (ca_sse_status(s) != 200) {
        ca_log_warn("openai stream: http status %d", ca_sse_status(s));
        ca_sse_close(s);
        return -1;
    }

    char line[16384];
    int rc = 0;
    while (ca_sse_next(s, line, sizeof(line)) == 1) {
        if (llm->cancel) { rc = -1; break; }
        cJSON *root = cJSON_Parse(line);
        if (!root) continue;
        cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        cJSON *ch = choices && choices->child ? choices->child : NULL;
        cJSON *delta = ch ? cJSON_GetObjectItemCaseSensitive(ch, "delta") : NULL;
        cJSON *content = delta ? cJSON_GetObjectItemCaseSensitive(delta, "content") : NULL;
        if (content && cJSON_IsString(content) && content->valuestring)
            cb(content->valuestring, ud);
        cJSON_Delete(root);
    }
    ca_sse_close(s);
    return rc;
}

ca_llm *ca_openai_create(const char *base_url, const char *api_key, const char *model) {
    ca_llm *llm = calloc(1, sizeof(ca_llm));
    openai_impl *im = calloc(1, sizeof(openai_impl));
    if (!llm || !im) { free(llm); free(im); return NULL; }
    static const ca_llm_vtable vt = {openai_destroy, openai_chat, openai_stream};
    llm->vt = &vt;
    llm->provider = ca_strdup("openai");
    llm->base_url = ca_strdup(base_url && *base_url ? base_url : OPENAI_DEFAULT_BASE);
    llm->api_key = api_key ? ca_strdup(api_key) : NULL;
    llm->model = ca_strdup(model ? model : "gpt-4o-mini");
    llm->impl = im;
    im->base_url = ca_strdup(llm->base_url);
    return llm;
}
