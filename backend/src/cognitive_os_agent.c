/* cognitive-os-agent.c — runtime context assembly: wires config, bus, metrics, policy,
 * memory, snapshot, tools, LLM, transactions, reasoning, scheduler and the
 * HTTP API into a single coa_ctx. */
#include "cognitive-os-agent/cognitive-os-agent.h"
#include "cognitive-os-agent/api/api_rest.h"
#include "cognitive-os-agent/runtime/event_bus.h"
#include "cognitive-os-agent/runtime/flow.h"
#include "cognitive-os-agent/os/os_socket.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/infra/logging.h"
#include "cognitive-os-agent/retrieval/embedding.h"
#include "cognitive-os-agent/retrieval/engine.h"
#include "cognitive-os-agent/im/im.h"
#include "cognitive-os-agent/plugin_runtime/wasm_runner.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

const char *coa_version(void) { return CAGENT_VERSION; }

/* Scheduler completion hook: mirror terminal task status into the Context
 * layer's task state slot (architecture v1.0 §5). */
static void coa_task_done(coa_task *t, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!ctx->state || !t) return;
    const char *st = "UNKNOWN";
    switch (t->status) {
    case COA_TS_QUEUED:    st = "QUEUED";    break;
    case COA_TS_RUNNING:   st = "RUNNING";   break;
    case COA_TS_DONE:      st = "DONE";      break;
    case COA_TS_FAILED:    st = "FAILED";    break;
    case COA_TS_CANCELLED: st = "CANCELLED"; break;
    case COA_TS_TIMEOUT:   st = "TIMEOUT";   break;
    default: break;
    }
    coa_state_store_task_set(ctx->state, t->id, st, t->input);
}

/* ---------- process/state snapshot (architecture v1.0 §9) ---------- */

int coa_state_export(coa_ctx *ctx, const char *path) {
    if (!ctx || !path || !*path) return -1;
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON_AddStringToObject(root, "version", CAGENT_VERSION);
    cJSON_AddNumberToObject(root, "ts_ms", (double)coa_time_now_ms());
    if (ctx->provider) cJSON_AddStringToObject(root, "provider", ctx->provider);
    /* Context layer state slots */
    char *st = ctx->state ? coa_state_store_json(ctx->state) : NULL;
    if (st) cJSON_AddStringToObject(root, "state", st);
    free(st);
    /* long-term memory facts */
    char *facts = ctx->memory ? coa_memory_longterm_json(ctx->memory) : NULL;
    if (facts) cJSON_AddStringToObject(root, "facts", facts);
    free(facts);
    /* agent roster (informational; agents.json is persisted separately) */
    char *roster = ctx->agents ? coa_agent_pool_snapshot_json(ctx->agents) : NULL;
    if (roster) cJSON_AddStringToObject(root, "agents", roster);
    free(roster);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;
    int rc = coa_fs_write_file(path, body, strlen(body));
    free(body);
    return rc == 0 ? 0 : -1;
}

int coa_state_import(coa_ctx *ctx, const char *path) {
    if (!ctx || !path || !*path) return -1;
    char *body = coa_fs_read_file(path);
    if (!body) return -1;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return -1;
    }
    int rc = 0;
    cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (st && cJSON_IsString(st) && ctx->state) {
        if (coa_state_store_load_json(ctx->state, st->valuestring) < 0) rc = -1;
    }
    cJSON *facts = cJSON_GetObjectItemCaseSensitive(root, "facts");
    if (facts && cJSON_IsString(facts) && ctx->memory) {
        cJSON *fobj = cJSON_Parse(facts->valuestring);
        if (fobj && cJSON_IsObject(fobj)) {
            cJSON *it;
            cJSON_ArrayForEach(it, fobj) {
                if (it->string && cJSON_IsString(it))
                    coa_memory_remember(ctx->memory, it->string, it->valuestring);
            }
        }
        if (fobj) cJSON_Delete(fobj);
        else rc = -1;
    }
    cJSON_Delete(root);
    return rc;
}

