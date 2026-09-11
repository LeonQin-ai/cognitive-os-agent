/* openai.c — OpenAI-compatible chat/completions adapter (also works with
 * Ollama, llama.cpp, vLLM, DeepSeek, Qwen-compatible gateways). */
#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/llm/sse.h"
#include "cognitive-os-agent/os/http.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/infra/logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define OPENAI_DEFAULT_BASE "http://localhost:11434" /* Ollama OpenAI-compatible endpoint */
#define OPENAI_PATH "/v1/chat/completions"

typedef struct {
    char *base_url;
} openai_impl;

static openai_impl *impl_of(llm *llm) {
    return (openai_impl *)llm->impl;
}

static void openai_destroy(llm *llm) {
    openai_impl *im = impl_of(llm);
    free(im->base_url);
    free(im);
    free(llm->base_url);
    free(llm->api_key);
    free(llm->model);
    free(llm);
}

static char *build_request_body(const llm_request *req, const char *model, int stream) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model ? model : "default");
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    for (size_t i = 0; i < req->num_messages; i++) {
        const llm_message *m = &req->messages[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "role", m->role);
        /* Tool observations can carry non-UTF-8 bytes (binary files, PDF
         * extraction); providers reject such bodies outright (Ark
         * "InvalidParameter.NonUTF8Body"), so sanitize every text field. */
        char *content = m->content ? str_utf8_sanitize(m->content) : NULL;
        if (m->image_b64 && *m->image_b64) {
            /* multimodal message: content is a typed parts array */
            const char *mime = (m->image_mime && *m->image_mime) ? m->image_mime : "image/png";
            cJSON *parts = cJSON_AddArrayToObject(o, "content");
            cJSON *text = cJSON_CreateObject();
            cJSON_AddStringToObject(text, "type", "text");
            cJSON_AddStringToObject(text, "text", content ? content : "");
            cJSON_AddItemToArray(parts, text);
            cJSON *img = cJSON_CreateObject();
            cJSON_AddStringToObject(img, "type", "image_url");
            cJSON *iu = cJSON_CreateObject();
            /* url = "data:<mime>;base64," + payload (single allocation) */
            size_t ulen = strlen(mime) + strlen(m->image_b64) + 32;
            char *u = (char *)malloc(ulen);
            if (u) {
                snprintf(u, ulen, "data:%s;base64,%s", mime, m->image_b64);
                cJSON_AddStringToObject(iu, "url", u);
                free(u);
            }
            cJSON_AddItemToObject(img, "image_url", iu);
            cJSON_AddItemToArray(parts, img);
        } else {
            cJSON_AddStringToObject(o, "content", content ? content : "");
        }
        free(content);
        cJSON_AddItemToArray(msgs, o);
    }
    cJSON_AddNumberToObject(root, "temperature", req->temperature);
    if (req->max_tokens > 0)
        cJSON_AddNumberToObject(root, "max_tokens", req->max_tokens);
    if (stream)
        cJSON_AddBoolToObject(root, "stream", 1);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static void set_error(llm_response *resp, const char *msg) {
    resp->error = xstrdup(msg);
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
        char *s = xstrdup(base);
        s[n - 3] = '\0';
        return s;
    }
    if (n >= 3 && base[n - 3] == '/' && base[n - 2] == 'v' && base[n - 1] >= '0' && base[n - 1] <= '9') {
        char *s = xstrdup(base);
        *path_out = "/chat/completions";
        return s;
    }
    return xstrdup(base);
}

