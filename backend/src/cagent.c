/* cagent.c — runtime context assembly: wires config, bus, metrics, policy,
 * memory, snapshot, tools, LLM, transactions, reasoning, scheduler and the
 * HTTP API into a single cagent_ctx. */
#include "cagent/cagent.h"
#include "cagent/api/api_rest.h"
#include "cagent/os/os_socket.h"
#include "cagent/os/os_fs.h"
#include "cagent/os/os_time.h"
#include "cagent/infra/util.h"
#include "cagent/infra/logging.h"
#include "cagent/retrieval/embedding.h"
#include "cagent/im/im.h"
#include "cagent/plugin_runtime/wasm_runner.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

const char *cagent_version(void) { return CAGENT_VERSION; }

/* Scheduler task runner: run the prompt through the reasoning pipeline and
 * store the result on the task. Runs under the ctx run-lock (tasks are
 * serialized; the scheduler/worker machinery is still exercised). */
static void cagent_task_runner(ca_task *t, ca_scheduler *s, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    (void)s;
    if (ca_task_should_abort(t)) {
        t->status = CA_TS_CANCELLED;
        return;
    }
    char *answer = NULL;
    ca_scheduler_yield(); /* cooperative fairness: let another task start first */
    ca_mutex_lock(&ctx->run_lock);
    int rc = ca_reasoning_run(ctx->reasoning, t->input, &answer);
    ca_mutex_unlock(&ctx->run_lock);
    t->output = answer ? answer : ca_strdup("(no output)");
    if (rc != 0) t->status = CA_TS_FAILED;
}

/* Ingest a message received from an external messaging channel into the IM
 * session linked to that channel, then push it to WebSocket clients. */
static void channel_ingest(const char *channel_name, const char *sender,
                           const char *text, void *ud) {
    cagent_ctx *ctx = (cagent_ctx *)ud;
    if (!ctx || !ctx->im || !text) return;
    int64_t sid = ca_im_session_by_channel(ctx->im, channel_name);
    if (sid < 0) return;
    int64_t id = ca_im_send_ex(ctx->im, sid, "user", text, sender);
    if (id < 0) return;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "im.message");
    cJSON_AddNumberToObject(o, "session_id", (double)sid);
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddStringToObject(o, "role", "user");
    if (sender) cJSON_AddStringToObject(o, "sender", sender);
    cJSON_AddStringToObject(o, "content", text);
    cJSON_AddNumberToObject(o, "ts_ms", (double)ca_time_now_ms());
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (js) {
        if (ctx->http) ca_http_server_ws_broadcast(ctx->http, js);
        free(js);
    }
    if (ctx->memory) {
        char buf[384];
        snprintf(buf, sizeof(buf), "im channel %s (%s)",
                 channel_name, sender ? sender : "phone");
        ca_memory_record_experience(ctx->memory, buf, text);
    }
}

/* Poll every telegram channel for inbound messages (~5s tick, checks the stop
 * flag every 100ms so shutdown joins quickly). */
static void channel_poller_loop(void *arg) {
    cagent_ctx *ctx = (cagent_ctx *)arg;
    while (!ctx->channels_stop) {
        if (ctx->channels && ctx->im) {
            int n = ca_im_channel_count(ctx->channels);
            for (int i = 0; i < n && !ctx->channels_stop; i++) {
                ca_im_channel *ch = ca_im_channel_get(ctx->channels, (size_t)i);
                if (ch && strcmp(ch->type, "telegram") == 0)
                    ca_im_channel_poll_telegram(ctx->channels, ch->name,
                                                channel_ingest, ctx);
            }
        }
        for (int i = 0; i < 50 && !ctx->channels_stop; i++)
            ca_time_sleep_ms(100);
    }
}

