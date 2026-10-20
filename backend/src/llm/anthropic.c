/* anthropic.c — Claude Messages API adapter (streaming + non-streaming). */
#include "llm/llm.h"
#include "llm/sse.h"
#include "os/http.h"
#include "infra/util.h"
#include "infra/logging.h"

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
    cJSON *msgs;
    char *s;

    cJSON_AddStringToObject(root, "model", model ? model : "claude-sonnet-4-6");
    cJSON_AddNumberToObject(root, "max_tokens", req->max_tokens > 0 ? req->max_tokens : 16384);
    if (req->temperature > 0)
        cJSON_AddNumberToObject(root, "temperature", req->temperature);
    if (stream)
        cJSON_AddBoolToObject(root, "stream", 1);

    if (req->tools_json) {
        cJSON *source=cJSON_Parse(req->tools_json), *entry;
        cJSON *native=cJSON_CreateArray();
        cJSON_ArrayForEach(entry,source) {
            cJSON *fn=cJSON_GetObjectItemCaseSensitive(entry,"function");
            const char *name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(fn,"name"));
            if (!name) continue;
            cJSON *tool=cJSON_CreateObject();cJSON_AddStringToObject(tool,"name",name);
            const char *desc=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(fn,"description"));
            cJSON_AddStringToObject(tool,"description",desc?desc:"");
            cJSON *params=cJSON_GetObjectItemCaseSensitive(fn,"parameters");
            cJSON_AddItemToObject(tool,"input_schema",params?cJSON_Duplicate(params,1):cJSON_CreateObject());
            cJSON_AddItemToArray(native,tool);
        }
        cJSON_Delete(source);
        if(cJSON_GetArraySize(native))cJSON_AddItemToObject(root,"tools",native);else cJSON_Delete(native);
    }
    msgs = cJSON_AddArrayToObject(root, "messages");
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

    s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static void set_error(llm_response *resp, const char *msg) {
    resp->error = xstrdup(msg);
}

/* Accumulate an Anthropic usage object {input_tokens, output_tokens}. */
static void accumulate_usage_obj(llm *llm, cJSON *u, long long *last) {
    cJSON *in, *out;

    if (!llm || !u || !cJSON_IsObject(u))
        return;
    in = cJSON_GetObjectItemCaseSensitive(u, "input_tokens");
    out = cJSON_GetObjectItemCaseSensitive(u, "output_tokens");
    if (cJSON_IsNumber(in) || cJSON_IsNumber(out)) {
        long long delta[2] = {0};
        cJSON *fields[2] = {in, out};
        for (int i=0; i<2; i++) if (cJSON_IsNumber(fields[i]) && fields[i]->valuedouble >= 0 && fields[i]->valuedouble < 9223372036854775807.0) {
            long long value = (long long)fields[i]->valuedouble;
            delta[i] = last ? (value > last[i] ? value-last[i] : 0) : value;
            if (last && value > last[i]) last[i] = value;
        }
        llm_record_usage(llm, delta[0], delta[1], 0);
    }
}

static strmap *anthropic_headers(llm *llm) {
    strmap *hdrs;

    if (!llm->api_key)
        return NULL;
    hdrs = calloc(1, sizeof(strmap));
    if (!hdrs)
        return NULL;
    strmap_set(hdrs, "x-api-key", llm->api_key);
    strmap_set(hdrs, "anthropic-version", "2023-06-01");
    return hdrs;
}

