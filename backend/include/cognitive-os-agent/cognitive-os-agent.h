/* cognitive-os-agent.h — Cognitive OS Runtime umbrella header.
 *
 * Top-level public API. Include this single header from application code.
 * Exposes the runtime context (coa_ctx) that assembles all layers:
 * config / logging / event bus / scheduler / policy / memory / reasoning /
 * LLM / tools / transactions / snapshots / HTTP API.
 */
#pragma once

#define CAGENT_VERSION "0.1.0"

/* --- feature / platform helpers --- */
#if defined(_WIN32) || defined(_WIN64)
#  define COA_WINDOWS 1
#else
#  define COA_POSIX 1
#endif

/* Sub-layer public headers (all include-guarded). */
#include "cognitive-os-agent/runtime/event_bus.h"
#include "cognitive-os-agent/runtime/hook.h"
#include "cognitive-os-agent/runtime/scheduler.h"
#include "cognitive-os-agent/runtime/state_store.h"
#include "cognitive-os-agent/memory/service.h"
#include "cognitive-os-agent/runtime/task.h"
#include "cognitive-os-agent/runtime/state_machine.h"
#include "cognitive-os-agent/runtime/policy_engine.h"
#include "cognitive-os-agent/infra/config.h"
#include "cognitive-os-agent/infra/metrics.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/memory/memory.h"
#include "cognitive-os-agent/retrieval/context_builder.h"
#include "cognitive-os-agent/cognition/reasoning.h"
#include "cognitive-os-agent/cognition/planner.h"
#include "cognitive-os-agent/cognition/evaluator.h"
#include "cognitive-os-agent/cognition/blackboard.h"
#include "cognitive-os-agent/runtime/agent.h"
#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/tx/tx.h"
#include "cognitive-os-agent/snapshot/snapshot.h"
#include "cognitive-os-agent/api/http_server.h"
#include "cognitive-os-agent/api/auth.h"
#include "cognitive-os-agent/api/websocket.h"
#include "cognitive-os-agent/plugin_runtime/manager.h"
#include "cognitive-os-agent/plugin_runtime/sandbox.h"
#include "cognitive-os-agent/plugin_runtime/capability.h"
#include "cognitive-os-agent/plugin_runtime/registry.h"
#include "cognitive-os-agent/plugin_intelligence/analyzer.h"
#include "cognitive-os-agent/plugin_intelligence/architect.h"
#include "cognitive-os-agent/plugin_intelligence/codegen.h"
#include "cognitive-os-agent/plugin_intelligence/testing.h"
#include "cognitive-os-agent/plugin_intelligence/security.h"
#include "cognitive-os-agent/plugin_intelligence/generator.h"
#include "cognitive-os-agent/action/skill.h"
#include "cognitive-os-agent/action/mcp_conn.h"
#include "cognitive-os-agent/cluster/node.h"
#include "cognitive-os-agent/cognition/attention.h"
#include "cognitive-os-agent/infra/trace.h"
#include "cognitive-os-agent/llm/router.h"
#include "cognitive-os-agent/llm/usage.h"
#include "cognitive-os-agent/im/im.h"
#include "cognitive-os-agent/im/channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the library version string, e.g. "0.1.0". */
const char *coa_version(void);

/* --- runtime context --- */
typedef struct coa_ctx {
    coa_config *config;
    coa_event_bus *bus;
    coa_metrics *metrics;
    coa_policy_engine *policy;
    coa_memory *memory;
    coa_snapshot *snapshot;
    coa_tool_registry *tools;
    coa_llm *llm;
    coa_tx_manager *txm;
    coa_reasoning *reasoning;
    coa_scheduler *scheduler;
    coa_http_server *http;
    coa_blackboard *blackboard;  /* shared state space (multi-agent coordination) */
    coa_agent_pool *agents;      /* registered agents sharing the blackboard */
    coa_auth *auth;              /* NULL unless an auth.key is configured */
    coa_trace *trace;            /* span-based tracing / observability */
    coa_router *router;          /* multi-provider model route table */
    coa_usage *usage;            /* per-model token accounting */
    coa_plugin_registry *registry; /* versioned plugin metadata */
    coa_skill_registry *skills;  /* static Shell/Python skills */
    coa_mcp_manager *mcp;        /* named MCP server connections */
    coa_cluster *cluster;        /* cluster node registry */
    coa_hook_registry *hooks;    /* horizontal hook system (third-party extensions) */
    coa_state_store *state;      /* Context layer: KV/Task/Agent state slots */
    coa_memory_service *memsvc;  /* Memory Service interface (default backend) */
    coa_attention *attention;    /* salience scoring / focus */
    struct coa_index *index;     /* code index over session-touched files */
    coa_im *im;                  /* instant messaging store (sessions/messages) */
    coa_im_channels *channels;   /* external messaging channel adapters (IM bridge) */
    struct coa_thread *channels_poller; /* telegram inbound poller thread */
    volatile int channels_stop; /* poller stop flag */
    struct coa_thread *hb_poller;       /* cluster heartbeat thread */
    volatile int hb_stop;              /* heartbeat stop flag */
    coa_mutex run_lock;          /* serializes reasoning runs */
    char *state_root;
    char *workspace;
    char *provider;
    char *http_bind;            /* bind address ("127.0.0.1" default) */
    char *market_url;           /* networked marketplace base URL ("" = local only) */
    int workers;
    int use_transaction;
    uint16_t http_port;
} coa_ctx;

