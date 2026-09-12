/* cognitive-os-agent.c — runtime context assembly: wires config, bus, metrics, policy,
 * memory, snapshot, tools, LLM, transactions, reasoning, scheduler and the
 * HTTP API into a single runtime_ctx. */
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

const char *version(void) {
    return CAGENT_VERSION;
}

/* Scheduler completion hook: mirror terminal task status into the Context
 * layer's task state slot (architecture v1.0 §5). */
static void task_done(task *t, void *ud) {
    runtime_ctx *ctx = (runtime_ctx *)ud;
    const char *st = "UNKNOWN";

    if (!ctx->state || !t)
        return;
    switch (t->status) {
    case TS_QUEUED:
        st = "QUEUED";
        break;
    case TS_RUNNING:
        st = "RUNNING";
        break;
    case TS_DONE:
        st = "DONE";
        break;
    case TS_FAILED:
        st = "FAILED";
        break;
    case TS_CANCELLED:
        st = "CANCELLED";
        break;
    case TS_TIMEOUT:
        st = "TIMEOUT";
        break;
    default:
        break;
    }

    state_store_task_set(ctx->state, t->id, st, t->input);
}

/* ---------- process/state snapshot (architecture v1.0 §9) ---------- */

int state_export(runtime_ctx *ctx, const char *path) {
    cJSON *root;
    /* Context layer state slots */
    char *st;
    /* long-term memory facts */
    char *facts;
    /* agent roster (informational; agents.json is persisted separately) */
    char *roster;
    char *body;
    int rc;

    if (!ctx || !path || !*path)
        return -1;
    root = cJSON_CreateObject();
    if (!root)
        return -1;
    cJSON_AddStringToObject(root, "version", CAGENT_VERSION);
    cJSON_AddNumberToObject(root, "ts_ms", (double)time_now_ms());
    if (ctx->provider)
        cJSON_AddStringToObject(root, "provider", ctx->provider);
    /* Context layer state slots */
    st = ctx->state ? state_store_json(ctx->state) : NULL;
    if (st)
        cJSON_AddStringToObject(root, "state", st);
    free(st);
    /* long-term memory facts */
    facts = ctx->memory ? memory_longterm_json(ctx->memory) : NULL;
    if (facts)
        cJSON_AddStringToObject(root, "facts", facts);
    free(facts);
    /* agent roster (informational; agents.json is persisted separately) */
    roster = ctx->agents ? agent_pool_snapshot_json(ctx->agents) : NULL;
    if (roster)
        cJSON_AddStringToObject(root, "agents", roster);
    free(roster);
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body)
        return -1;
    rc = fs_write_file(path, body, strlen(body));
    free(body);
    return rc == 0 ? 0 : -1;
}

int state_import(runtime_ctx *ctx, const char *path) {
    char *body;
    cJSON *root;
    int rc = 0;
    cJSON *st;
    cJSON *facts;

    if (!ctx || !path || !*path)
        return -1;
    body = fs_read_file(path);
    if (!body)
        return -1;
    root = cJSON_Parse(body);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root)
            cJSON_Delete(root);
        return -1;
    }

    st = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (st && cJSON_IsString(st) && ctx->state) {
        if (state_store_load_json(ctx->state, st->valuestring) < 0)
            rc = -1;
    }

    facts = cJSON_GetObjectItemCaseSensitive(root, "facts");
    if (facts && cJSON_IsString(facts) && ctx->memory) {
        cJSON *fobj = cJSON_Parse(facts->valuestring);
        if (fobj && cJSON_IsObject(fobj)) {
            cJSON *it;
            cJSON_ArrayForEach(it, fobj) {
                if (it->string && cJSON_IsString(it))
                    memory_remember(ctx->memory, it->string, it->valuestring);
            }
        }
        if (fobj)
            cJSON_Delete(fobj);
        else
            rc = -1;
    }

    cJSON_Delete(root);
    return rc;
}

/* Scheduler task runner: run the prompt through the reasoning pipeline and
 * store the result on the task. Runs under the ctx run-lock (tasks are
 * serialized; the scheduler/worker machinery is still exercised). */
