#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/infra/logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* provider constructors */
coa_llm *coa_openai_create(const char *base_url, const char *api_key, const char *model);
coa_llm *coa_anthropic_create(const char *base_url, const char *api_key, const char *model);
coa_llm *coa_mock_create(const char *model);

coa_llm *coa_llm_create(const char *provider, const char *base_url, const char *api_key, const char *model) {
    if (!provider) return NULL;
    if (strcmp(provider, "openai") == 0)
        return coa_openai_create(base_url, api_key, model);
    if (strcmp(provider, "anthropic") == 0)
        return coa_anthropic_create(base_url, api_key, model);
    if (strcmp(provider, "mock") == 0)
        return coa_mock_create(model);
    return NULL;
}

void coa_llm_destroy(coa_llm *llm) {
    if (!llm) return;
    if (llm->vt && llm->vt->destroy) llm->vt->destroy(llm);
}

int coa_llm_chat(coa_llm *llm, const coa_llm_request *req, coa_llm_response *resp) {
    if (!llm || !llm->vt || !llm->vt->chat) return -1;
    return llm->vt->chat(llm, req, resp);
}

int coa_llm_stream(coa_llm *llm, const coa_llm_request *req, coa_llm_stream_cb cb, void *ud) {
    if (!llm || !llm->vt || !llm->vt->stream) return -1;
    int rc = llm->vt->stream(llm, req, cb, ud);
    llm->cancel = 0; /* consumed: the next stream starts uncancelled */
    return rc;
}

void coa_llm_cancel(coa_llm *llm) {
    if (llm) llm->cancel = 1;
}

const coa_llm_caps *coa_llm_capabilities(coa_llm *llm) {
    static const coa_llm_caps openai_caps = {1, 1, 128000};
    static const coa_llm_caps anthropic_caps = {1, 1, 200000};
    static const coa_llm_caps mock_caps = {1, 0, 8192};
    static const coa_llm_caps unknown_caps = {1, 1, 0};
    if (!llm || !llm->provider) return &unknown_caps;
    if (strcmp(llm->provider, "openai") == 0) return &openai_caps;
    if (strcmp(llm->provider, "anthropic") == 0) return &anthropic_caps;
    if (strcmp(llm->provider, "mock") == 0) return &mock_caps;
    return &unknown_caps;
}

char *coa_llm_chat_simple(coa_llm *llm, const char *system_prompt, const char *user_prompt) {
    coa_llm_message msgs[2] = {
        {"system", system_prompt ? system_prompt : ""},
        {"user", user_prompt ? user_prompt : ""},
    };
    coa_llm_request req = {0};
    req.messages = msgs;
    req.num_messages = 2;
    req.temperature = 0.2;
    req.max_tokens = 1024;
    coa_llm_response resp = {0};
    if (coa_llm_chat(llm, &req, &resp) != 0) {
        if (resp.error) {
            coa_log_warn("llm: chat_simple failed: %s", resp.error);
            char *e = resp.error; resp.error = NULL; free(e);
        }
        return NULL;
    }
    char *out = resp.content;
    resp.content = NULL;
    return out;
}
