#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/infra/logging.h"

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

int llm_chat(llm *llm, const llm_request *req, llm_response *resp) {
    if (!llm || !llm->vt || !llm->vt->chat)
        return -1;
    return llm->vt->chat(llm, req, resp);
}

int llm_stream(llm *llm, const llm_request *req, llm_stream_cb cb, void *ud) {
    int rc;

    if (!llm || !llm->vt || !llm->vt->stream)
        return -1;
    rc = llm->vt->stream(llm, req, cb, ud);
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

char *llm_chat_simple(llm *llm, const char *system_prompt, const char *user_prompt) {
    return llm_chat_simple_ex(llm, system_prompt, user_prompt, 1024);
}

char *llm_chat_simple_ex(llm *llm, const char *system_prompt, const char *user_prompt, int max_tokens) {
    llm_request req = {0};
    llm_response resp = {0};
    char *out;

    llm_message msgs[2] = {
        {"system", system_prompt ? system_prompt : ""},
        {"user", user_prompt ? user_prompt : ""},
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