/* Scheduler task runner: run the prompt through the reasoning pipeline and
 * store the result on the task. Runs under the ctx run-lock (tasks are
 * serialized; the scheduler/worker machinery is still exercised). */
static void coa_sched_trampoline(coa_task *t, coa_scheduler *s, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    (void)s;
    if (coa_task_should_abort(t)) {
        t->status = COA_TS_CANCELLED;
        return;
    }
    char *answer = NULL;
    coa_scheduler_yield(); /* cooperative fairness: let another task start first */
    if (t->userdata) {
        /* userdata marker (set by /v1/orchestrate = 1, /v1/flows = 2): run the
         * multi-agent pipeline / flow DAG instead of the single-agent loop */
        if (t->userdata == (void *)2)
            coa_flow_run(ctx, t->input, &answer, NULL);
        else
            coa_orchestrate(ctx, t->input, &answer, NULL);
    } else {
        coa_mutex_lock(&ctx->run_lock);
        coa_reasoning_run(ctx->reasoning, t->input, &answer);
        coa_mutex_unlock(&ctx->run_lock);
    }
    t->output = answer ? answer : coa_strdup("(no output)");
    if (!answer) t->status = COA_TS_FAILED;
}

/* Ingest a message received from an external messaging channel into the IM
 * session linked to that channel, then push it to WebSocket clients. */
static void channel_ingest(const char *channel_name, const char *sender,
                           const char *text, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!ctx || !ctx->im || !text) return;
    int64_t sid = coa_im_session_by_channel(ctx->im, channel_name);
    if (sid < 0) return;
    int64_t id = coa_im_send_ex(ctx->im, sid, "user", text, sender);
    if (id < 0) return;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "im.message");
    cJSON_AddNumberToObject(o, "session_id", (double)sid);
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddStringToObject(o, "role", "user");
    if (sender) cJSON_AddStringToObject(o, "sender", sender);
    cJSON_AddStringToObject(o, "content", text);
    cJSON_AddNumberToObject(o, "ts_ms", (double)coa_time_now_ms());
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (js) {
        if (ctx->http) coa_http_server_ws_broadcast(ctx->http, js);
        free(js);
    }
    if (ctx->memory) {
        char buf[384];
        snprintf(buf, sizeof(buf), "im channel %s (%s)",
                 channel_name, sender ? sender : "phone");
        coa_memory_record_experience(ctx->memory, buf, text);
    }
}

/* Poll every telegram channel for inbound messages (~5s tick, checks the stop
 * flag every 100ms so shutdown joins quickly). */
static void channel_poller_loop(void *arg) {
    coa_ctx *ctx = (coa_ctx *)arg;
    while (!ctx->channels_stop) {
        if (ctx->channels && ctx->im) {
            int n = coa_im_channel_count(ctx->channels);
            for (int i = 0; i < n && !ctx->channels_stop; i++) {
                coa_im_channel *ch = coa_im_channel_get(ctx->channels, (size_t)i);
                if (ch && strcmp(ch->type, "telegram") == 0)
                    coa_im_channel_poll_telegram(ctx->channels, ch->name,
                                                channel_ingest, ctx);
            }
        }
        for (int i = 0; i < 50 && !ctx->channels_stop; i++)
            coa_time_sleep_ms(100);
    }
}

/* Cluster heartbeat: refresh self liveness every period; any node that misses
 * 3 consecutive periods is marked down. */
#define COA_HB_INTERVAL_MS 5000
static void heartbeat_loop(void *arg) {
    coa_ctx *ctx = (coa_ctx *)arg;
    while (!ctx->hb_stop) {
        if (ctx->cluster) {
            coa_cluster_heartbeat(ctx->cluster, "self");
            coa_cluster_mark_down(ctx->cluster, 3 * COA_HB_INTERVAL_MS);
        }
        for (int i = 0; i < COA_HB_INTERVAL_MS / 100 && !ctx->hb_stop; i++)
            coa_time_sleep_ms(100);
    }
}