int cagent_init(cagent_ctx *ctx, const cagent_config *cfg) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ca_mutex_init(&ctx->run_lock);

    const char *state_root = (cfg && cfg->state_root && *cfg->state_root) ? cfg->state_root : "state";
    const char *workspace  = (cfg && cfg->workspace  && *cfg->workspace)  ? cfg->workspace  : ".";
    ctx->state_root = ca_strdup(state_root);
    ctx->workspace  = ca_strdup(workspace);

    ca_fs_mkdirs(ctx->state_root);
    ca_sock_init();

    /* layered config: defaults -> <state_root>/cagent.json -> env CA_ */
    ctx->config = ca_config_new();
    ca_config_apply_json(ctx->config,
        "{\"llm.provider\":\"mock\",\"llm.model\":\"mock\",\"llm.base_url\":\"\","
        "\"scheduler.workers\":2,\"tx.use_transaction\":true,\"http.port\":0,"
        "\"workspace\":\".\"}");
    {
        char cfgfile[600];
        ca_path_join(cfgfile, sizeof(cfgfile), ctx->state_root, "cagent.json");
        if (ca_config_load_file(ctx->config, cfgfile) != 0)
            ca_log_debug("no config file at %s (using defaults + env)", cfgfile);
    }
    ca_config_apply_env(ctx->config, "CA_");

    /* effective values: explicit cfg > config > defaults */
    const char *provider = (cfg && cfg->provider && *cfg->provider) ? cfg->provider
                          : ca_config_get_str(ctx->config, "llm.provider", "mock");
    const char *model    = (cfg && cfg->model && *cfg->model) ? cfg->model
                          : ca_config_get_str(ctx->config, "llm.model", NULL);
    const char *base_url = (cfg && cfg->base_url && *cfg->base_url) ? cfg->base_url
                          : ca_config_get_str(ctx->config, "llm.base_url", NULL);
    const char *api_key  = (cfg && cfg->api_key && *cfg->api_key) ? cfg->api_key
                          : ca_config_get_str(ctx->config, "llm.api_key", NULL);
    ctx->workers = (cfg && cfg->workers > 0) ? cfg->workers
                  : (int)ca_config_get_int(ctx->config, "scheduler.workers", 2);
    ctx->use_transaction = (cfg && cfg->use_transaction) ? 1
                  : (int)ca_config_get_bool(ctx->config, "tx.use_transaction", 1);
    ctx->http_port = (cfg && cfg->http_port > 0) ? cfg->http_port
                  : (uint16_t)ca_config_get_int(ctx->config, "http.port", 0);
    ctx->provider  = ca_strdup(provider);
    ctx->http_bind = ca_strdup(ca_config_get_str(ctx->config, "http.bind", "127.0.0.1"));

    /* embedding provider: embedding.provider = local (default) | remote */
    {
        const char *emb_provider = ca_config_get_str(ctx->config, "embedding.provider", "local");
        if (emb_provider && strcmp(emb_provider, "remote") == 0) {
            const char *emb_base = ca_config_get_str(ctx->config, "embedding.base_url", NULL);
            if (emb_base && *emb_base) {
                int rc = ca_embedding_use_remote(emb_base,
                            ca_config_get_str(ctx->config, "embedding.api_key", NULL),
                            ca_config_get_str(ctx->config, "embedding.model", NULL));
                ca_log_info("embedding provider=remote base=%s rc=%d", emb_base, rc);
            } else {
                ca_log_warn("embedding.provider=remote but embedding.base_url unset; using local");
                ca_embedding_use_local();
            }
        } else {
            ca_embedding_use_local();
        }
    }

    /* components */
    ctx->metrics = ca_metrics_new();
    ctx->bus = ca_event_bus_new();
    ctx->policy = ca_policy_engine_new();
    ca_policy_add_rule(ctx->policy, "*", "allow", "default allow (demo)");

    ctx->memory = ca_memory_new(ctx->state_root);
    ctx->snapshot = ca_snapshot_open(ctx->state_root);

    ctx->tools = ca_tool_registry_new();
    ca_tool_register_builtins(ctx->tools);

    ctx->llm = ca_llm_create(provider, base_url, api_key, model);
    if (!ctx->llm) {
        ca_log_error("cagent_init: unknown LLM provider '%s'", provider);
        cagent_shutdown(ctx);
        return -1;
    }
    ctx->txm = ca_tx_manager_new();

    /* multi-agent coordination: shared blackboard + agent pool + optional auth */
    ctx->blackboard = ca_blackboard_new();
    ctx->agents = ca_agent_pool_new();
    {
        const char *auth_key = ca_config_get_str(ctx->config, "auth.key", NULL);
        if (auth_key && *auth_key) {
            ctx->auth = ca_auth_new();
            ca_auth_add_key(ctx->auth, auth_key);
        }
    }

    /* observability / routing / plugin / skill / mcp / cluster / attention */
    ctx->trace = ca_trace_new(1024);
    ctx->router = ca_router_new();
    ctx->usage = ca_usage_new();
    ctx->registry = ca_plugin_registry_new();
    ctx->skills = ca_skill_registry_new();
    ctx->mcp = ca_mcp_manager_new();
    ctx->cluster = ca_cluster_new();
    ctx->attention = ca_attention_new();
    ctx->im = ca_im_new(ctx->state_root);
    ctx->channels = ca_im_channels_new(ctx->state_root);

    /* register the wasm3-backed sandbox Wasm runner */
    if (ca_wasm3_available()) ca_sandbox_set_wasm_runner(ca_wasm3_run);

    /* telegram inbound poller (joins fast when no telegram channels exist) */
    ctx->channels_stop = 0;
    ctx->channels_poller = ca_thread_create(channel_poller_loop, ctx);

    /* seed the route table with the configured provider so the Models UI is
     * populated even before explicit routes are added */
    if (ctx->router)
        ca_router_add(ctx->router, provider, provider, base_url, api_key, model, 1.0);

    /* built-in demo skill (cross-platform: echo works in cmd.exe and sh) */
    if (ctx->skills) {
        const ca_skill demo = { "echo_hello", "Reply with a fixed greeting.",
                                "shell", "echo hello from c-agent" };
        ca_skill_register(ctx->skills, &demo);
    }

    /* register the local node in the cluster */
    if (ctx->cluster)
        ca_cluster_upsert(ctx->cluster, "self", "127.0.0.1",
                          ctx->http_port, "coordinator");

    {
        ca_reasoning_config rc;
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
        ctx->reasoning = ca_reasoning_new(&rc);
    }
    if (!ctx->reasoning) {
        ca_log_error("cagent_init: reasoning engine failed to initialize");
        cagent_shutdown(ctx);
        return -1;
    }

    ctx->scheduler = ca_scheduler_new(ctx->workers, cagent_task_runner, ctx);
    if (!ctx->scheduler) {
        ca_log_error("cagent_init: scheduler failed to start");
        cagent_shutdown(ctx);
        return -1;
    }

    if (cagent_api_attach(ctx) != 0) {
        ca_log_error("cagent_init: failed to start HTTP API on port %u", (unsigned)ctx->http_port);
        cagent_shutdown(ctx);
        return -1;
    }

    ca_log_info("cagent %s initialized (provider=%s workers=%d tx=%s state=%s)",
                CAGENT_VERSION, provider, ctx->workers,
                ctx->use_transaction ? "on" : "off", ctx->state_root);
    if (ctx->bus) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "version", CAGENT_VERSION);
        cJSON_AddStringToObject(p, "provider", provider);
        ca_event_bus_publish(ctx->bus, CA_EV_SYSTEM, "cagent", p);
    }
    return 0;
}

