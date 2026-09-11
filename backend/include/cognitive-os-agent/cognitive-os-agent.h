/* cognitive-os-agent.h — Cognitive OS Runtime umbrella header.
 *
 * Top-level public API. Include this single header from application code.
 * Exposes the runtime context (runtime_ctx) that assembles all layers:
 * config / logging / event bus / scheduler / policy / memory / reasoning /
 * LLM / tools / transactions / snapshots / HTTP API.
 */
#pragma once

#define CAGENT_VERSION "0.1.0"

/* --- feature / platform helpers --- */
#if defined(_WIN32) || defined(_WIN64)
#define WINDOWS 1
#else
#define POSIX 1
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
const char *version(void);

/* --- runtime context --- */
typedef struct runtime_ctx {
    config *config;
    event_bus *bus;
    metrics *metrics;
    policy_engine *policy;
    memory *memory;
    snapshot *snapshot;
    tool_registry *tools;
    llm *llm;
    tx_manager *txm;
    reasoning *reasoning;
    scheduler *scheduler;
    http_server *http;
    blackboard *blackboard;         /* shared state space (multi-agent coordination) */
    agent_pool *agents;             /* registered agents sharing the blackboard */
    auth *auth;                     /* NULL unless an auth.key is configured */
    trace *trace;                   /* span-based tracing / observability */
    router *router;                 /* multi-provider model route table */
    usage *usage;                   /* per-model token accounting */
    plugin_registry *registry;      /* versioned plugin metadata */
    skill_registry *skills;         /* static Shell/Python skills */
    mcp_manager *mcp;               /* named MCP server connections */
    cluster *cluster;               /* cluster node registry */
    hook_registry *hooks;           /* horizontal hook system (third-party extensions) */
    state_store *state;             /* Context layer: KV/Task/Agent state slots */
    memory_service *memsvc;         /* Memory Service interface (default backend) */
    attention *attention;           /* salience scoring / focus */
    struct index *index;            /* code index over session-touched files */
    im *im;                         /* instant messaging store (sessions/messages) */
    im_channels *channels;          /* external messaging channel adapters (IM bridge) */
    struct thread_t *channels_poller; /* telegram inbound poller thread */
    volatile int channels_stop;         /* poller stop flag */
    struct thread_t *hb_poller;       /* cluster heartbeat thread */
    volatile int hb_stop;               /* heartbeat stop flag */
    mutex_t run_lock;                 /* serializes reasoning runs */
    char *state_root;
    char *workspace;
    char *provider;
    char *http_bind;  /* bind address ("127.0.0.1" default) */
    char *market_url; /* networked marketplace base URL ("" = local only) */
    int workers;
    int use_transaction;
    uint16_t http_port;
} runtime_ctx;

typedef struct config {
    const char *state_root; /* NULL = "state" */
    const char *workspace;  /* NULL = "." */
    const char *provider;   /* "mock" | "openai" | "anthropic" */
    const char *model;
    const char *base_url;
    const char *api_key;
    const char *market_url; /* NULL = local-only marketplace */
    uint16_t http_port;     /* 0 = no HTTP API */
    int workers;            /* scheduler worker threads; 0 = default (2) */
    int use_transaction;    /* 1 = wrap tool actions in a snapshot tx */
} config;

/* Build and wire all layers. Returns 0 ok, -1 error (ctx left zeroed). */
int init(runtime_ctx *ctx, const config *cfg);

/* Stop scheduler/http and free every owned component. */
void runtime_shutdown(runtime_ctx *ctx);

/* Run one prompt through the reasoning pipeline synchronously.
 * *answer receives malloc'd output (caller frees). Returns 0 ok, -1 failed. */
int run(runtime_ctx *ctx, const char *prompt, char **answer);

/* Run one task AS a named agent (must be registered via /v1/agents). Executes
 * through the reasoning engine and publishes the result on the shared
 * blackboard under the agent's name. Returns 0 ok, -1 bad args, -2 unknown. */
int agent_run(runtime_ctx *ctx, const char *agent, const char *task, char **answer);

/* Run a task through the multi-agent orchestration pipeline: LLM decomposes
 * the task into subtasks compiled to a Flow DAG, flow_run executes it (one
 * isolated reasoning instance per node, parallel; results on the blackboard
 * under "flow/"), and a final LLM call merges the results. *trace_json (if
 * non-NULL) receives a malloc'd JSON array [{id,agent,task,status,result}].
 * Degrades to a plain single-agent run when no agents are registered or the
 * plan is unparseable. */
int orchestrate(runtime_ctx *ctx, const char *task, char **answer, char **trace_json);

/* Decompose a task into a Flow DAG without executing it: the LLM plan is
 * compiled to {"nodes":[{id,agent,task}...],"edges":[]} (malloc'd, caller
 * frees) so the caller can inspect/modify it before flow_run. Returns 0
 * ok, -1 when no agents are registered or no plan could be parsed. */
int flow_decompose(runtime_ctx *ctx, const char *task, char **dag_json);

/* Switch the active LLM at runtime (provider/base_url/model/api_key). Rebuilds
 * the LLM adapter, swaps it into the reasoning engine under the run-lock, keeps
 * the route table in sync, and persists llm.* to <state_root>/cognitive-os-agent.json so it
 * survives restart. Returns 0 ok, -1 if the provider is invalid. */
int set_llm(runtime_ctx *ctx, const char *provider, const char *base_url, const char *model, const char *api_key);

/* Serve the HTTP API until stop. Returns 0 ok, -1 if no server. */
int serve(runtime_ctx *ctx);
void stop(runtime_ctx *ctx);

/* Process/state snapshot: export the full runtime state (KV/Task/Agent state
 * store, long-term memory facts, agent roster, llm config) as one JSON file;
 * import applies a snapshot back (state store entries + facts are restored).
 * Returns 0 ok, -1 bad args/unreadable file. */
int state_export(runtime_ctx *ctx, const char *path);
int state_import(runtime_ctx *ctx, const char *path);

#ifdef __cplusplus
}
#endif