static void sched_trampoline(task *t, scheduler *s, void *ud) {
    runtime_ctx *ctx = (runtime_ctx *)ud;
    char *answer = NULL;

    (void)s;
    if (task_should_abort(t)) {
        t->status = TS_CANCELLED;
        return;
    }

    scheduler_yield(); /* cooperative fairness: let another task start first */
    if (t->userdata) {
        /* userdata marker (set by /v1/orchestrate = 1, /v1/flows = 2): run the
         * multi-agent pipeline / flow DAG instead of the single-agent loop */
        if (t->userdata == (void *)2)
            flow_run(ctx, t->input, &answer, NULL);
        else
            orchestrate(ctx, t->input, &answer, NULL);
    } else {
        mutex_lock(&ctx->run_lock);
        reasoning_run_ex(ctx->reasoning, t->tag, t->input, &answer);
        mutex_unlock(&ctx->run_lock);
    }

    t->output = answer ? answer : xstrdup("(no output)");
    if (!answer)
        t->status = TS_FAILED;
}

/* Ingest a message received from an external messaging channel into the IM
 * session linked to that channel, then push it to WebSocket clients. */
static void channel_ingest(const char *channel_name, const char *sender, const char *text, void *ud) {
    runtime_ctx *ctx = (runtime_ctx *)ud;
    int64_t sid;
    int64_t id;
    cJSON *o;
    char *js;

    if (!ctx || !ctx->im || !text)
        return;
    sid = im_session_by_channel(ctx->im, channel_name);
    if (sid < 0)
        return;
    id = im_send_ex(ctx->im, sid, "user", text, sender);
    if (id < 0)
        return;

    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "im.message");
    cJSON_AddNumberToObject(o, "session_id", (double)sid);
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddStringToObject(o, "role", "user");
    if (sender)
        cJSON_AddStringToObject(o, "sender", sender);
    cJSON_AddStringToObject(o, "content", text);
    cJSON_AddNumberToObject(o, "ts_ms", (double)time_now_ms());
    js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (js) {
        if (ctx->http)
            http_server_ws_broadcast(ctx->http, js);
        free(js);
    }

    if (ctx->memory) {
        char buf[384];
        snprintf(buf, sizeof(buf), "im channel %s (%s)", channel_name, sender ? sender : "phone");
        memory_record_experience(ctx->memory, buf, text);
    }
}

/* Poll every telegram channel for inbound messages (~5s tick, checks the stop
 * flag every 100ms so shutdown joins quickly). */
static void channel_poller_loop(void *arg) {
    runtime_ctx *ctx = (runtime_ctx *)arg;
    while (!ctx->channels_stop) {
        if (ctx->channels && ctx->im) {
            int n = im_channel_count(ctx->channels);
            for (int i = 0; i < n && !ctx->channels_stop; i++) {
                im_channel *ch = im_channel_get(ctx->channels, (size_t)i);
                if (ch && strcmp(ch->type, "telegram") == 0)
                    im_channel_poll_telegram(ctx->channels, ch->name, channel_ingest, ctx);
            }
        }
        for (int i = 0; i < 50 && !ctx->channels_stop; i++)
            time_sleep_ms(100);
    }
}

/* Cluster heartbeat: refresh self liveness every period; any node that misses
 * 3 consecutive periods is marked down. */
#define HB_INTERVAL_MS 5000
static void heartbeat_loop(void *arg) {
    runtime_ctx *ctx = (runtime_ctx *)arg;
    while (!ctx->hb_stop) {
        if (ctx->cluster) {
            cluster_heartbeat(ctx->cluster, "self");
            cluster_mark_down(ctx->cluster, 3 * HB_INTERVAL_MS);
        }
        for (int i = 0; i < HB_INTERVAL_MS / 100 && !ctx->hb_stop; i++)
            time_sleep_ms(100);
    }
}

/* Forward every bus event to WebSocket clients as {"type":"event",...}. */
static void bus_to_ws(const event *ev, void *ud) {
    runtime_ctx *ctx = (runtime_ctx *)ud;
    cJSON *o;
    char *s;

    if (!ctx || !ctx->http)
        return;
    o = cJSON_CreateObject();
    if (!o)
        return;
    cJSON_AddStringToObject(o, "type", "event");
    cJSON_AddNumberToObject(o, "kind", (double)ev->type);
    cJSON_AddStringToObject(o, "source", ev->source ? ev->source : "");
    cJSON_AddNumberToObject(o, "ts", (double)ev->ts_ms);
    if (ev->payload)
        cJSON_AddItemToObject(o, "payload", cJSON_Duplicate(ev->payload, 1));
    s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        http_server_ws_broadcast(ctx->http, s);
        free(s);
    }
}

