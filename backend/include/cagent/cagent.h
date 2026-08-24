/* cagent.h — Cognitive OS Runtime umbrella header.
 *
 * Top-level public API. Include this single header from application code.
 * Exposes the runtime context (cagent_ctx) that assembles all layers:
 * config / logging / event bus / scheduler / policy / memory / reasoning /
 * LLM / tools / transactions / snapshots / HTTP API.
 */
#pragma once

#define CAGENT_VERSION "0.1.0"

/* --- feature / platform helpers --- */
#if defined(_WIN32) || defined(_WIN64)
#  define CA_WINDOWS 1
#else
#  define CA_POSIX 1
#endif

/* Sub-layer public headers (all include-guarded). */
#include "cagent/runtime/event_bus.h"
#include "cagent/runtime/scheduler.h"
#include "cagent/runtime/task.h"
#include "cagent/runtime/state_machine.h"
#include "cagent/runtime/policy_engine.h"
#include "cagent/infra/config.h"
#include "cagent/infra/metrics.h"
#include "cagent/os/os_thread.h"
#include "cagent/memory/memory.h"
#include "cagent/retrieval/context_builder.h"
#include "cagent/cognition/reasoning.h"
#include "cagent/cognition/planner.h"
#include "cagent/cognition/evaluator.h"
#include "cagent/cognition/blackboard.h"
#include "cagent/runtime/agent.h"
#include "cagent/llm/llm.h"
#include "cagent/action/tools.h"
#include "cagent/tx/tx.h"
#include "cagent/snapshot/snapshot.h"
#include "cagent/api/http_server.h"
#include "cagent/api/auth.h"
#include "cagent/api/websocket.h"
#include "cagent/plugin_runtime/manager.h"
#include "cagent/plugin_runtime/sandbox.h"
#include "cagent/plugin_runtime/capability.h"
#include "cagent/plugin_runtime/registry.h"
#include "cagent/plugin_intelligence/analyzer.h"
#include "cagent/plugin_intelligence/architect.h"
#include "cagent/plugin_intelligence/codegen.h"
#include "cagent/plugin_intelligence/testing.h"
#include "cagent/plugin_intelligence/security.h"
#include "cagent/plugin_intelligence/generator.h"
#include "cagent/action/skill.h"
#include "cagent/action/mcp_conn.h"
#include "cagent/cluster/node.h"
#include "cagent/cognition/attention.h"
#include "cagent/infra/trace.h"
#include "cagent/llm/router.h"
#include "cagent/llm/usage.h"
#include "cagent/im/im.h"
#include "cagent/im/channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the library version string, e.g. "0.1.0". */
const char *cagent_version(void);

/* --- runtime context --- */
typedef struct cagent_ctx {
    ca_config *config;
    ca_event_bus *bus;
    ca_metrics *metrics;
    ca_policy_engine *policy;
    ca_memory *memory;
    ca_snapshot *snapshot;
    ca_tool_registry *tools;
    ca_llm *llm;
    ca_tx_manager *txm;
    ca_reasoning *reasoning;
    ca_scheduler *scheduler;
    ca_http_server *http;
    ca_blackboard *blackboard;  /* shared state space (multi-agent coordination) */
    ca_agent_pool *agents;      /* registered agents sharing the blackboard */
    ca_auth *auth;              /* NULL unless an auth.key is configured */
    ca_trace *trace;            /* span-based tracing / observability */
    ca_router *router;          /* multi-provider model route table */
    ca_usage *usage;            /* per-model token accounting */
    ca_plugin_registry *registry; /* versioned plugin metadata */
    ca_skill_registry *skills;  /* static Shell/Python skills */
    ca_mcp_manager *mcp;        /* named MCP server connections */
    ca_cluster *cluster;        /* cluster node registry */
    ca_attention *attention;    /* salience scoring / focus */
    ca_im *im;                  /* instant messaging store (sessions/messages) */
    ca_im_channels *channels;   /* external messaging channel adapters (IM bridge) */
    struct ca_thread *channels_poller; /* telegram inbound poller thread */
    volatile int channels_stop; /* poller stop flag */
    ca_mutex run_lock;          /* serializes reasoning runs */
    char *state_root;
    char *workspace;
    char *provider;
    char *http_bind;            /* bind address ("127.0.0.1" default) */
    int workers;
    int use_transaction;
    uint16_t http_port;
} cagent_ctx;

typedef struct cagent_config {
    const char *state_root;     /* NULL = "state" */
    const char *workspace;      /* NULL = "." */
    const char *provider;       /* "mock" | "openai" | "anthropic" */
    const char *model;
    const char *base_url;
    const char *api_key;
    uint16_t http_port;         /* 0 = no HTTP API */
    int workers;                /* scheduler worker threads; 0 = default (2) */
    int use_transaction;        /* 1 = wrap tool actions in a snapshot tx */
} cagent_config;

/* Build and wire all layers. Returns 0 ok, -1 error (ctx left zeroed). */
int cagent_init(cagent_ctx *ctx, const cagent_config *cfg);

/* Stop scheduler/http and free every owned component. */
void cagent_shutdown(cagent_ctx *ctx);

/* Run one prompt through the reasoning pipeline synchronously.
 * *answer receives malloc'd output (caller frees). Returns 0 ok, -1 failed. */
int cagent_run(cagent_ctx *ctx, const char *prompt, char **answer);

/* Switch the active LLM at runtime (provider/base_url/model/api_key). Rebuilds
 * the LLM adapter, swaps it into the reasoning engine under the run-lock, keeps
 * the route table in sync, and persists llm.* to <state_root>/cagent.json so it
 * survives restart. Returns 0 ok, -1 if the provider is invalid. */
int cagent_set_llm(cagent_ctx *ctx, const char *provider, const char *base_url,
                   const char *model, const char *api_key);

/* Serve the HTTP API until cagent_stop. Returns 0 ok, -1 if no server. */
int cagent_serve(cagent_ctx *ctx);
void cagent_stop(cagent_ctx *ctx);

#ifdef __cplusplus
}
#endif