void cagent_shutdown(cagent_ctx *ctx) {
    if (!ctx) return;
    cagent_stop(ctx);
    if (ctx->http) { ca_http_server_free(ctx->http); ctx->http = NULL; }
    if (ctx->scheduler) { ca_scheduler_shutdown(ctx->scheduler, 3000); }
    if (ctx->scheduler) { ca_scheduler_free(ctx->scheduler); ctx->scheduler = NULL; }

    ca_reasoning_free(ctx->reasoning);
    ctx->reasoning = NULL;
    ca_llm_destroy(ctx->llm);
    ctx->llm = NULL;
    ca_tx_manager_free(ctx->txm);
    ctx->txm = NULL;
    if (ctx->auth) { ca_auth_free(ctx->auth); ctx->auth = NULL; }
    if (ctx->agents) { ca_agent_pool_free(ctx->agents); ctx->agents = NULL; }
    if (ctx->blackboard) { ca_blackboard_free(ctx->blackboard); ctx->blackboard = NULL; }
    ca_tool_registry_free(ctx->tools);
    ctx->tools = NULL;
    if (ctx->snapshot) ca_snapshot_close(ctx->snapshot);
    ctx->snapshot = NULL;
    if (ctx->memory) ca_memory_free(ctx->memory);
    ctx->memory = NULL;
    if (ctx->policy) ca_policy_engine_free(ctx->policy);
    ctx->policy = NULL;
    if (ctx->bus) ca_event_bus_free(ctx->bus);
    ctx->bus = NULL;
    if (ctx->metrics) ca_metrics_free(ctx->metrics);
    ctx->metrics = NULL;
    if (ctx->attention) { ca_attention_free(ctx->attention); ctx->attention = NULL; }
    if (ctx->im) { ca_im_free(ctx->im); ctx->im = NULL; }
    /* stop and join the channel poller before freeing the channel registry */
    ctx->channels_stop = 1;
    if (ctx->channels_poller) { ca_thread_join(ctx->channels_poller); ctx->channels_poller = NULL; }
    if (ctx->channels) { ca_im_channels_free(ctx->channels); ctx->channels = NULL; }
    if (ctx->cluster) { ca_cluster_free(ctx->cluster); ctx->cluster = NULL; }
    if (ctx->mcp) { ca_mcp_manager_free(ctx->mcp); ctx->mcp = NULL; }
    if (ctx->skills) { ca_skill_registry_free(ctx->skills); ctx->skills = NULL; }
    if (ctx->registry) { ca_plugin_registry_free(ctx->registry); ctx->registry = NULL; }
    if (ctx->usage) { ca_usage_free(ctx->usage); ctx->usage = NULL; }
    if (ctx->router) { ca_router_free(ctx->router); ctx->router = NULL; }
    if (ctx->trace) { ca_trace_free(ctx->trace); ctx->trace = NULL; }
    if (ctx->config) ca_config_free(ctx->config);
    ctx->config = NULL;

    ca_mutex_destroy(&ctx->run_lock);
    free(ctx->state_root);
    free(ctx->workspace);
    free(ctx->provider);
    free(ctx->http_bind);
    ctx->state_root = ctx->workspace = ctx->provider = ctx->http_bind = NULL;
    ca_sock_cleanup();
    memset(ctx, 0, sizeof(*ctx));
}