int init(runtime_ctx *ctx, const config *cfg) {
    if (!ctx)
        return -1;
    memset(ctx, 0, sizeof(*ctx));
    mutex_init(&ctx->run_lock);

    const char *state_root = (cfg && cfg->state_root && *cfg->state_root) ? cfg->state_root : "state";
    const char *workspace = (cfg && cfg->workspace && *cfg->workspace) ? cfg->workspace : ".";
    ctx->state_root = xstrdup(state_root);
    ctx->workspace = xstrdup(workspace);

    fs_mkdirs(ctx->state_root);
    fs_mkdirs(ctx->workspace); /* tools (esp. shell cwd) need it to exist */
    sock_init();

    /* layered config: defaults -> <state_root>/cognitive-os-agent.json -> env COA_ */
    ctx->config = config_new();
    config_apply_json(ctx->config, "{\"llm.provider\":\"mock\",\"llm.model\":\"mock\",\"llm.base_url\":\"\","
                                       "\"scheduler.workers\":2,\"tx.use_transaction\":true,\"http.port\":0,"
                                       "\"workspace\":\".\",\"market.url\":\"\",\"reasoning.max_rounds\":32}");
    {
        char cfgfile[600];
        path_join(cfgfile, sizeof(cfgfile), ctx->state_root, "cognitive-os-agent.json");
        if (config_load_file(ctx->config, cfgfile) != 0)
            log_debug("no config file at %s (using defaults + env)", cfgfile);
    }

    config_apply_env(ctx->config, "COA_");

    /* effective values: explicit cfg > config > defaults */
    const char *provider = (cfg && cfg->provider && *cfg->provider)
                               ? cfg->provider
                               : config_get_str(ctx->config, "llm.provider", "mock");
    const char *model =
        (cfg && cfg->model && *cfg->model) ? cfg->model : config_get_str(ctx->config, "llm.model", NULL);
    const char *base_url = (cfg && cfg->base_url && *cfg->base_url)
                               ? cfg->base_url
                               : config_get_str(ctx->config, "llm.base_url", NULL);
    const char *api_key =
        (cfg && cfg->api_key && *cfg->api_key) ? cfg->api_key : config_get_str(ctx->config, "llm.api_key", NULL);
    ctx->workers =
        (cfg && cfg->workers > 0) ? cfg->workers : (int)config_get_int(ctx->config, "scheduler.workers", 2);
    ctx->use_transaction =
        (cfg && cfg->use_transaction) ? 1 : (int)config_get_bool(ctx->config, "tx.use_transaction", 1);
    ctx->http_port =
        (cfg && cfg->http_port > 0) ? cfg->http_port : (uint16_t)config_get_int(ctx->config, "http.port", 0);
    ctx->provider = xstrdup(provider);
    ctx->http_bind = xstrdup(config_get_str(ctx->config, "http.bind", "127.0.0.1"));
    ctx->market_url = xstrdup(config_get_str(ctx->config, "market.url", ""));
    if ((cfg && cfg->market_url && *cfg->market_url))
        ctx->market_url = xstrdup(cfg->market_url);

    /* embedding provider: embedding.provider = local (default) | remote */
    {
        const char *emb_provider = config_get_str(ctx->config, "embedding.provider", "local");
        if (emb_provider && strcmp(emb_provider, "remote") == 0) {
            const char *emb_base = config_get_str(ctx->config, "embedding.base_url", NULL);
            if (emb_base && *emb_base) {
                int rc = embedding_use_remote(emb_base, config_get_str(ctx->config, "embedding.api_key", NULL),
                                                  config_get_str(ctx->config, "embedding.model", NULL));
                log_info("embedding provider=remote base=%s rc=%d", emb_base, rc);
            } else {
                log_warn("embedding.provider=remote but embedding.base_url unset; using local");
                embedding_use_local();
            }
        } else {
            embedding_use_local();
        }
    }

    /* components */
    ctx->metrics = metrics_new();
    ctx->bus = event_bus_new();
    ctx->policy = policy_engine_new();
    policy_add_rule(ctx->policy, "*", "allow", "default allow (demo)");
    /* persisted business rules (policy.json): exact-name deny/ask rules always
     * beat the wildcard allow; denied tools are hidden from the planner AND
     * hard-blocked at execution time */
    {
        char ppath[600];
        path_join(ppath, sizeof(ppath), ctx->state_root, "policy.json");
        int nr = policy_load_file(ctx->policy, ppath);
        if (nr > 0)
            log_info("policy: loaded %d rule(s) from policy.json", nr);
    }

    /* horizontal hook system + builtin audit trail (hooks.jsonl) */
    ctx->hooks = hook_registry_new();
    if (ctx->hooks) {
        char hpath[600];
        path_join(hpath, sizeof(hpath), ctx->state_root, "hooks.jsonl");
        char *hpath_heap = xstrdup(hpath);
        if (hpath_heap) {
            hook_register(ctx->hooks, "*", hook_audit_file, hpath_heap);
            log_info("hook: builtin audit -> %s", hpath);
        }
    }

    ctx->memory = memory_new(ctx->state_root);
    {
        /* memory lifecycle (reinforce/decay/forget/archive): off by default;
         * enable via memory.half_life_ms + memory.min_strength (flat keys) */
        long long hl = config_get_int(ctx->config, "memory.half_life_ms", 0);
        double ms = (double)config_get_int(ctx->config, "memory.min_strength_pct", 0) / 100.0;
        int arc = (int)config_get_int(ctx->config, "memory.archive", 1);
        if (ctx->memory && (hl > 0 || ms > 0))
            memory_set_lifecycle(ctx->memory, hl, ms, arc);
    }

    {
        char uploads[600];
        path_join(uploads, sizeof(uploads), ctx->state_root, "uploads");
        int nch = memory_index_uploads(ctx->memory, uploads);
        if (nch > 0)
            log_info("memory: reindexed %d chunk(s) from uploads", nch);
    }

    ctx->snapshot = snapshot_open(ctx->state_root);
    /* snapshot.max_file (bytes; 0 = unlimited) overrides default/env when set */
    if (ctx->snapshot) {
        long long mf = (long long)config_get_int(ctx->config, "snapshot.max_file", -1);
        if (mf >= 0)
            snapshot_set_max_file(ctx->snapshot, mf);
    }

    /* Context layer: unified KV/Task/Agent state slots (<state_root>/state.json) */
    ctx->state = state_store_new();
    if (ctx->state) {
        char spath[600];
        path_join(spath, sizeof(spath), ctx->state_root, "state.json");
        if (state_store_load(ctx->state, spath) == 0)
            log_info("state: restored %d entr%s from %s", state_store_count(ctx->state),
                         state_store_count(ctx->state) == 1 ? "y" : "ies", spath);
        else
            state_store_save(ctx->state, spath); /* bind path: enables auto-flush */
    }

    /* Memory Service interface (default backend = the memory facade) */
    ctx->memsvc = memory_service_new_default(ctx->memory);

    ctx->tools = tool_registry_new();
    tool_register_builtins(ctx->tools);

    /* MCP: restore persisted connections and re-discover their tools */
    ctx->mcp = mcp_manager_new();
    if (ctx->mcp) {
        if (mcp_manager_load(ctx->mcp, ctx->state_root) == 0)
            log_info("mcp: connections restored from %s/mcp.json", ctx->state_root);
        int n = mcp_manager_sync_tools(ctx->mcp, ctx->tools);
        if (n > 0)
            log_info("mcp: %d remote tool(s) registered", n);
    }

    ctx->llm = llm_create(provider, base_url, api_key, model);
    if (!ctx->llm) {
        log_error("init: unknown LLM provider '%s'", provider);
        runtime_shutdown(ctx);
        return -1;
    }

    ctx->txm = tx_manager_new();

    /* multi-agent coordination: shared blackboard + agent pool + optional auth */
    ctx->blackboard = blackboard_new();
    ctx->agents = agent_pool_new();
    agent_pool_adopt_blackboard(ctx->agents, ctx->blackboard); /* one shared space */
    if (ctx->agents) {
        int nagents = agent_pool_load(ctx->agents, ctx->state_root);
        if (nagents > 0)
            log_info("agents: %d roster entr%s restored from %s/agents.json", nagents, nagents == 1 ? "y" : "ies",
                         ctx->state_root);
    }

    {
        const char *auth_key = config_get_str(ctx->config, "auth.key", NULL);
        if (auth_key && *auth_key) {
            ctx->auth = auth_new();
            auth_add_key(ctx->auth, auth_key);
        }
    }

    /* observability / routing / plugin / skill / mcp / cluster / attention */
    ctx->trace = trace_new(1024);
    ctx->router = router_new();
    ctx->usage = usage_new();
    ctx->registry = plugin_registry_new();
    plugin_registry_load(ctx->registry, ctx->state_root);
    ctx->skills = skill_registry_new();
    ctx->cluster = cluster_new();
    ctx->attention = attention_new();
    ctx->index = index_new();
    ctx->im = im_new(ctx->state_root);
    ctx->channels = im_channels_new(ctx->state_root);

    /* register the wasm3-backed sandbox Wasm runner */
    if (wasm3_available())
        sandbox_set_wasm_runner(wasm3_run);

    /* telegram inbound poller (joins fast when no telegram channels exist) */
    ctx->channels_stop = 0;
    ctx->channels_poller = thread_create(channel_poller_loop, ctx);

    /* seed the route table with the configured provider so the Models UI is
     * populated even before explicit routes are added */
    if (ctx->router) {
        /* restore any routes saved in a previous session (survives restart) */
        char rpath[600];
        path_join(rpath, sizeof(rpath), ctx->state_root, "routes.json");
        router_load_file(ctx->router, rpath);
        /* if no persisted routes existed, seed with the configured provider */
        if (router_count(ctx->router) == 0)
            router_add(ctx->router, provider, provider, base_url, api_key, model, 1.0);
        else
            router_save_file(ctx->router, rpath); /* normalise/ensure file exists */
        /* routing policy: cost / latency / capability:<tag> / round_robin */
        const char *pol = config_get_str(ctx->config, "llm.route_policy", "");
        if (pol && *pol)
            router_set_policy(ctx->router, pol);
    }

    /* built-in demo skill (cross-platform: echo works in cmd.exe and sh) */
    if (ctx->skills) {
        const skill demo = {"echo_hello", "Reply with a fixed greeting.", "shell",
                                "echo hello from cognitive-os-agent", NULL};
        skill_register(ctx->skills, &demo);
        skill_registry_load(ctx->skills, ctx->state_root);
    }

    /* self-evolution: re-bind generated tools (tool name -> skill) persisted
     * by the missing-capability loop, so capabilities survive restarts */
    if (ctx->tools && ctx->skills) {
        char *gmap = tool_generated_load_mapping(ctx->state_root);
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
                        tool_register_generated(ctx->tools, ctx->skills, t->valuestring, sk->valuestring) == 0)
                        nbound++;
                }
                if (nbound > 0)
                    log_info("plugins: re-bound %d generated tool(s) from generated_tools.json", nbound);
            }
            if (garr)
                cJSON_Delete(garr);
        }
    }

    /* register the local node in the cluster and start the heartbeat loop */
    if (ctx->cluster)
        cluster_upsert_ex(ctx->cluster, "self", "127.0.0.1", ctx->http_port, "coordinator", "llm,tools,mcp");
    ctx->hb_stop = 0;
    ctx->hb_poller = thread_create(heartbeat_loop, ctx);

    {
        reasoning_config rc;
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
        rc.max_rounds = config_get_int(ctx->config, "reasoning.max_rounds", 32);
        rc.hooks = ctx->hooks;
        /* Context MMU budgets (hot/warm/cold, chars per section) */
        rc.budget_hot = config_get_int(ctx->config, "context.budget_hot", 0);
        rc.budget_warm = config_get_int(ctx->config, "context.budget_warm", 0);
        rc.budget_cold = config_get_int(ctx->config, "context.budget_cold", 0);
        /* HyDE retrieval: hypothetical answer passage as cold-tier query */
        rc.hyde = (int)config_get_int(ctx->config, "retrieval.hyde", 0);
        /* Execution backend: local (default) | wsl | remote:<host> via
         * execution.backend + execution.remote_host */
        rc.exec_backend = config_get_str(ctx->config, "execution.backend", "local");
        rc.exec_host = config_get_str(ctx->config, "execution.remote_host", NULL);
        ctx->reasoning = reasoning_new(&rc);
        /* wire the multi-provider router into the reasoning loop */
        if (ctx->reasoning && ctx->router)
            reasoning_set_router(ctx->reasoning, ctx->router);
    }

    if (!ctx->reasoning) {
        log_error("init: reasoning engine failed to initialize");
        runtime_shutdown(ctx);
        return -1;
    }

    ctx->scheduler = scheduler_new(ctx->workers, sched_trampoline, ctx);
    if (!ctx->scheduler) {
        log_error("init: scheduler failed to start");
        runtime_shutdown(ctx);
        return -1;
    }

    /* mirror task lifecycle into the Context layer's task state slot */
    scheduler_set_completion_cb(ctx->scheduler, task_done, ctx);

    if (api_attach(ctx) != 0) {
        log_error("init: failed to start HTTP API on port %u", (unsigned)ctx->http_port);
        runtime_shutdown(ctx);
        return -1;
    }

    /* stream every bus event to connected WebSocket clients (live Monitor UI) */
    if (ctx->bus)
        event_bus_subscribe(ctx->bus, -1, bus_to_ws, ctx);

    log_info("cognitive-os-agent %s initialized (provider=%s workers=%d tx=%s state=%s)", CAGENT_VERSION, provider,
                 ctx->workers, ctx->use_transaction ? "on" : "off", ctx->state_root);
    if (ctx->bus) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "version", CAGENT_VERSION);
        cJSON_AddStringToObject(p, "provider", provider);
        event_bus_publish(ctx->bus, EV_SYSTEM, "cognitive-os-agent", p);
    }

    return 0;
}

