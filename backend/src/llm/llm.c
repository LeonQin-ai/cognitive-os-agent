#include "llm/llm.h"
#include "infra/logging.h"
#include "infra/util.h"
#include "security/secret.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* provider constructors */
llm *openai_create(const char *base_url, const char *api_key, const char *model);
llm *anthropic_create(const char *base_url, const char *api_key, const char *model);
llm *mock_create(const char *model);

llm *llm_create(const char *provider, const char *base_url, const char *api_key, const char *model) {
    if (!provider)
        return NULL;
    if (strcmp(provider, "openai") == 0)
        return openai_create(base_url, api_key, model);
    if (strcmp(provider, "anthropic") == 0)
        return anthropic_create(base_url, api_key, model);
    if (strcmp(provider, "mock") == 0)
        return mock_create(model);
    return NULL;
}

void llm_destroy(llm *llm) {
    if (!llm)
        return;
    if (llm->vt && llm->vt->destroy)
        llm->vt->destroy(llm);
}

/* LLM HTTP timeout (ms). Reasoning models think for minutes before the
 * first byte, so a 60s default aborted mid-reasoning ("http request
 * failed" with no server error). Override via LLM_TIMEOUT_MS
 * (the COA_* env mapping exposes it as the "llm.timeout_ms" key). */
int llm_timeout_ms(void) {
    const char *e = getenv("COA_LLM_TIMEOUT_MS");
    if (e && *e) {
        long v = atol(e);
        if (v >= 10000 && v <= 600000)
            return (int)v;
    }

    return 300000;
}

/* Per-request timeout: bounded callers (e.g. the UI connection test, which
 * runs inline on the single-threaded HTTP server) set req->timeout_ms so a
 * dead provider cannot freeze every other request for the full 5 minutes. */
int llm_req_timeout_ms(const llm_request *req) {
    if (req && req->timeout_ms > 0)
        return (int)req->timeout_ms;
    return llm_timeout_ms();
}

/* Secret Security Plane ingress check (§13): scan every message content.
 * Passthrough mode only audits; strict mode blocks HIGH-confidence secrets
 * before the request leaves the trust boundary. */
static int llm_guard_input(const llm_request *req) {
    const char **contents;
    size_t i;
    int rc;

    if (!req || req->num_messages == 0)
        return 0;
    contents = (const char **)calloc(req->num_messages, sizeof(char *));
    if (!contents)
        return 0; /* fail open on OOM */
    for (i = 0; i < req->num_messages; i++)
        contents[i] = req->messages[i].content;
    rc = secret_guard_llm_input(contents, req->num_messages);
    free(contents);
    return rc;
}

int llm_chat(llm *llm, const llm_request *req, llm_response *resp) {
    if (!llm || !llm->vt || !llm->vt->chat)
        return -1;
    if (llm_guard_input(req) != 0) {
        if (resp) {
            resp->error = xstrdup("blocked: strict secret-security policy detected a "
                                  "high-confidence secret in the request");
            resp->content = NULL;
        }
        return -1;
    }
    {
        int rc = llm->vt->chat(llm, req, resp);
        if (rc == 0 && resp)
            secret_guard_llm_output(&resp->content); /* egress redaction (§8.2) */
        return rc;
    }
}

int llm_stream(llm *llm, const llm_request *req, llm_stream_cb cb, void *ud) {
    int rc;
    void *guard;

    if (!llm || !llm->vt || !llm->vt->stream)
        return -1;
    if (llm_guard_input(req) != 0) {
        llm->cancel = 0;
        return -1;
    }
    /* egress filter: deltas pass through the streaming secret guard */
    guard = secret_stream_guard_new(cb, ud);
    if (guard)
        rc = llm->vt->stream(llm, req, secret_stream_guard_cb, guard);
    else
        rc = llm->vt->stream(llm, req, cb, ud); /* fail open on OOM */
    if (guard)
        secret_stream_guard_free(guard); /* flushes the held tail */
    llm->cancel = 0; /* consumed: the next stream starts uncancelled */
    return rc;
}

void llm_cancel(llm *llm) {
    if (llm)
        llm->cancel = 1;
}

const llm_caps *llm_capabilities(llm *llm) {
    static const llm_caps openai_caps = {1, 1, 128000};
    static const llm_caps anthropic_caps = {1, 1, 200000};
    static const llm_caps mock_caps = {1, 0, 8192};
    static const llm_caps unknown_caps = {1, 1, 0};
    if (!llm || !llm->provider)
        return &unknown_caps;
    if (strcmp(llm->provider, "openai") == 0)
        return &openai_caps;
    if (strcmp(llm->provider, "anthropic") == 0)
        return &anthropic_caps;
    if (strcmp(llm->provider, "mock") == 0)
        return &mock_caps;
    return &unknown_caps;
}

void llm_usage_totals(const llm *l, long long *tokens_in, long long *tokens_out) {
    if (tokens_in)
        *tokens_in = l ? l->usage_in : 0;
    if (tokens_out)
        *tokens_out = l ? l->usage_out : 0;
}

char *llm_chat_simple(llm *llm, const char *system_prompt, const char *user_prompt) {
    return llm_chat_simple_ex(llm, system_prompt, user_prompt, 1024);
}

char *llm_chat_simple_ex(llm *llm, const char *system_prompt, const char *user_prompt, int max_tokens) {
    llm_request req = {0};
    llm_response resp = {0};
    char *out;

    llm_message msgs[2] = {
        {.role = "system", .content = system_prompt ? system_prompt : ""},
        {.role = "user", .content = user_prompt ? user_prompt : ""},
    };
    req.messages = msgs;
    req.num_messages = 2;
    req.temperature = 0.2;
    req.max_tokens = max_tokens > 0 ? max_tokens : 1024;
    if (llm_chat(llm, &req, &resp) != 0) {
        if (resp.error) {
            log_warn("llm: chat_simple failed: %s", resp.error);
            char *e = resp.error;
            resp.error = NULL;
            free(e);
        }
        return NULL;
    }

    out = resp.content;
    resp.content = NULL;
    return out;
}
