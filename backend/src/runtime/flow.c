/* flow.c — Flow Compiler MVP (see flow.h for the contract).
 *
 * Execution model: Kahn's algorithm for cycle detection and layering; each
 * topological layer runs in parallel via threads, mirroring the orchestrator's
 * per-agent isolation (independent history/session notes, shared locked
 * infra, no shared code index / snapshot). */
#include "cognitive-os-agent/runtime/flow.h"

#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/infra/logging.h"
#include "cognitive-os-agent/os/os_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "cJSON.h"

#define FLOW_MAX_NODES   16
#define FLOW_MAX_TASKLEN 2048
#define FLOW_SUBST_CAP   4096   /* max chars substituted per {{ref}} */

static void flow_event(coa_ctx *ctx, const char *stage, const char *id,
                       const char *agent, const char *detail) {
    if (!ctx || !ctx->bus) return;
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "stage", stage);
    if (id) cJSON_AddStringToObject(o, "node", id);
    if (agent) cJSON_AddStringToObject(o, "agent", agent);
    if (detail) cJSON_AddStringToObject(o, "detail", detail);
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (js) {
        coa_event_bus_publish_json(ctx->bus, COA_EV_SYSTEM, "flow", js);
        free(js);
    }
}

static void flow_err(char **err, const char *fmt, ...) {
    if (!err) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    *err = coa_strdup(buf);
}

/* ---------- DAG parsing ---------- */

typedef struct flow_node {
    char id[64];
    char agent[64];
    char task[FLOW_MAX_TASKLEN];
    int  layer;
    int  indeg;
    int  nadj;                       /* out-degree */
    int  adj[FLOW_MAX_NODES];        /* successor indices */
} flow_node;

typedef struct flow_dag {
    int n;
    flow_node nodes[FLOW_MAX_NODES];
} flow_dag;

static int flow_node_index(flow_dag *d, const char *id) {
    for (int i = 0; i < d->n; i++)
        if (strcmp(d->nodes[i].id, id) == 0) return i;
    return -1;
}