void runtime_shutdown(runtime_ctx *ctx) {
    if (!ctx)
        return;
    stop(ctx);
    if (ctx->http) {
        http_server_free(ctx->http);
        ctx->http = NULL;
    }

    if (ctx->scheduler) {
        scheduler_shutdown(ctx->scheduler, 3000);
    }

    if (ctx->scheduler) {
        scheduler_free(ctx->scheduler);
        ctx->scheduler = NULL;
    }

    reasoning_free(ctx->reasoning);
    ctx->reasoning = NULL;
    llm_destroy(ctx->llm);
    ctx->llm = NULL;
    tx_manager_free(ctx->txm);
    ctx->txm = NULL;
    if (ctx->auth) {
        auth_free(ctx->auth);
        ctx->auth = NULL;
    }

    if (ctx->agents) {
        agent_pool_free(ctx->agents);
        ctx->agents = NULL;
    }

    if (ctx->blackboard) {
        blackboard_free(ctx->blackboard);
        ctx->blackboard = NULL;
    }

    tool_registry_free(ctx->tools);
    ctx->tools = NULL;
    if (ctx->snapshot)
        snapshot_close(ctx->snapshot);
    ctx->snapshot = NULL;
    if (ctx->memory)
        memory_free(ctx->memory);
    ctx->memory = NULL;
    if (ctx->policy)
        policy_engine_free(ctx->policy);
    ctx->policy = NULL;
    if (ctx->hooks) {
        hook_registry_free(ctx->hooks);
        ctx->hooks = NULL;
    }

    if (ctx->state) {
        state_store_free(ctx->state);
        ctx->state = NULL;
    }

    if (ctx->memsvc) {
        memory_service_free(ctx->memsvc);
        ctx->memsvc = NULL;
    }

    if (ctx->bus)
        event_bus_free(ctx->bus);
    ctx->bus = NULL;
    if (ctx->metrics)
        metrics_free(ctx->metrics);
    ctx->metrics = NULL;
    if (ctx->attention) {
        attention_free(ctx->attention);
        ctx->attention = NULL;
    }

    if (ctx->index) {
        index_free(ctx->index);
        ctx->index = NULL;
    }

    if (ctx->im) {
        im_free(ctx->im);
        ctx->im = NULL;
    }

    /* stop and join the channel poller before freeing the channel registry */
    ctx->channels_stop = 1;
    if (ctx->channels_poller) {
        thread_join(ctx->channels_poller);
        ctx->channels_poller = NULL;
    }

    if (ctx->channels) {
        im_channels_free(ctx->channels);
        ctx->channels = NULL;
    }

    /* stop the cluster heartbeat before freeing the node registry */
    ctx->hb_stop = 1;
    if (ctx->hb_poller) {
        thread_join(ctx->hb_poller);
        ctx->hb_poller = NULL;
    }

    if (ctx->cluster) {
        cluster_free(ctx->cluster);
        ctx->cluster = NULL;
    }

    if (ctx->mcp) {
        mcp_manager_free(ctx->mcp);
        ctx->mcp = NULL;
    }

    if (ctx->skills) {
        skill_registry_free(ctx->skills);
        ctx->skills = NULL;
    }

    if (ctx->registry) {
        plugin_registry_free(ctx->registry);
        ctx->registry = NULL;
    }

    if (ctx->usage) {
        usage_free(ctx->usage);
        ctx->usage = NULL;
    }

    if (ctx->router) {
        router_free(ctx->router);
        ctx->router = NULL;
    }

    if (ctx->trace) {
        trace_free(ctx->trace);
        ctx->trace = NULL;
    }

    if (ctx->config)
        config_free(ctx->config);
    ctx->config = NULL;

    mutex_destroy(&ctx->run_lock);
    free(ctx->state_root);
    free(ctx->workspace);
    free(ctx->provider);
    free(ctx->http_bind);
    free(ctx->market_url);
    ctx->state_root = ctx->workspace = ctx->provider = ctx->http_bind = NULL;
    ctx->market_url = NULL;
    sock_cleanup();
    memset(ctx, 0, sizeof(*ctx));
}

