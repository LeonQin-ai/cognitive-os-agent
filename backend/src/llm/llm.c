#include "cagent/llm/llm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* provider constructors */
ca_llm *ca_openai_create(const char *base_url, const char *api_key, const char *model);
ca_llm *ca_anthropic_create(const char *base_url, const char *api_key, const char *model);
ca_llm *ca_mock_create(const char *model);

ca_llm *ca_llm_create(const char *provider, const char *base_url, const char *api_key, const char *model) {
    if (!provider) return NULL;
    if (strcmp(provider, "openai") == 0)
        return ca_openai_create(base_url, api_key, model);
    if (strcmp(provider, "anthropic") == 0)
        return ca_anthropic_create(base_url, api_key, model);
    if (strcmp(provider, "mock") == 0)
        return ca_mock_create(model);
    return NULL;
}

void ca_llm_destroy(ca_llm *llm) {
    if (!llm) return;
    if (llm->vt && llm->vt->destroy) llm->vt->destroy(llm);
}

int ca_llm_chat(ca_llm *llm, const ca_llm_request *req, ca_llm_response *resp) {
    if (!llm || !llm->vt || !llm->vt->chat) return -1;
    return llm->vt->chat(llm, req, resp);
}

int ca_llm_stream(ca_llm *llm, const ca_llm_request *req, ca_llm_stream_cb cb, void *ud) {
    if (!llm || !llm->vt || !llm->vt->stream) return -1;
    return llm->vt->stream(llm, req, cb, ud);
}

char *ca_llm_chat_simple(ca_llm *llm, const char *system_prompt, const char *user_prompt) {
    ca_llm_message msgs[2] = {
        {"system", system_prompt ? system_prompt : ""},
        {"user", user_prompt ? user_prompt : ""},
    };
    ca_llm_request req = {0};
    req.messages = msgs;
    req.num_messages = 2;
    req.temperature = 0.2;
    req.max_tokens = 1024;
    ca_llm_response resp = {0};
    if (ca_llm_chat(llm, &req, &resp) != 0) {
        if (resp.error) { char *e = resp.error; resp.error = NULL; free(e); }
        return NULL;
    }
    char *out = resp.content;
    resp.content = NULL;
    return out;
}