/* Forward every bus event to WebSocket clients as {"type":"event",...}. */
static void bus_to_ws(const coa_event *ev, void *ud) {
    coa_ctx *ctx = (coa_ctx *)ud;
    if (!ctx || !ctx->http) return;
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "type", "event");
    cJSON_AddNumberToObject(o, "kind", (double)ev->type);
    cJSON_AddStringToObject(o, "source", ev->source ? ev->source : "");
    cJSON_AddNumberToObject(o, "ts", (double)ev->ts_ms);
    if (ev->payload) cJSON_AddItemToObject(o, "payload", cJSON_Duplicate(ev->payload, 1));
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        coa_http_server_ws_broadcast(ctx->http, s);
        free(s);
    }
}

int coa_init(coa_ctx *ctx, const coa_config *cfg) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    coa_mutex_init(&ctx->run_lock);

    const char *state_root = (cfg && cfg->state_root && *cfg->state_root) ? cfg->state_root : "state";
    const char *workspace  = (cfg && cfg->workspace  && *cfg->workspace)  ? cfg->workspace  : ".";
    ctx->state_root = coa_strdup(state_root);
    ctx->workspace  = coa_strdup(workspace);

    coa_fs_mkdirs(ctx->state_root);
    coa_fs_mkdirs(ctx->workspace); /* tools (esp. shell cwd) need it to exist */
    coa_sock_init();

    /* layered config: defaults -> <state_root>/cognitive-os-agent.json -> env COA_ */
    ctx->config = coa_config_new();
    coa_config_apply_json(ctx->config,
        "{\"llm.provider\":\"mock\",\"llm.model\":\"mock\",\"llm.base_url\":\"\","
        "\"scheduler.workers\":2,\"tx.use_transaction\":true,\"http.port\":0,"
        "\"workspace\":\".\",\"market.url\":\"\",\"reasoning.max_rounds\":32}");
    {
        char cfgfile[600];
        coa_path_join(cfgfile, sizeof(cfgfile), ctx->state_root, "cognitive-os-agent.json");
        if (coa_config_load_file(ctx->config, cfgfile) != 0)
            coa_log_debug("no config file at %s (using defaults + env)", cfgfile);
    }
    coa_config_apply_env(ctx->config, "COA_");

    /* effective values: explicit cfg > config > defaults */
    const char *provider = (cfg && cfg->provider && *cfg->provider) ? cfg->provider
                          : coa_config_get_str(ctx->config, "llm.provider", "mock");
    const char *model    = (cfg && cfg->model && *cfg->model) ? cfg->model
                          : coa_config_get_str(ctx->config, "llm.model", NULL);
    const char *base_url = (cfg && cfg->base_url && *cfg->base_url) ? cfg->base_url
                          : coa_config_get_str(ctx->config, "llm.base_url", NULL);
    const char *api_key  = (cfg && cfg->api_key && *cfg->api_key) ? cfg->api_key
                          : coa_config_get_str(ctx->config, "llm.api_key", NULL);
    ctx->workers = (cfg && cfg->workers > 0) ? cfg->workers
                  : (int)coa_config_get_int(ctx->config, "scheduler.workers", 2);
    ctx->use_transaction = (cfg && cfg->use_transaction) ? 1
                  : (int)coa_config_get_bool(ctx->config, "tx.use_transaction", 1);
    ctx->http_port = (cfg && cfg->http_port > 0) ? cfg->http_port
                  : (uint16_t)coa_config_get_int(ctx->config, "http.port", 0);
    ctx->provider  = coa_strdup(provider);
    ctx->http_bind = coa_strdup(coa_config_get_str(ctx->config, "http.bind", "127.0.0.1"));
    ctx->market_url = coa_strdup(coa_config_get_str(ctx->config, "market.url", ""));
    if ((cfg && cfg->market_url && *cfg->market_url))
        ctx->market_url = coa_strdup(cfg->market_url);

    /* embedding provider: embedding.provider = local (default) | remote */
    {
        const char *emb_provider = coa_config_get_str(ctx->config, "embedding.provider", "local");
        if (emb_provider && strcmp(emb_provider, "remote") == 0) {
            const char *emb_base = coa_config_get_str(ctx->config, "embedding.base_url", NULL);
            if (emb_base && *emb_base) {
                int rc = coa_embedding_use_remote(emb_base,
                            coa_config_get_str(ctx->config, "embedding.api_key", NULL),
                            coa_config_get_str(ctx->config, "embedding.model", NULL));
                coa_log_info("embedding provider=remote base=%s rc=%d", emb_base, rc);
            } else {
                coa_log_warn("embedding.provider=remote but embedding.base_url unset; using local");
                coa_embedding_use_local();
            }
        } else {
            coa_embedding_use_local();
        }
    }

    /* components */
    ctx->metrics = coa_metrics_new();
    ctx->bus = coa_event_bus_new();
    ctx->policy = coa_policy_engine_new();
    coa_policy_add_rule(ctx->policy, "*", "allow", "default allow (demo)");
    /* persisted business rules (policy.json): exact-name deny/ask rules always
     * beat the wildcard allow; denied tools are hidden from the planner AND
     * hard-blocked at execution time */
    {
        char ppath[600];
        coa_path_join(ppath, sizeof(ppath), ctx->state_root, "policy.json");
        int nr = coa_policy_load_file(ctx->policy, ppath);
        if (nr > 0) coa_log_info("policy: loaded %d rule(s) from policy.json", nr);
    }

    /* horizontal hook system + builtin audit trail (hooks.jsonl) */
    ctx->hooks = coa_hook_registry_new();
    if (ctx->hooks) {
        char hpath[600];
        coa_path_join(hpath, sizeof(hpath), ctx->state_root, "hooks.jsonl");
        char *hpath_heap = coa_strdup(hpath);
        if (hpath_heap) {
            coa_hook_register(ctx->hooks, "*", coa_hook_audit_file, hpath_heap);
            coa_log_info("hook: builtin audit -> %s", hpath);
        }
    }

    ctx->memory = coa_memory_new(ctx->state_root);
    {
        /* memory lifecycle (reinforce/decay/forget/archive): off by default;
         * enable via memory.half_life_ms + memory.min_strength (flat keys) */
        long long hl = coa_config_get_int(ctx->config, "memory.half_life_ms", 0);
        double ms = (double)coa_config_get_int(ctx->config, "memory.min_strength_pct", 0) / 100.0;
        int arc = (int)coa_config_get_int(ctx->config, "memory.archive", 1);
        if (ctx->memory && (hl > 0 || ms > 0))
            coa_memory_set_lifecycle(ctx->memory, hl, ms, arc);
    }
    {
        char uploads[600];
        coa_path_join(uploads, sizeof(uploads), ctx->state_root, "uploads");
        int nch = coa_memory_index_uploads(ctx->memory, uploads);
        if (nch > 0) coa_log_info("memory: reindexed %d chunk(s) from uploads", nch);
    }
    ctx->snapshot = coa_snapshot_open(ctx->state_root);
    /* snapshot.max_file (bytes; 0 = unlimited) overrides default/env when set */
    if (ctx->snapshot) {
        long long mf = (long long)coa_config_get_int(ctx->config, "snapshot.max_file", -1);
        if (mf >= 0) coa_snapshot_set_max_file(ctx->snapshot, mf);
    }

    /* Context layer: unified KV/Task/Agent state slots (<state_root>/state.json) */
    ctx->state = coa_state_store_new();
    if (ctx->state) {
        char spath[600];
        coa_path_join(spath, sizeof(spath), ctx->state_root, "state.json");
        if (coa_state_store_load(ctx->state, spath) == 0)
            coa_log_info("state: restored %d entr%s from %s",
                        coa_state_store_count(ctx->state),
                        coa_state_store_count(ctx->state) == 1 ? "y" : "ies", spath);
        else
            coa_state_store_save(ctx->state, spath); /* bind path: enables auto-flush */
    }
    /* Memory Service interface (default backend = the coa_memory facade) */
    ctx->memsvc = coa_memory_service_new_default(ctx->memory);

    ctx->tools = coa_tool_registry_new();
    coa_tool_register_builtins(ctx->tools);

    /* MCP: restore persisted connections and re-discover their tools */
    ctx->mcp = coa_mcp_manager_new();
    if (ctx->mcp) {
        if (coa_mcp_manager_load(ctx->mcp, ctx->state_root) == 0)
            coa_log_info("mcp: connections restored from %s/mcp.json", ctx->state_root);
        int n = coa_mcp_manager_sync_tools(ctx->mcp, ctx->tools);
        if (n > 0) coa_log_info("mcp: %d remote tool(s) registered", n);
    }

    ctx->llm = coa_llm_create(provider, base_url, api_key, model);
    if (!ctx->llm) {
        coa_log_error("coa_init: unknown LLM provider '%s'", provider);
        coa_shutdown(ctx);
        return -1;
    }
    ctx->txm = coa_tx_manager_new();

    /* multi-agent coordination: shared blackboard + agent pool + optional auth */
    ctx->blackboard = coa_blackboard_new();
    ctx->agents = coa_agent_pool_new();
    coa_agent_pool_adopt_blackboard(ctx->agents, ctx->blackboard); /* one shared space */
    if (ctx->agents) {
        int nagents = coa_agent_pool_load(ctx->agents, ctx->state_root);
        if (nagents > 0)
            coa_log_info("agents: %d roster entr%s restored from %s/agents.json",
                        nagents, nagents == 1 ? "y" : "ies", ctx->state_root);
    }
    {
        const char *auth_key = coa_config_get_str(ctx->config, "auth.key", NULL);
        if (auth_key && *auth_key) {
            ctx->auth = coa_auth_new();
            coa_auth_add_key(ctx->auth, auth_key);
        }
    }

    /* observability / routing / plugin / skill / mcp / cluster / attention */
    ctx->trace = coa_trace_new(1024);
    ctx->router = coa_router_new();
    ctx->usage = coa_usage_new();
    ctx->registry = coa_plugin_registry_new();
    coa_plugin_registry_load(ctx->registry, ctx->state_root);
    ctx->skills = coa_skill_registry_new();
    ctx->cluster = coa_cluster_new();
    ctx->attention = coa_attention_new();
    ctx->index = coa_index_new();
    ctx->im = coa_im_new(ctx->state_root);
    ctx->channels = coa_im_channels_new(ctx->state_root);

    /* register the wasm3-backed sandbox Wasm runner */
    if (coa_wasm3_available()) coa_sandbox_set_wasm_runner(coa_wasm3_run);

    /* telegram inbound poller (joins fast when no telegram channels exist) */
    ctx->channels_stop = 0;
    ctx->channels_poller = coa_thread_create(channel_poller_loop, ctx);

    /* seed the route table with the configured provider so the Models UI is
     * populated even before explicit routes are added */
    if (ctx->router) {
        /* restore any routes saved in a previous session (survives restart) */
        char rpath[600];
        coa_path_join(rpath, sizeof(rpath), ctx->state_root, "routes.json");
        coa_router_load_file(ctx->router, rpath);
        /* if no persisted routes existed, seed with the configured provider */
        if (coa_router_count(ctx->router) == 0)
            coa_router_add(ctx->router, provider, provider, base_url, api_key, model, 1.0);
        else
            coa_router_save_file(ctx->router, rpath); /* normalise/ensure file exists */
        /* routing policy: cost / latency / capability:<tag> / round_robin */
        const char *pol = coa_config_get_str(ctx->config, "llm.route_policy", "");
        if (pol && *pol) coa_router_set_policy(ctx->router, pol);
    }

    /* built-in demo skill (cross-platform: echo works in cmd.exe and sh) */
    if (ctx->skills) {
        const coa_skill demo = { "echo_hello", "Reply with a fixed greeting.",
                                "shell", "echo hello from cognitive-os-agent", NULL };
        coa_skill_register(ctx->skills, &demo);
        coa_skill_registry_load(ctx->skills, ctx->state_root);
    }

    /* self-evolution: re-bind generated tools (tool name -> skill) persisted
     * by the missing-capability loop, so capabilities survive restarts */
    if (ctx->tools && ctx->skills) {
        char *gmap = coa_tool_generated_load_mapping(ctx->state_root);
        if (gmap) {
            cJSON *garr = cJSON_Parse(gmap);
            free(gmap);
            if (garr && cJSON_IsArray(garr)) {
                int nbound = 0;
                cJSON *git;
                cJSON_ArrayForEach(git, garr) {
                    cJSON *t = cJSON_GetObjectItemCaseSensitive(git, "tool");
                    cJSON *sk = cJSON_GetObjectItemCaseSensitive(git, "skill");
                    if (t && cJSON_IsString(t) && sk && cJSON_IsString(sk) &&
                        coa_tool_register_generated(ctx->tools, ctx->skills,
                                                   t->valuestring, sk->valuestring) == 0)
                        nbound++;
                }
                if (nbound > 0)
                    coa_log_info("plugins: re-bound %d generated tool(s) from generated_tools.json", nbound);
            }
            if (garr) cJSON_Delete(garr);
        }
    }

    /* register the local node in the cluster and start the heartbeat loop */
    if (ctx->cluster)
        coa_cluster_upsert_ex(ctx->cluster, "self", "127.0.0.1",
                             ctx->http_port, "coordinator", "llm,tools,mcp");
    ctx->hb_stop = 0;
    ctx->hb_poller = coa_thread_create(heartbeat_loop, ctx);

    {
        coa_reasoning_config rc;
        memset(&rc, 0, sizeof(rc));
        rc.llm = ctx->llm;
        rc.tools = ctx->tools;
        rc.memory = ctx->memory;
        rc.policy = ctx->policy;
        rc.snapshot = ctx->snapshot;
        rc.bus = ctx->bus;
        rc.metrics = ctx->metrics;
        rc.workspace = ctx->workspace;
        rc.use_transaction = ctx->use_transaction;
        rc.skills = ctx->skills;
        rc.mcp = ctx->mcp;
        rc.index = ctx->index;
        rc.plugin_registry = ctx->registry;
        rc.state_root = ctx->state_root;
        rc.max_rounds = coa_config_get_int(ctx->config, "reasoning.max_rounds", 32);
        rc.hooks = ctx->hooks;
        /* Context MMU budgets (hot/warm/cold, chars per section) */
        rc.budget_hot = coa_config_get_int(ctx->config, "context.budget_hot", 0);
        rc.budget_warm = coa_config_get_int(ctx->config, "context.budget_warm", 0);
        rc.budget_cold = coa_config_get_int(ctx->config, "context.budget_cold", 0);
        /* HyDE retrieval: hypothetical answer passage as cold-tier query */
        rc.hyde = (int)coa_config_get_int(ctx->config, "retrieval.hyde", 0);
        /* Execution backend: local (default) | wsl | remote:<host> via
         * execution.backend + execution.remote_host */
        rc.exec_backend = coa_config_get_str(ctx->config, "execution.backend", "local");
        rc.exec_host = coa_config_get_str(ctx->config, "execution.remote_host", NULL);
        ctx->reasoning = coa_reasoning_new(&rc);
        /* wire the multi-provider router into the reasoning loop */
        if (ctx->reasoning && ctx->router)
            coa_reasoning_set_router(ctx->reasoning, ctx->router);
    }
    if (!ctx->reasoning) {
        coa_log_error("coa_init: reasoning engine failed to initialize");
        coa_shutdown(ctx);
        return -1;
    }

    ctx->scheduler = coa_scheduler_new(ctx->workers, coa_sched_trampoline, ctx);
    if (!ctx->scheduler) {
        coa_log_error("coa_init: scheduler failed to start");
        coa_shutdown(ctx);
        return -1;
    }
    /* mirror task lifecycle into the Context layer's task state slot */
    coa_scheduler_set_completion_cb(ctx->scheduler, coa_task_done, ctx);

    if (coa_api_attach(ctx) != 0) {
        coa_log_error("coa_init: failed to start HTTP API on port %u", (unsigned)ctx->http_port);
        coa_shutdown(ctx);
        return -1;
    }

    /* stream every bus event to connected WebSocket clients (live Monitor UI) */
    if (ctx->bus)
        coa_event_bus_subscribe(ctx->bus, -1, bus_to_ws, ctx);

    coa_log_info("cognitive-os-agent %s initialized (provider=%s workers=%d tx=%s state=%s)",
                CAGENT_VERSION, provider, ctx->workers,
                ctx->use_transaction ? "on" : "off", ctx->state_root);
    if (ctx->bus) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "version", CAGENT_VERSION);
        cJSON_AddStringToObject(p, "provider", provider);
        coa_event_bus_publish(ctx->bus, COA_EV_SYSTEM, "cognitive-os-agent", p);
    }
    return 0;
}