/* Parse the JSON document into `d`. Returns 0 ok; on failure -1 with *err. */
static int flow_parse(const char *dag_json, flow_dag *d, char **err) {
    memset(d, 0, sizeof(*d));
    cJSON *root = cJSON_Parse(dag_json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        flow_err(err, "invalid JSON document");
        return -1;
    }
    cJSON *jnodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
    if (!jnodes || !cJSON_IsArray(jnodes) || cJSON_GetArraySize(jnodes) == 0) {
        cJSON_Delete(root);
        flow_err(err, "missing 'nodes' array");
        return -1;
    }
    if (cJSON_GetArraySize(jnodes) > FLOW_MAX_NODES) {
        cJSON_Delete(root);
        flow_err(err, "too many nodes (max %d)", FLOW_MAX_NODES);
        return -1;
    }
    cJSON *jn;
    cJSON_ArrayForEach(jn, jnodes) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(jn, "id");
        cJSON *jag = cJSON_GetObjectItemCaseSensitive(jn, "agent");
        cJSON *jtk = cJSON_GetObjectItemCaseSensitive(jn, "task");
        if (!cJSON_IsString(jid) || !*jid->valuestring ||
            !cJSON_IsString(jag) || !*jag->valuestring ||
            !cJSON_IsString(jtk) || !*jtk->valuestring) {
            cJSON_Delete(root);
            flow_err(err, "each node needs string 'id', 'agent' and 'task'");
            return -1;
        }
        flow_node *nd = &d->nodes[d->n];
        snprintf(nd->id, sizeof(nd->id), "%s", jid->valuestring);
        snprintf(nd->agent, sizeof(nd->agent), "%s", jag->valuestring);
        snprintf(nd->task, sizeof(nd->task), "%s", jtk->valuestring);
        d->n++;
    }
    /* unique ids */
    for (int i = 0; i < d->n; i++)
        for (int j = i + 1; j < d->n; j++)
            if (strcmp(d->nodes[i].id, d->nodes[j].id) == 0) {
                cJSON_Delete(root);
                flow_err(err, "duplicate node id '%s'", d->nodes[i].id);
                return -1;
            }

    /* edges: [{"from","to"}] or [["from","to"]] */
    cJSON *jedges = cJSON_GetObjectItemCaseSensitive(root, "edges");
    if (jedges && cJSON_IsArray(jedges)) {
        cJSON *je;
        cJSON_ArrayForEach(je, jedges) {
            const char *from = NULL, *to = NULL;
            if (cJSON_IsObject(je)) {
                cJSON *jf = cJSON_GetObjectItemCaseSensitive(je, "from");
                cJSON *jt = cJSON_GetObjectItemCaseSensitive(je, "to");
                if (cJSON_IsString(jf)) from = jf->valuestring;
                if (cJSON_IsString(jt)) to = jt->valuestring;
            } else if (cJSON_IsArray(je) && cJSON_GetArraySize(je) == 2) {
                cJSON *jf = cJSON_GetArrayItem(je, 0);
                cJSON *jt = cJSON_GetArrayItem(je, 1);
                if (cJSON_IsString(jf)) from = jf->valuestring;
                if (cJSON_IsString(jt)) to = jt->valuestring;
            }
            if (!from || !to) {
                cJSON_Delete(root);
                flow_err(err, "each edge needs 'from' and 'to'");
                return -1;
            }
            int fi = flow_node_index(d, from), ti = flow_node_index(d, to);
            if (fi < 0 || ti < 0) {
                /* from/to borrow into the cJSON tree — copy before freeing it */
                char fbuf[128], tbuf[128];
                snprintf(fbuf, sizeof(fbuf), "%s", from);
                snprintf(tbuf, sizeof(tbuf), "%s", to);
                cJSON_Delete(root);
                flow_err(err, "edge references unknown node ('%s' -> '%s')",
                         fbuf, tbuf);
                return -1;
            }
            if (d->nodes[fi].nadj >= FLOW_MAX_NODES) continue;
            d->nodes[fi].adj[d->nodes[fi].nadj++] = ti;
            d->nodes[ti].indeg++;
        }
    }
    cJSON_Delete(root);
    return 0;
}

/* Kahn's algorithm: assign layers (longest path from sources); detect cycles.
 * Returns number of nodes assigned (< n means a cycle exists). */
static int flow_layer(flow_dag *d) {
    int queue[FLOW_MAX_NODES], qh = 0, qt = 0, done = 0;
    for (int i = 0; i < d->n; i++) {
        d->nodes[i].layer = 0;
        if (d->nodes[i].indeg == 0) queue[qt++] = i;
    }
    /* working copy of indeg so the dag stays reusable */
    int indeg[FLOW_MAX_NODES];
    for (int i = 0; i < d->n; i++) indeg[i] = d->nodes[i].indeg;
    while (qh < qt) {
        int u = queue[qh++];
        done++;
        for (int k = 0; k < d->nodes[u].nadj; k++) {
            int v = d->nodes[u].adj[k];
            if (d->nodes[u].layer + 1 > d->nodes[v].layer)
                d->nodes[v].layer = d->nodes[u].layer + 1;
            if (--indeg[v] == 0) queue[qt++] = v;
        }
    }
    return done;
}

/* ---------- execution ---------- */

typedef struct flow_job {
    coa_ctx *ctx;
    flow_node *nd;
    char task[FLOW_MAX_TASKLEN];     /* after {{ref}} substitution */
    char *out;
    int rc;
} flow_job;

/* Same isolation rules as the orchestrator's per-agent instances. */
static coa_reasoning *flow_reasoning_new(coa_ctx *ctx) {
    coa_reasoning_config rc;
    memset(&rc, 0, sizeof(rc));
    rc.llm = ctx->llm;
    rc.tools = ctx->tools;
    rc.memory = ctx->memory;
    rc.policy = ctx->policy;
    rc.bus = ctx->bus;
    rc.metrics = ctx->metrics;
    rc.workspace = ctx->workspace;
    rc.skills = ctx->skills;
    rc.mcp = ctx->mcp;
    rc.state_root = ctx->state_root;
    rc.max_rounds = ctx->config
        ? (int)coa_config_get_int(ctx->config, "reasoning.max_rounds", 8) : 8;
    return coa_reasoning_new(&rc);
}

