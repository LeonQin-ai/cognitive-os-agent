/* llm.h — unified LLM provider interface.
 * Providers: "openai" (OpenAI-compatible chat/completions), "anthropic"
 * (Claude Messages API), "mock" (built-in offline provider for demos/tests).
 * Each provider implements chat and streaming (SSE). */
#pragma once
#include <stddef.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct llm llm;

typedef struct llm_message {
    const char *role; /* "system" | "user" | "assistant" */
    const char *content;
    /* Optional image attachment (multimodal messages). When image_b64 is
     * non-NULL the adapters emit a typed content-parts array instead of a
     * plain string: OpenAI-compatible {type:image_url,data:URL} and Anthropic
     * {type:image,source:{type:base64}}. Mime defaults to "image/png". */
    const char *image_b64;  /* base64 payload, no data: prefix (may be NULL) */
    const char *image_mime; /* e.g. "image/png" (NULL = image/png) */
} llm_message;

typedef struct llm_request {
    const char *model; /* NULL = provider default */
    const llm_message *messages;
    size_t num_messages;
    double temperature;
    int max_tokens;
    const char *reasoning_effort; /* optional OpenAI-compatible reasoning control */
    const char *tools_json; /* optional OpenAI-format native function catalog, borrowed */
    int stream;      /* set by the streaming entry points */
    long timeout_ms; /* HTTP timeout override; 0 = llm_timeout_ms() default */
} llm_request;

typedef struct llm_response {
    char *content; /* full accumulated text (caller frees) */
    char *error;   /* NULL if ok (caller frees) */
    int truncated; /* provider stopped at its output-token limit */
    int http_status; /* 0 when no HTTP response was received */
    char stop_reason[64]; /* provider metadata only; never response text */
    size_t visible_content_bytes; /* before native-call validation */
    int tool_call_count; /* received blocks, including rejected/truncated ones */
    char *tool_actions_json; /* normalized native tool calls, caller frees */
} llm_response;

typedef void (*llm_stream_cb)(const char *delta, void *ud);

typedef struct llm_vtable {
    void (*destroy)(llm *self);
    int (*chat)(llm *self, const llm_request *req, llm_response *resp);
    int (*stream)(llm *self, const llm_request *req, llm_stream_cb cb, void *ud);
} llm_vtable;

struct llm {
    const llm_vtable *vt;
    char *provider;
    char *base_url;
    char *api_key;
    char *model;
    long long context_window_tokens; /* selected model capacity; 0 resolves via catalog/default */
    long long max_output_tokens; /* model capacity, not the per-request output budget */
    void *impl;
    volatile int cancel; /* set by llm_cancel(); checked between stream deltas */
    /* cumulative token usage from provider "usage" objects (polled without a
     * lock; atomic updates preserve concurrent request totals) */
    _Atomic long long usage_in, usage_out;
    /* invisible thinking tokens reported by thinking models (subset of
     * usage_out; issue #7 — shown separately on the dashboard) */
    _Atomic long long usage_reason;
};

/* Provider capability summary (bridge-level; agents pick models by capability
 * without knowing who the model is). max_ctx is approximate tokens,
 * 0 = unknown. */
typedef struct {
    int stream;        /* supports SSE streaming */
    int tools;         /* supports tool/function calling */
    long long max_ctx; /* approximate context window in tokens */
} llm_caps;

/* Create a provider instance. base_url may be NULL for defaults.
 * api_key may be NULL (required for anthropic). Returns NULL on bad provider. */
llm *llm_create(const char *provider, const char *base_url, const char *api_key, const char *model);
void llm_destroy(llm *llm);

/* HTTP timeout for LLM calls in ms (default 300000; override with
 * LLM_TIMEOUT_MS / the "llm.timeout_ms" config key). Reasoning models
 * routinely think longer than a minute before the first byte. */
int llm_timeout_ms(void);

/* Effective timeout for one request: req->timeout_ms when set (>0),
 * else the global llm_timeout_ms(). */
int llm_req_timeout_ms(const llm_request *req);

/* Non-streaming chat. resp->content is filled; caller frees. Returns 0 ok, -1 error. */
int llm_chat(llm *llm, const llm_request *req, llm_response *resp);
/* Streaming chat; cb is called with deltas. Returns 0 ok, -1 error. A pending
 * cancel aborts the stream between deltas (-1). */
int llm_stream(llm *llm, const llm_request *req, llm_stream_cb cb, void *ud);

/* Request cancellation of an in-flight stream (safe from another thread;
 * takes effect between deltas, and also aborts a stream started afterwards).
 * The flag is consumed when the stream returns. */
void llm_cancel(llm *llm);

/* Borrowed capability record for this provider (static, do not free). */
const llm_caps *llm_capabilities(llm *llm);
long long llm_context_window_tokens(const llm *llm);

/* Cumulative token usage observed from provider "usage" objects across all
 * chats/streams on this instance (0 if the provider never reported usage).
 * Values are polled without a lock — a status display may lag slightly. */
void llm_usage_totals(const llm *llm, long long *tokens_in, long long *tokens_out);
/* Cumulative invisible thinking tokens reported by the provider (0 when the
 * model is not a thinking model). Subset of usage_out. */
long long llm_usage_reason_total(const llm *llm);

/* Provider reports usage once per response (or increments of cumulative SSE usage).
 * Thread totals isolate accounting from other agents sharing this provider. */
void llm_record_usage(llm *l, long long in, long long out, long long reason);
void llm_usage_thread_totals(long long *in, long long *out, long long *reason, long long *reports);

/* Convenience one-shot chat. Returns malloc'd string (NULL on error). */
char *llm_chat_simple(llm *llm, const char *system_prompt, const char *user_prompt);

/* Same, with an explicit max_tokens budget (needed when the reply embeds
 * long content, e.g. JSON plans carrying whole scripts — 1024 truncates). */
char *llm_chat_simple_ex(llm *llm, const char *system_prompt, const char *user_prompt, int max_tokens);

#ifdef __cplusplus
}
#endif