int set_llm(runtime_ctx *ctx, const char *provider, const char *base_url, const char *model, const char *api_key) {
    llm *nl;
    llm *old;

    if (!ctx || !provider || !*provider)
        return -1;
    nl = llm_create(provider, base_url, api_key, model);
    if (!nl)
        return -1;

    mutex_lock(&ctx->run_lock);
    old = ctx->llm;
    ctx->llm = nl;
    if (ctx->reasoning)
        reasoning_set_llm(ctx->reasoning, nl);
    /* keep the route table in sync: drop the previously-active route, then add
     * the new active one so round-robin never falls back to a stale config. */
    if (ctx->router) {
        if (ctx->provider)
            router_remove(ctx->router, ctx->provider);
        router_add(ctx->router, provider, provider, base_url, api_key, model, 1.0);
    }

    mutex_unlock(&ctx->run_lock);
    if (old)
        llm_destroy(old);

    if (ctx->config) {
        config_set_str(ctx->config, "llm.provider", provider);
        config_set_str(ctx->config, "llm.model", model);
        config_set_str(ctx->config, "llm.base_url", base_url);
        config_set_str(ctx->config, "llm.api_key", api_key);
        char cfgfile[600];
        path_join(cfgfile, sizeof(cfgfile), ctx->state_root, "cognitive-os-agent.json");
        if (config_save_file(ctx->config, cfgfile) != 0)
            log_warn("set_llm: could not persist config to %s", cfgfile);
        char rpath[600];
        path_join(rpath, sizeof(rpath), ctx->state_root, "routes.json");
        if (ctx->router && router_save_file(ctx->router, rpath) != 0)
            log_warn("set_llm: could not persist routes to %s", rpath);
    }

    free(ctx->provider);
    ctx->provider = xstrdup(provider);
    log_info("cognitive-os-agent: active LLM switched to provider=%s model=%s", provider,
                 model ? model : "(default)");
    return 0;
}