static void flow_worker(void *arg) {
    flow_job *j = (flow_job *)arg;
    flow_event(j->ctx, "execute", j->nd->id, j->nd->agent, j->task);
    coa_reasoning *r = flow_reasoning_new(j->ctx);
    if (r) {
        j->rc = coa_reasoning_run(r, j->task, &j->out);
        coa_reasoning_free(r);
    } else {
        j->rc = -1;
    }
    const char *result = j->out && *j->out ? j->out : "";
    char key[96];
    snprintf(key, sizeof(key), "flow/%s/result", j->nd->id);
    coa_blackboard_put(j->ctx->blackboard, key, result);
    flow_event(j->ctx, "done", j->nd->id, j->nd->agent,
               j->rc == 0 ? "ok" : "error");
}

/* Replace "{{<id>}}" in the node's task with upstream results (capped). */
static void flow_substitute(flow_dag *d, flow_node *nd, char **results,
                            char *out, size_t outsz) {
    const char *t = nd->task;
    size_t o = 0;
    out[0] = '\0';
    while (*t && o + 1 < outsz) {
        if (t[0] == '{' && t[1] == '{') {
            const char *close = strstr(t + 2, "}}");
            if (close) {
                size_t idlen = (size_t)(close - (t + 2));
                char ref[64];
                if (idlen > 0 && idlen < sizeof(ref)) {
                    memcpy(ref, t + 2, idlen);
                    ref[idlen] = '\0';
                    int ri = flow_node_index(d, ref);
                    if (ri >= 0 && results[ri]) {
                        size_t cap = outsz - o - 1;
                        size_t len = strlen(results[ri]);
                        if (len > FLOW_SUBST_CAP) len = FLOW_SUBST_CAP;
                        if (len > cap) len = cap;
                        /* strncat would scan for a NUL that is not written
                         * at out+o — copy directly instead. */
                        memcpy(out + o, results[ri], len);
                        o += len;
                        out[o] = '\0';
                        t = close + 2;
                        continue;
                    }
                }
            }
        }
        out[o++] = *t++;
    }
    out[o] = '\0';
}

int coa_flow_validate(const char *dag_json, char **err) {
    if (err) *err = NULL;
    if (!dag_json || !*dag_json) {
        flow_err(err, "empty flow document");
        return -1;
    }
    flow_dag d;
    if (flow_parse(dag_json, &d, err) != 0) return -1;
    if (flow_layer(&d) < d.n) {
        flow_err(err, "cycle detected in flow graph");
        return -1;
    }
    return 0;
}