void coa_shutdown(coa_ctx *ctx) {
    if (!ctx) return;
    coa_stop(ctx);
    if (ctx->http) { coa_http_server_free(ctx->http); ctx->http = NULL; }
    if (ctx->scheduler) { coa_scheduler_shutdown(ctx->scheduler, 3000); }
    if (ctx->scheduler) { coa_scheduler_free(ctx->scheduler); ctx->scheduler = NULL; }

    coa_reasoning_free(ctx->reasoning);
    ctx->reasoning = NULL;
    coa_llm_destroy(ctx->llm);
    ctx->llm = NULL;
    coa_tx_manager_free(ctx->txm);
    ctx->txm = NULL;
    if (ctx->auth) { coa_auth_free(ctx->auth); ctx->auth = NULL; }
    if (ctx->agents) { coa_agent_pool_free(ctx->agents); ctx->agents = NULL; }
    if (ctx->blackboard) { coa_blackboard_free(ctx->blackboard); ctx->blackboard = NULL; }
    coa_tool_registry_free(ctx->tools);
    ctx->tools = NULL;
    if (ctx->snapshot) coa_snapshot_close(ctx->snapshot);
    ctx->snapshot = NULL;
    if (ctx->memory) coa_memory_free(ctx->memory);
    ctx->memory = NULL;
    if (ctx->policy) coa_policy_engine_free(ctx->policy);
    ctx->policy = NULL;
    if (ctx->hooks) { coa_hook_registry_free(ctx->hooks); ctx->hooks = NULL; }
    if (ctx->state) { coa_state_store_free(ctx->state); ctx->state = NULL; }
    if (ctx->memsvc) { coa_memory_service_free(ctx->memsvc); ctx->memsvc = NULL; }
    if (ctx->bus) coa_event_bus_free(ctx->bus);
    ctx->bus = NULL;
    if (ctx->metrics) coa_metrics_free(ctx->metrics);
    ctx->metrics = NULL;
    if (ctx->attention) { coa_attention_free(ctx->attention); ctx->attention = NULL; }
    if (ctx->index) { coa_index_free(ctx->index); ctx->index = NULL; }
    if (ctx->im) { coa_im_free(ctx->im); ctx->im = NULL; }
    /* stop and join the channel poller before freeing the channel registry */
    ctx->channels_stop = 1;
    if (ctx->channels_poller) { coa_thread_join(ctx->channels_poller); ctx->channels_poller = NULL; }
    if (ctx->channels) { coa_im_channels_free(ctx->channels); ctx->channels = NULL; }
    /* stop the cluster heartbeat before freeing the node registry */
    ctx->hb_stop = 1;
    if (ctx->hb_poller) { coa_thread_join(ctx->hb_poller); ctx->hb_poller = NULL; }
    if (ctx->cluster) { coa_cluster_free(ctx->cluster); ctx->cluster = NULL; }
    if (ctx->mcp) { coa_mcp_manager_free(ctx->mcp); ctx->mcp = NULL; }
    if (ctx->skills) { coa_skill_registry_free(ctx->skills); ctx->skills = NULL; }
    if (ctx->registry) { coa_plugin_registry_free(ctx->registry); ctx->registry = NULL; }
    if (ctx->usage) { coa_usage_free(ctx->usage); ctx->usage = NULL; }
    if (ctx->router) { coa_router_free(ctx->router); ctx->router = NULL; }
    if (ctx->trace) { coa_trace_free(ctx->trace); ctx->trace = NULL; }
    if (ctx->config) coa_config_free(ctx->config);
    ctx->config = NULL;

    coa_mutex_destroy(&ctx->run_lock);
    free(ctx->state_root);
    free(ctx->workspace);
    free(ctx->provider);
    free(ctx->http_bind);
    free(ctx->market_url);
    ctx->state_root = ctx->workspace = ctx->provider = ctx->http_bind = NULL;
    ctx->market_url = NULL;
    coa_sock_cleanup();
    memset(ctx, 0, sizeof(*ctx));
}

