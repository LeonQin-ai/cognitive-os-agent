/* llm.h — unified LLM provider interface.
 * Providers: "openai" (OpenAI-compatible chat/completions), "anthropic"
 * (Claude Messages API), "mock" (built-in offline provider for demos/tests).
 * Each provider implements chat and streaming (SSE). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_llm coa_llm;

typedef struct coa_llm_message {
    const char *role;    /* "system" | "user" | "assistant" */
    const char *content;
    /* Optional image attachment (multimodal messages). When image_b64 is
     * non-NULL the adapters emit a typed content-parts array instead of a
     * plain string: OpenAI-compatible {type:image_url,data:URL} and Anthropic
     * {type:image,source:{type:base64}}. Mime defaults to "image/png". */
    const char *image_b64;   /* base64 payload, no data: prefix (may be NULL) */
    const char *image_mime;  /* e.g. "image/png" (NULL = image/png) */
} coa_llm_message;

typedef struct coa_llm_request {
    const char *model;         /* NULL = provider default */
    const coa_llm_message *messages;
    size_t num_messages;
    double temperature;
    int max_tokens;
    int stream;                /* set by the streaming entry points */
} coa_llm_request;

typedef struct coa_llm_response {
    char *content;             /* full accumulated text (caller frees) */
    char *error;               /* NULL if ok (caller frees) */
} coa_llm_response;

typedef void (*coa_llm_stream_cb)(const char *delta, void *ud);

typedef struct coa_llm_vtable {
    void (*destroy)(coa_llm *self);
    int (*chat)(coa_llm *self, const coa_llm_request *req, coa_llm_response *resp);
    int (*stream)(coa_llm *self, const coa_llm_request *req, coa_llm_stream_cb cb, void *ud);
} coa_llm_vtable;

struct coa_llm {
    const coa_llm_vtable *vt;
    char *provider;
    char *base_url;
    char *api_key;
    char *model;
    void *impl;
    volatile int cancel; /* set by coa_llm_cancel(); checked between stream deltas */
};

/* Provider capability summary (bridge-level; agents pick models by capability
 * without knowing who the model is). max_ctx is approximate tokens,
 * 0 = unknown. */
typedef struct {
    int stream;        /* supports SSE streaming */
    int tools;         /* supports tool/function calling */
    long long max_ctx; /* approximate context window in tokens */
} coa_llm_caps;

/* Create a provider instance. base_url may be NULL for defaults.
 * api_key may be NULL (required for anthropic). Returns NULL on bad provider. */
coa_llm *coa_llm_create(const char *provider, const char *base_url, const char *api_key, const char *model);
void coa_llm_destroy(coa_llm *llm);

/* HTTP timeout for LLM calls in ms (default 300000; override with
 * COA_LLM_TIMEOUT_MS / the "llm.timeout_ms" config key). Reasoning models
 * routinely think longer than a minute before the first byte. */
int coa_llm_timeout_ms(void);

/* Non-streaming chat. resp->content is filled; caller frees. Returns 0 ok, -1 error. */
int coa_llm_chat(coa_llm *llm, const coa_llm_request *req, coa_llm_response *resp);
/* Streaming chat; cb is called with deltas. Returns 0 ok, -1 error. A pending
 * cancel aborts the stream between deltas (-1). */
int coa_llm_stream(coa_llm *llm, const coa_llm_request *req, coa_llm_stream_cb cb, void *ud);

/* Request cancellation of an in-flight stream (safe from another thread;
 * takes effect between deltas, and also aborts a stream started afterwards).
 * The flag is consumed when the stream returns. */
void coa_llm_cancel(coa_llm *llm);

/* Borrowed capability record for this provider (static, do not free). */
const coa_llm_caps *coa_llm_capabilities(coa_llm *llm);

/* Convenience one-shot chat. Returns malloc'd string (NULL on error). */
char *coa_llm_chat_simple(coa_llm *llm, const char *system_prompt, const char *user_prompt);

/* Same, with an explicit max_tokens budget (needed when the reply embeds
 * long content, e.g. JSON plans carrying whole scripts — 1024 truncates). */
char *coa_llm_chat_simple_ex(coa_llm *llm, const char *system_prompt,
                             const char *user_prompt, int max_tokens);

#ifdef __cplusplus
}
#endif