int coa_flow_run(coa_ctx *ctx, const char *dag_json, char **answer,
                char **trace_json) {
    if (answer) *answer = NULL;
    if (trace_json) *trace_json = NULL;
    if (!ctx || !dag_json || !*dag_json || !answer) return -1;

    flow_dag d;
    char *err = NULL;
    if (flow_parse(dag_json, &d, &err) != 0) {
        coa_log_warn("flow: parse failed: %s", err ? err : "?");
        free(err);
        return -1;
    }
    if (flow_layer(&d) < d.n) {
        coa_log_warn("flow: cycle detected");
        return -1;
    }
    /* every node's agent must be registered */
    for (int i = 0; i < d.n; i++) {
        if (coa_agent_pool_find(ctx->agents, d.nodes[i].agent) < 0) {
            coa_log_warn("flow: node '%s' references unregistered agent '%s'",
                        d.nodes[i].id, d.nodes[i].agent);
            return -1;
        }
    }

    int maxlayer = 0;
    for (int i = 0; i < d.n; i++)
        if (d.nodes[i].layer > maxlayer) maxlayer = d.nodes[i].layer;

    char **results = (char **)calloc((size_t)d.n, sizeof(char *));
    char **tasks = (char **)calloc((size_t)d.n, sizeof(char *));
    flow_job *jobs = (flow_job *)calloc((size_t)d.n, sizeof(flow_job));
    if (!results || !tasks || !jobs) {
        free(results); free(tasks); free(jobs);
        return -1;
    }

    for (int L = 0; L <= maxlayer; L++) {
        int layern = 0, runidx[FLOW_MAX_NODES];
        for (int i = 0; i < d.n; i++)
            if (d.nodes[i].layer == L) runidx[layern++] = i;
        /* substitute upstream results into task templates, set up jobs */
        for (int k = 0; k < layern; k++) {
            int i = runidx[k];
            tasks[i] = (char *)malloc(FLOW_MAX_TASKLEN);
            if (tasks[i]) flow_substitute(&d, &d.nodes[i], results, tasks[i],
                                          FLOW_MAX_TASKLEN);
            else tasks[i] = coa_strdup(d.nodes[i].task);
            jobs[i].ctx = ctx;
            jobs[i].nd = &d.nodes[i];
            snprintf(jobs[i].task, FLOW_MAX_TASKLEN, "%s",
                     tasks[i] ? tasks[i] : d.nodes[i].task);
            jobs[i].out = NULL;
            jobs[i].rc = -1;
        }
        /* parallel within the layer */
        coa_thread *threads[FLOW_MAX_NODES];
        memset(threads, 0, sizeof(threads));
        for (int k = 0; k < layern; k++) {
            int i = runidx[k];
            threads[k] = coa_thread_create(flow_worker, &jobs[i]);
            if (!threads[k]) flow_worker(&jobs[i]); /* spawn failed → inline */
        }
        for (int k = 0; k < layern; k++) {
            int i = runidx[k];
            if (threads[k]) coa_thread_join(threads[k]);
            results[i] = jobs[i].out; /* may be NULL on failure */
        }
    }

    /* trace + answer (sink nodes = no outgoing edges) */
    cJSON *trace = cJSON_CreateArray();
    coa_strbuf fin;
    coa_strbuf_init(&fin);
    for (int i = 0; i < d.n; i++) {
        const char *result = results[i] ? results[i] : "";
        cJSON *st = cJSON_CreateObject();
        if (st) {
            cJSON_AddStringToObject(st, "id", d.nodes[i].id);
            cJSON_AddStringToObject(st, "agent", d.nodes[i].agent);
            cJSON_AddStringToObject(st, "task", tasks[i] ? tasks[i] : d.nodes[i].task);
            cJSON_AddNumberToObject(st, "layer", (double)d.nodes[i].layer);
            cJSON_AddStringToObject(st, "status", jobs[i].rc == 0 ? "ok" : "error");
            cJSON_AddStringToObject(st, "result", result);
            cJSON_AddItemToArray(trace, st);
        }
        if (d.nodes[i].nadj == 0 && *result)
            coa_strbuf_appendf(&fin, "%s: %s\n", d.nodes[i].id, result);
        /* publish per-node result to the agent pool like agent runs do */
        if (jobs[i].rc == 0 && results[i] && *results[i]) {
            char rk[160];
            snprintf(rk, sizeof(rk), "result:%s", d.nodes[i].agent);
            coa_agent_post(ctx->agents, d.nodes[i].agent, rk, results[i]);
        }
    }

    char *trace_str = cJSON_PrintUnformatted(trace);
    if (trace_str) {
        coa_blackboard_put(ctx->blackboard, "flow/trace", trace_str);
        if (trace_json) *trace_json = coa_strdup(trace_str);
        free(trace_str);
    }
    cJSON_Delete(trace);
    for (int i = 0; i < d.n; i++) free(results[i]);
    for (int i = 0; i < d.n; i++) free(tasks[i]);
    free(results);
    free(tasks);
    free(jobs);

    *answer = fin.len > 0 ? fin.buf : coa_strdup("(flow produced no output)");
    return 0;
}