int coa_set_llm(coa_ctx *ctx, const char *provider, const char *base_url,
                   const char *model, const char *api_key) {
    if (!ctx || !provider || !*provider) return -1;
    coa_llm *nl = coa_llm_create(provider, base_url, api_key, model);
    if (!nl) return -1;

    coa_mutex_lock(&ctx->run_lock);
    coa_llm *old = ctx->llm;
    ctx->llm = nl;
    if (ctx->reasoning) coa_reasoning_set_llm(ctx->reasoning, nl);
    /* keep the route table in sync: drop the previously-active route, then add
     * the new active one so round-robin never falls back to a stale config. */
    if (ctx->router) {
        if (ctx->provider) coa_router_remove(ctx->router, ctx->provider);
        coa_router_add(ctx->router, provider, provider, base_url, api_key, model, 1.0);
    }
    coa_mutex_unlock(&ctx->run_lock);
    if (old) coa_llm_destroy(old);

    if (ctx->config) {
        coa_config_set_str(ctx->config, "llm.provider", provider);
        coa_config_set_str(ctx->config, "llm.model", model);
        coa_config_set_str(ctx->config, "llm.base_url", base_url);
        coa_config_set_str(ctx->config, "llm.api_key", api_key);
        char cfgfile[600];
        coa_path_join(cfgfile, sizeof(cfgfile), ctx->state_root, "cognitive-os-agent.json");
        if (coa_config_save_file(ctx->config, cfgfile) != 0)
            coa_log_warn("coa_set_llm: could not persist config to %s", cfgfile);
        char rpath[600];
        coa_path_join(rpath, sizeof(rpath), ctx->state_root, "routes.json");
        if (ctx->router && coa_router_save_file(ctx->router, rpath) != 0)
            coa_log_warn("coa_set_llm: could not persist routes to %s", rpath);
    }
    free(ctx->provider);
    ctx->provider = coa_strdup(provider);
    coa_log_info("cognitive-os-agent: active LLM switched to provider=%s model=%s",
                provider, model ? model : "(default)");
    return 0;
}