typedef struct coa_config {
    const char *state_root;     /* NULL = "state" */
    const char *workspace;      /* NULL = "." */
    const char *provider;       /* "mock" | "openai" | "anthropic" */
    const char *model;
    const char *base_url;
    const char *api_key;
    const char *market_url;     /* NULL = local-only marketplace */
    uint16_t http_port;         /* 0 = no HTTP API */
    int workers;                /* scheduler worker threads; 0 = default (2) */
    int use_transaction;        /* 1 = wrap tool actions in a snapshot tx */
} coa_config;

/* Build and wire all layers. Returns 0 ok, -1 error (ctx left zeroed). */
int coa_init(coa_ctx *ctx, const coa_config *cfg);

/* Stop scheduler/http and free every owned component. */
void coa_shutdown(coa_ctx *ctx);

/* Run one prompt through the reasoning pipeline synchronously.
 * *answer receives malloc'd output (caller frees). Returns 0 ok, -1 failed. */
int coa_run(coa_ctx *ctx, const char *prompt, char **answer);

/* Run one task AS a named agent (must be registered via /v1/agents). Executes
 * through the reasoning engine and publishes the result on the shared
 * blackboard under the agent's name. Returns 0 ok, -1 bad args, -2 unknown. */
int coa_agent_run(coa_ctx *ctx, const char *agent, const char *task,
                     char **answer);

/* Run a task through the multi-agent orchestration pipeline: LLM decomposes
 * the task into subtasks compiled to a Flow DAG, coa_flow_run executes it (one
 * isolated reasoning instance per node, parallel; results on the blackboard
 * under "flow/"), and a final LLM call merges the results. *trace_json (if
 * non-NULL) receives a malloc'd JSON array [{id,agent,task,status,result}].
 * Degrades to a plain single-agent run when no agents are registered or the
 * plan is unparseable. */
int coa_orchestrate(coa_ctx *ctx, const char *task, char **answer,
                       char **trace_json);

/* Decompose a task into a Flow DAG without executing it: the LLM plan is
 * compiled to {"nodes":[{id,agent,task}...],"edges":[]} (malloc'd, caller
 * frees) so the caller can inspect/modify it before coa_flow_run. Returns 0
 * ok, -1 when no agents are registered or no plan could be parsed. */
int coa_flow_decompose(coa_ctx *ctx, const char *task, char **dag_json);

/* Switch the active LLM at runtime (provider/base_url/model/api_key). Rebuilds
 * the LLM adapter, swaps it into the reasoning engine under the run-lock, keeps
 * the route table in sync, and persists llm.* to <state_root>/cognitive-os-agent.json so it
 * survives restart. Returns 0 ok, -1 if the provider is invalid. */
int coa_set_llm(coa_ctx *ctx, const char *provider, const char *base_url,
                   const char *model, const char *api_key);

/* Serve the HTTP API until coa_stop. Returns 0 ok, -1 if no server. */
int coa_serve(coa_ctx *ctx);
void coa_stop(coa_ctx *ctx);

/* Process/state snapshot: export the full runtime state (KV/Task/Agent state
 * store, long-term memory facts, agent roster, llm config) as one JSON file;
 * import applies a snapshot back (state store entries + facts are restored).
 * Returns 0 ok, -1 bad args/unreadable file. */
int coa_state_export(coa_ctx *ctx, const char *path);
int coa_state_import(coa_ctx *ctx, const char *path);

#ifdef __cplusplus
}
#endif