static int openai_chat(llm *llm, const llm_request *req, llm_response *resp) {
    char *body = build_request_body(req, llm->model, 0);
    if (!body) {
        set_error(resp, "request build failed");
        return -1;
    }

    const char *path;
    char *base = normalize_base(impl_of(llm)->base_url, &path);
    strmap *hdrs = NULL;
    if (llm->api_key) {
        hdrs = (strmap *)calloc(1, sizeof(strmap));
        char auth[2048];
        snprintf(auth, sizeof(auth), "Bearer %s", llm->api_key);
        strmap_set(hdrs, "Authorization", auth);
    }

    http_response *r = http_post(base, path, body, "application/json", hdrs, llm_timeout_ms());
    free(body);
    free(base);
    if (hdrs) {
        strmap_free(hdrs);
        free(hdrs);
    }
    if (!r) {
        set_error(resp, "http request failed");
        return -1;
    }

    if (r->status != 200) {
        char err[512];
        snprintf(err, sizeof(err), "openai http %d: %s", r->status, r->body && r->body[0] ? r->body : "(empty)");
        set_error(resp, err);
        http_response_free(r);
        return -1;
    }

    cJSON *root = cJSON_Parse(r->body);
    http_response_free(r);
    if (!root) {
        set_error(resp, "openai: invalid JSON response");
        return -1;
    }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *msg = choices && choices->child ? cJSON_GetObjectItemCaseSensitive(choices->child, "message") : NULL;
    cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
    if (content && cJSON_IsString(content) && content->valuestring[0]) {
        resp->content = xstrdup(content->valuestring);
    } else {
        /* Reasoning models (GLM/DeepSeek-R1 style) can exhaust the token
         * budget while thinking and return an empty content with the
         * answer text left in reasoning_content — use it as a fallback
         * rather than surfacing "no content in response". */
        cJSON *reasoning = msg ? cJSON_GetObjectItemCaseSensitive(msg, "reasoning_content") : NULL;
        if (reasoning && cJSON_IsString(reasoning) && reasoning->valuestring[0]) {
            resp->content = xstrdup(reasoning->valuestring);
        } else {
            cJSON *err_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
            const char *em = err_obj && cJSON_IsObject(err_obj)
                                 ? (err_obj->valuestring ? err_obj->valuestring : "unknown")
                                 : "no content in response";
            set_error(resp, em);
        }
    }
    cJSON_Delete(root);
    return resp->error ? -1 : 0;
}

static int openai_stream(llm *llm, const llm_request *req, llm_stream_cb cb, void *ud) {
    char *body = build_request_body(req, llm->model, 1);
    if (!body)
        return -1;

    const char *path;
    char *base = normalize_base(impl_of(llm)->base_url, &path);
    strmap *hdrs = NULL;
    if (llm->api_key) {
        hdrs = (strmap *)calloc(1, sizeof(strmap));
        char auth[2048];
        snprintf(auth, sizeof(auth), "Bearer %s", llm->api_key);
        strmap_set(hdrs, "Authorization", auth);
    }

    sse *s = sse_start(base, path, body, "application/json", hdrs, llm_timeout_ms());
    free(body);
    free(base);
    if (hdrs) {
        strmap_free(hdrs);
        free(hdrs);
    }
    if (!s)
        return -1;
    if (sse_status(s) != 200) {
        log_warn("openai stream: http status %d", sse_status(s));
        sse_close(s);
        return -1;
    }

    char line[16384];
    int rc = 0;
    while (sse_next(s, line, sizeof(line)) == 1) {
        if (llm->cancel) {
            rc = -1;
            break;
        }
        cJSON *root = cJSON_Parse(line);
        if (!root)
            continue;
        cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        cJSON *ch = choices && choices->child ? choices->child : NULL;
        cJSON *delta = ch ? cJSON_GetObjectItemCaseSensitive(ch, "delta") : NULL;
        cJSON *content = delta ? cJSON_GetObjectItemCaseSensitive(delta, "content") : NULL;
        if (content && cJSON_IsString(content) && content->valuestring)
            cb(content->valuestring, ud);
        cJSON_Delete(root);
    }
    sse_close(s);
    return rc;
}

llm *openai_create(const char *base_url, const char *api_key, const char *model) {
    llm *llm = calloc(1, sizeof(*llm));
    openai_impl *im = calloc(1, sizeof(openai_impl));
    if (!llm || !im) {
        free(llm);
        free(im);
        return NULL;
    }
    static const llm_vtable vt = {openai_destroy, openai_chat, openai_stream};
    llm->vt = &vt;
    llm->provider = xstrdup("openai");
    llm->base_url = xstrdup(base_url && *base_url ? base_url : OPENAI_DEFAULT_BASE);
    llm->api_key = api_key ? xstrdup(api_key) : NULL;
    llm->model = xstrdup(model ? model : "gpt-4o-mini");
    llm->impl = im;
    im->base_url = xstrdup(llm->base_url);
    return llm;
}