int run(runtime_ctx *ctx, const char *prompt, char **answer) {
    int rc;

    if (!ctx || !prompt || !ctx->reasoning)
        return -1;
    mutex_lock(&ctx->run_lock);
    rc = reasoning_run(ctx->reasoning, prompt, answer);
    mutex_unlock(&ctx->run_lock);
    return rc;
}

/* Run a task as a named agent: executes through the reasoning engine, then
 * publishes the result on the shared blackboard under the agent's name so
 * other agents can read it (multi-agent coordination). */
int agent_run(runtime_ctx *ctx, const char *agent, const char *task, char **answer) {
    int rc;

    if (!ctx || !agent || !task || !ctx->reasoning || !ctx->agents)
        return -1;
    if (agent_pool_find(ctx->agents, agent) < 0)
        return -2; /* unknown agent */
    mutex_lock(&ctx->run_lock);
    rc = reasoning_run(ctx->reasoning, task, answer);
    mutex_unlock(&ctx->run_lock);
    if (rc == 0 && answer && *answer) {
        char key[160];
        snprintf(key, sizeof(key), "result:%s", agent);
        agent_post(ctx->agents, agent, key, *answer);
    }

    return rc;
}

int serve(runtime_ctx *ctx) {
    if (!ctx || !ctx->http)
        return -1;
    log_info("cognitive-os-agent HTTP API serving on port %u", (unsigned)ctx->http_port);
    return http_server_serve(ctx->http);
}

void stop(runtime_ctx *ctx) {
    if (ctx && ctx->http)
        http_server_stop(ctx->http);
}