int coa_run(coa_ctx *ctx, const char *prompt, char **answer) {
    if (!ctx || !prompt || !ctx->reasoning) return -1;
    coa_mutex_lock(&ctx->run_lock);
    int rc = coa_reasoning_run(ctx->reasoning, prompt, answer);
    coa_mutex_unlock(&ctx->run_lock);
    return rc;
}

/* Run a task as a named agent: executes through the reasoning engine, then
 * publishes the result on the shared blackboard under the agent's name so
 * other agents can read it (multi-agent coordination). */
int coa_agent_run(coa_ctx *ctx, const char *agent, const char *task, char **answer) {
    if (!ctx || !agent || !task || !ctx->reasoning || !ctx->agents) return -1;
    if (coa_agent_pool_find(ctx->agents, agent) < 0) return -2; /* unknown agent */
    coa_mutex_lock(&ctx->run_lock);
    int rc = coa_reasoning_run(ctx->reasoning, task, answer);
    coa_mutex_unlock(&ctx->run_lock);
    if (rc == 0 && answer && *answer) {
        char key[160];
        snprintf(key, sizeof(key), "result:%s", agent);
        coa_agent_post(ctx->agents, agent, key, *answer);
    }
    return rc;
}

int coa_serve(coa_ctx *ctx) {
    if (!ctx || !ctx->http) return -1;
    coa_log_info("cognitive-os-agent HTTP API serving on port %u", (unsigned)ctx->http_port);
    return coa_http_server_serve(ctx->http);
}

void coa_stop(coa_ctx *ctx) {
    if (ctx && ctx->http) coa_http_server_stop(ctx->http);
}
