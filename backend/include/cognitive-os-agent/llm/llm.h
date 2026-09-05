/* llm.h — unified LLM provider interface.
 * Providers: "openai" (OpenAI-compatible chat/completions), "anthropic"
 * (Claude Messages API), "mock" (built-in offline provider for demos/tests).
 * Each provider implements chat and streaming (SSE). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_llm ca_llm;

typedef struct ca_llm_message {
    const char *role;    /* "system" | "user" | "assistant" */
    const char *content;
} ca_llm_message;

typedef struct ca_llm_request {
    const char *model;         /* NULL = provider default */
    const ca_llm_message *messages;
    size_t num_messages;
    double temperature;
    int max_tokens;
    int stream;                /* set by the streaming entry points */
} ca_llm_request;

typedef struct ca_llm_response {
    char *content;             /* full accumulated text (caller frees) */
    char *error;               /* NULL if ok (caller frees) */
} ca_llm_response;

typedef void (*ca_llm_stream_cb)(const char *delta, void *ud);

typedef struct ca_llm_vtable {
    void (*destroy)(ca_llm *self);
    int (*chat)(ca_llm *self, const ca_llm_request *req, ca_llm_response *resp);
    int (*stream)(ca_llm *self, const ca_llm_request *req, ca_llm_stream_cb cb, void *ud);
} ca_llm_vtable;

struct ca_llm {
    const ca_llm_vtable *vt;
    char *provider;
    char *base_url;
    char *api_key;
    char *model;
    void *impl;
    volatile int cancel; /* set by ca_llm_cancel(); checked between stream deltas */
};

/* Provider capability summary (bridge-level; agents pick models by capability
 * without knowing who the model is). max_ctx is approximate tokens,
 * 0 = unknown. */
typedef struct {
    int stream;        /* supports SSE streaming */
    int tools;         /* supports tool/function calling */
    long long max_ctx; /* approximate context window in tokens */
} ca_llm_caps;

/* Create a provider instance. base_url may be NULL for defaults.
 * api_key may be NULL (required for anthropic). Returns NULL on bad provider. */
ca_llm *ca_llm_create(const char *provider, const char *base_url, const char *api_key, const char *model);
void ca_llm_destroy(ca_llm *llm);

/* Non-streaming chat. resp->content is filled; caller frees. Returns 0 ok, -1 error. */
int ca_llm_chat(ca_llm *llm, const ca_llm_request *req, ca_llm_response *resp);
/* Streaming chat; cb is called with deltas. Returns 0 ok, -1 error. A pending
 * cancel aborts the stream between deltas (-1). */
int ca_llm_stream(ca_llm *llm, const ca_llm_request *req, ca_llm_stream_cb cb, void *ud);

/* Request cancellation of an in-flight stream (safe from another thread;
 * takes effect between deltas, and also aborts a stream started afterwards).
 * The flag is consumed when the stream returns. */
void ca_llm_cancel(ca_llm *llm);

/* Borrowed capability record for this provider (static, do not free). */
const ca_llm_caps *ca_llm_capabilities(ca_llm *llm);

/* Convenience one-shot chat. Returns malloc'd string (NULL on error). */
char *ca_llm_chat_simple(ca_llm *llm, const char *system_prompt, const char *user_prompt);

#ifdef __cplusplus
}
#endif