static int anthropic_chat(llm *llm, const llm_request *req, llm_response *resp) {
    char *body = build_request_body(req, llm->model, 0);
    strmap *hdrs;
    http_response *r;
    cJSON *root;
    strbuf sb;
    cJSON *content;

    if (!body) {
        set_error(resp, "request build failed");
        return -1;
    }

    hdrs = anthropic_headers(llm);

    r =
        http_post(impl_of(llm)->base_url, ANTHROPIC_PATH, body, "application/json", hdrs, llm_req_timeout_ms(req));
    free(body);
    if (hdrs) {
        strmap_free(hdrs);
        free(hdrs);
    }

    if (!r) {
        char err[320];
        snprintf(err, sizeof err, "http request failed: %s",
                 *http_last_error() ? http_last_error() : "transport failure");
        set_error(resp, err);
        return -1;
    }

    if (r->status != 200) {
        char err[512];
        snprintf(err, sizeof(err), "anthropic http %d: %s", r->status, r->body && r->body[0] ? r->body : "(empty)");
        set_error(resp, err);
        http_response_free(r);
        return -1;
    }

    root = cJSON_Parse(r->body);
    http_response_free(r);
    if (!root) {
        set_error(resp, "anthropic: invalid JSON response");
        return -1;
    }

    strbuf_init(&sb);
    cJSON *stop_reason=cJSON_GetObjectItemCaseSensitive(root,"stop_reason");
    resp->truncated=cJSON_IsString(stop_reason) && !strcmp(stop_reason->valuestring,"max_tokens");
    accumulate_usage_obj(llm, cJSON_GetObjectItemCaseSensitive(root, "usage"), NULL);
    content = cJSON_GetObjectItemCaseSensitive(root, "content");
    if (content && cJSON_IsArray(content)) {
        cJSON *it;
        cJSON_ArrayForEach(it, content) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(it, "text");
            if (text && cJSON_IsString(text))
                strbuf_append(&sb, text->valuestring);
        }
    }

    cJSON *actions=cJSON_CreateArray(), *block;
    int native_valid=!resp->truncated, native_seen=0;
    cJSON_ArrayForEach(block,content) {
        const char *type=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block,"type"));
        if (!type || strcmp(type,"tool_use")) continue;
        native_seen=1;
        const char *name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block,"name"));
        cJSON *input=cJSON_GetObjectItemCaseSensitive(block,"input");
        if(!name || !*name || !cJSON_IsObject(input)) { native_valid=0;break; }
        cJSON *action=cJSON_CreateObject();cJSON_AddStringToObject(action,"tool",name);
        cJSON_AddItemToObject(action,"args",cJSON_Duplicate(input,1));cJSON_AddItemToArray(actions,action);
    }
    if(cJSON_GetArraySize(actions) && native_valid) {
        resp->tool_actions_json=cJSON_PrintUnformatted(actions);
        log_info("anthropic: native tool blocks=%d",cJSON_GetArraySize(actions));
    }
    int tool_stop=cJSON_IsString(stop_reason) && !strcmp(stop_reason->valuestring,"tool_use");
    if ((!native_valid && native_seen) || (tool_stop && !resp->tool_actions_json))
        set_error(resp,"anthropic: incomplete native tool use; no actions executed");
    cJSON_Delete(actions);
    resp->native_end_turn=req->tools_json && !resp->tool_actions_json && !resp->truncated &&
        cJSON_IsString(stop_reason) && !strcmp(stop_reason->valuestring,"end_turn");
    resp->content = strbuf_detach(&sb);
    if ((!resp->content || !*resp->content) && !resp->tool_actions_json) {
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
    strmap *hdrs;
    sse *s;
    char line[32768];

    if (!body)
        return -1;
    hdrs = anthropic_headers(llm);

    s =
        sse_start(impl_of(llm)->base_url, ANTHROPIC_PATH, body, "application/json", hdrs, llm_req_timeout_ms(req));
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

    long long stream_usage[2] = {0};
    while (sse_next(s, line, sizeof(line)) == 1) {
        if (llm->cancel) {
            sse_close(s);
            return -1;
        }
        cJSON *root = cJSON_Parse(line);
        if (!root)
            continue;
        /* usage arrives in message_start (input tokens, inside "message")
         * and message_delta (cumulative output tokens, at top level) */
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        if (type && cJSON_IsString(type)) {
            if (strcmp(type->valuestring, "message_start") == 0)
                accumulate_usage_obj(llm, cJSON_GetObjectItemCaseSensitive(
                                               cJSON_GetObjectItemCaseSensitive(root, "message"), "usage"), stream_usage);
            else if (strcmp(type->valuestring, "message_delta") == 0)
                accumulate_usage_obj(llm, cJSON_GetObjectItemCaseSensitive(root, "usage"), stream_usage);
        }
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