int cagent_set_llm(cagent_ctx *ctx, const char *provider, const char *base_url,
                   const char *model, const char *api_key) {
    if (!ctx || !provider || !*provider) return -1;
    ca_llm *nl = ca_llm_create(provider, base_url, api_key, model);
    if (!nl) return -1;

    ca_mutex_lock(&ctx->run_lock);
    ca_llm *old = ctx->llm;
    ctx->llm = nl;
    if (ctx->reasoning) ca_reasoning_set_llm(ctx->reasoning, nl);
    /* keep the route table in sync so the Models UI reflects the active LLM */
    if (ctx->router) {
        ca_router_remove(ctx->router, provider);
        ca_router_add(ctx->router, provider, provider, base_url, api_key, model, 1.0);
    }
    ca_mutex_unlock(&ctx->run_lock);
    if (old) ca_llm_destroy(old);

    if (ctx->config) {
        ca_config_set_str(ctx->config, "llm.provider", provider);
        ca_config_set_str(ctx->config, "llm.model", model);
        ca_config_set_str(ctx->config, "llm.base_url", base_url);
        ca_config_set_str(ctx->config, "llm.api_key", api_key);
        char cfgfile[600];
        ca_path_join(cfgfile, sizeof(cfgfile), ctx->state_root, "cagent.json");
        if (ca_config_save_file(ctx->config, cfgfile) != 0)
            ca_log_warn("cagent_set_llm: could not persist config to %s", cfgfile);
    }
    ca_log_info("cagent: active LLM switched to provider=%s model=%s",
                provider, model ? model : "(default)");
    return 0;
}

int cagent_run(cagent_ctx *ctx, const char *prompt, char **answer) {
    if (!ctx || !prompt || !ctx->reasoning) return -1;
    ca_mutex_lock(&ctx->run_lock);
    int rc = ca_reasoning_run(ctx->reasoning, prompt, answer);
    ca_mutex_unlock(&ctx->run_lock);
    return rc;
}

int cagent_serve(cagent_ctx *ctx) {
    if (!ctx || !ctx->http) return -1;
    ca_log_info("cagent HTTP API serving on port %u", (unsigned)ctx->http_port);
    return ca_http_server_serve(ctx->http);
}

void cagent_stop(cagent_ctx *ctx) {
    if (ctx && ctx->http) ca_http_server_stop(ctx->http);
}
