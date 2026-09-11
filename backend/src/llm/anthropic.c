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

static anthropic_impl *impl_of(llm *llm) {
    return (anthropic_impl *)llm->impl;
}

static void anthropic_destroy(llm *llm) {
    anthropic_impl *im = impl_of(llm);
    free(im->base_url);
    free(im);
    free(llm->base_url);
    free(llm->api_key);
    free(llm->model);
    free(llm);
}

/* Anthropic puts the system prompt at top level and only user/assistant in messages. */
static char *build_request_body(const llm_request *req, const char *model, int stream) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model ? model : "claude-sonnet-4-6");
    cJSON_AddNumberToObject(root, "max_tokens", req->max_tokens > 0 ? req->max_tokens : 1024);
    if (req->temperature > 0)
        cJSON_AddNumberToObject(root, "temperature", req->temperature);
    if (stream)
        cJSON_AddBoolToObject(root, "stream", 1);

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    for (size_t i = 0; i < req->num_messages; i++) {
        const llm_message *m = &req->messages[i];
        /* Tool observations can carry non-UTF-8 bytes (binary files, PDF
         * extraction); providers reject such bodies, so sanitize text. */
        char *content = m->content ? str_utf8_sanitize(m->content) : NULL;
        if (strcmp(m->role, "system") == 0) {
            cJSON_AddStringToObject(root, "system", content ? content : "");
            free(content);
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "role", m->role);
        if (m->image_b64 && *m->image_b64) {
            /* multimodal message: content is a typed blocks array */
            const char *mime = (m->image_mime && *m->image_mime) ? m->image_mime : "image/png";
            cJSON *blocks = cJSON_AddArrayToObject(o, "content");
            cJSON *text = cJSON_CreateObject();
            cJSON_AddStringToObject(text, "type", "text");
            cJSON_AddStringToObject(text, "text", content ? content : "");
            cJSON_AddItemToArray(blocks, text);
            cJSON *img = cJSON_CreateObject();
            cJSON_AddStringToObject(img, "type", "image");
            cJSON *src = cJSON_CreateObject();
            cJSON_AddStringToObject(src, "type", "base64");
            cJSON_AddStringToObject(src, "media_type", mime);
            cJSON_AddStringToObject(src, "data", m->image_b64);
            cJSON_AddItemToObject(img, "source", src);
            cJSON_AddItemToArray(blocks, img);
        } else {
            cJSON_AddStringToObject(o, "content", content ? content : "");
        }
        free(content);
        cJSON_AddItemToArray(msgs, o);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static void set_error(llm_response *resp, const char *msg) {
    resp->error = xstrdup(msg);
}

static strmap *anthropic_headers(llm *llm) {
    if (!llm->api_key)
        return NULL;
    strmap *hdrs = calloc(1, sizeof(strmap));
    if (!hdrs)
        return NULL;
    strmap_set(hdrs, "x-api-key", llm->api_key);
    strmap_set(hdrs, "anthropic-version", "2023-06-01");
    return hdrs;
}

static int anthropic_chat(llm *llm, const llm_request *req, llm_response *resp) {
    char *body = build_request_body(req, llm->model, 0);
    if (!body) {
        set_error(resp, "request build failed");
        return -1;
    }
    strmap *hdrs = anthropic_headers(llm);

    http_response *r =
        http_post(impl_of(llm)->base_url, ANTHROPIC_PATH, body, "application/json", hdrs, llm_timeout_ms());
    free(body);
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
        snprintf(err, sizeof(err), "anthropic http %d: %s", r->status, r->body && r->body[0] ? r->body : "(empty)");
        set_error(resp, err);
        http_response_free(r);
        return -1;
    }

    cJSON *root = cJSON_Parse(r->body);
    http_response_free(r);
    if (!root) {
        set_error(resp, "anthropic: invalid JSON response");
        return -1;
    }

    strbuf sb;
    strbuf_init(&sb);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    if (content && cJSON_IsArray(content)) {
        cJSON *it;
        cJSON_ArrayForEach(it, content) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(it, "text");
            if (text && cJSON_IsString(text))
                strbuf_append(&sb, text->valuestring);
        }
    }
    resp->content = strbuf_detach(&sb);
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

static int anthropic_stream(llm *llm, const llm_request *req, llm_stream_cb cb, void *ud) {
    char *body = build_request_body(req, llm->model, 1);
    if (!body)
        return -1;
    strmap *hdrs = anthropic_headers(llm);

    sse *s =
        sse_start(impl_of(llm)->base_url, ANTHROPIC_PATH, body, "application/json", hdrs, llm_timeout_ms());
    free(body);
    if (hdrs) {
        strmap_free(hdrs);
        free(hdrs);
    }
    if (!s)
        return -1;
    if (sse_status(s) != 200) {
        log_warn("anthropic stream: http status %d", sse_status(s));
        sse_close(s);
        return -1;
    }

    char line[32768];
    while (sse_next(s, line, sizeof(line)) == 1) {
        if (llm->cancel) {
            sse_close(s);
            return -1;
        }
        cJSON *root = cJSON_Parse(line);
        if (!root)
            continue;
        /* content_block_delta -> delta.text */
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
        cJSON *text = delta ? cJSON_GetObjectItemCaseSensitive(delta, "text") : NULL;
        if (text && cJSON_IsString(text) && text->valuestring)
            cb(text->valuestring, ud);
        cJSON_Delete(root);
    }
    sse_close(s);
    return 0;
}

llm *anthropic_create(const char *base_url, const char *api_key, const char *model) {
    llm *llm = calloc(1, sizeof(*llm));
    anthropic_impl *im = calloc(1, sizeof(anthropic_impl));
    if (!llm || !im) {
        free(llm);
        free(im);
        return NULL;
    }
    static const llm_vtable vt = {anthropic_destroy, anthropic_chat, anthropic_stream};
    llm->vt = &vt;
    llm->provider = xstrdup("anthropic");
    llm->base_url = xstrdup(base_url && *base_url ? base_url : ANTHROPIC_DEFAULT_BASE);
    llm->api_key = api_key ? xstrdup(api_key) : NULL;
    llm->model = xstrdup(model ? model : "claude-sonnet-4-6");
    llm->impl = im;
    im->base_url = xstrdup(llm->base_url);
    return llm;
}
