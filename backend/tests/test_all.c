/* test_all.c — unit tests for cognitive-os-agent modules.
 * Build with: zig cc -Iinclude -Ithird_party/cJSON $(find src third_party -name "*.c")
 * or via build.sh test. Exit 0 = all green, non-zero = failure. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* setenv/unsetenv under -std=c11 */
#endif
#include "cognitive-os-agent/runtime/event_bus.h"
#include "cognitive-os-agent/runtime/flow.h"
#include "cognitive-os-agent/runtime/scheduler.h"
#include "cognitive-os-agent/runtime/state_machine.h"
#include "cognitive-os-agent/runtime/policy_engine.h"
#include "cognitive-os-agent/memory/memory.h"
#include "cognitive-os-agent/snapshot/snapshot.h"
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/tx/tx.h"
#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/retrieval/engine.h"
#include "cognitive-os-agent/cognition/blackboard.h"
#include "cognitive-os-agent/runtime/agent.h"
#include "cognitive-os-agent/api/auth.h"
#include "cognitive-os-agent/api/websocket.h"
#include "cognitive-os-agent/plugin_runtime/manager.h"
#include "cognitive-os-agent/infra/metrics.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/infra/config.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/os/os_coro.h"
#include "cognitive-os-agent/memory/kv.h"
#include "cognitive-os-agent/memory/episode.h"
#include "cognitive-os-agent/memory/vector.h"
#include "cognitive-os-agent/memory/graph.h"
#include "cognitive-os-agent/retrieval/context_builder.h"
#include "cognitive-os-agent/cognition/planner.h"
#include "cognitive-os-agent/cognition/evaluator.h"
#include "cognitive-os-agent/cognition/reasoning.h"
#include "cognitive-os-agent/plugin_runtime/sandbox.h"
#include "cognitive-os-agent/plugin_runtime/filetracker.h"
#include "cognitive-os-agent/plugin_runtime/capability.h"
#include "cognitive-os-agent/plugin_runtime/registry.h"
#include "cognitive-os-agent/plugin_intelligence/analyzer.h"
#include "cognitive-os-agent/plugin_intelligence/architect.h"
#include "cognitive-os-agent/plugin_intelligence/codegen.h"
#include "cognitive-os-agent/plugin_intelligence/testing.h"
#include "cognitive-os-agent/plugin_intelligence/security.h"
#include "cognitive-os-agent/action/skill.h"
#include "cognitive-os-agent/action/mcp_conn.h"
#include "cognitive-os-agent/cluster/node.h"
#include "cognitive-os-agent/cognition/attention.h"
#include "cognitive-os-agent/infra/trace.h"
#include "cognitive-os-agent/llm/router.h"
#include "cognitive-os-agent/llm/usage.h"
#include "cognitive-os-agent/runtime/task.h"
#include "cognitive-os-agent/infra/ringbuf.h"
#include "cognitive-os-agent/retrieval/embedding.h"
#include "cognitive-os-agent/execution/executor.h"
#include "cognitive-os-agent/im/im.h"
#include "cognitive-os-agent/plugin_intelligence/generator.h"
#include "cognitive-os-agent/plugin_runtime/wasm_runner.h"
#include "cognitive-os-agent/cognitive-os-agent.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/os/os_socket.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_proc.h"
#include "cognitive-os-agent/api/http_server.h"
#include "cognitive-os-agent/infra/catalog.h"
#include "cognitive-os-agent/infra/audit.h"
#include "cognitive-os-agent/llm/sse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "cJSON.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) do {                                                    \
    if (cond) { g_pass++; }                                                 \
    else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_STR(a, b) do {                                                \
    const char *_a = (a), *_b = (b);                                        \
    if (_a && _b && strcmp(_a, _b) == 0) { g_pass++; }                      \
    else { g_fail++; printf("  FAIL %s:%d  '%s' != '%s'\n",                 \
           __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); }   \
} while (0)

static void section(const char *name) { printf("\n== %s ==\n", name); }

/* ---------- infra: util ---------- */
static void test_util(void) {
    section("util");
    char out[256];
    path_join(out, sizeof(out), "a", "b.txt");
#if defined(_WIN32)
    CHECK_STR(out, "a\\b.txt");
#else
    CHECK_STR(out, "a/b.txt");
#endif
    /* alias-safe: out may be the same buffer as the input base */
    char p[256];
    snprintf(p, sizeof(p), "x/y");
    path_join(p, sizeof(p), p, "z.txt");
    CHECK(strstr(p, "z.txt") != NULL);

    /* resolve against a workspace */
    char r[256];
    path_resolve(r, sizeof(r), "w", "sub/f.txt");
    CHECK(strstr(r, "sub/f.txt") != NULL);
    path_resolve(r, sizeof(r), "w", "/abs/path");
    CHECK_STR(r, "/abs/path");

    CHECK(hash64("hello", 5) != 0);
    char hex[17];
    hash_hex(hex, 0xDEADBEEFDEADBEEFULL);
    CHECK(strlen(hex) == 16);

    /* UTF-8 validation + sanitization (GBK console output must not poison
     * LLM prompts — providers hard-reject invalid UTF-8) */
    CHECK(str_utf8_valid_n("hello \xE4\xBD\xA0\xE5\xA5\xBD", -1) == 1);
    CHECK(str_utf8_valid_n("\xC4\xE3\xBA\xC3", -1) == 0);   /* GBK 你好 */
    CHECK(str_utf8_valid_n("\xE4\xBD", -1) == 0);           /* truncated seq */
    CHECK(str_utf8_valid_n("\xC0\x80", -1) == 0);           /* overlong */
    char *san = str_utf8_sanitize("a\xC4" "\xE3" "b");
    CHECK(san != NULL && strcmp(san, "a??b") == 0);
    free(san);
    san = str_utf8_sanitize("\xE4\xBD\xA0\xE5\xA5\xBD");
    CHECK(san != NULL && strcmp(san, "\xE4\xBD\xA0\xE5\xA5\xBD") == 0);
    free(san);

    strbuf b;
    strbuf_init(&b);
    strbuf_append(&b, "a");
    strbuf_appendf(&b, "-%d", 42);
    CHECK_STR(b.buf, "a-42");
    char *det = strbuf_detach(&b);
    CHECK_STR(det, "a-42");
    free(det);

    strmap m;
    memset(&m, 0, sizeof(m));
    strmap_set(&m, "k", "v1");
    strmap_set(&m, "k", "v2");
    CHECK_STR(strmap_get(&m, "k"), "v2");
    CHECK(strmap_get(&m, "missing") == NULL);
    strmap_free(&m);
}

/* ---------- core: event bus ---------- */
static int ev_count = 0;
static int ev_types[16];
static void on_ev(const event *ev, void *ud) {
    (void)ud;
    if (ev_count < 16) ev_types[ev_count] = (int)ev->type;
    ev_count++;
}

static void test_event_bus(void) {
    section("event_bus");
    event_bus *b = event_bus_new();
    CHECK(b != NULL);
    event_bus_subscribe(b, -1, on_ev, NULL);
    event_bus_publish_json(b, EV_TOOL, "test", "{\"tool\":\"file_read\"}");
    event_bus_publish_json(b, EV_MEMORY, "test", "{\"k\":\"v\"}");
    CHECK(ev_count == 2);
    CHECK(ev_types[0] == EV_TOOL);
    CHECK(ev_types[1] == EV_MEMORY);
    event_bus_free(b);
}

/* ---------- core: scheduler ---------- */
static void run_fast(task *t, scheduler *s, void *ud) {
    (void)s; (void)ud;
    t->output = xstrdup("ran");
    t->status = TS_DONE;
}

static void test_scheduler(void) {
    section("scheduler");
    scheduler *s = scheduler_new(2, run_fast, NULL);
    int64_t id = scheduler_submit(s, 0, "job", NULL, 0);
    CHECK(id >= 0);
    CHECK(scheduler_wait_idle(s, 3000) == 0);
    task *t = scheduler_get(s, id);
    CHECK(t != NULL);
    CHECK(t->status == TS_DONE);
    if (t) {
        CHECK_STR(t->output, "ran");
        free(t->output);
        t->output = NULL;
    }
    CHECK(scheduler_total(s) == 1);
    CHECK(scheduler_shutdown(s, 3000) == 0);
    scheduler_free(s);
}

/* ---------- os: stackful coroutine ---------- */
static int coro_steps[8];
static int coro_step_count = 0;

static void coro_body(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) {
        coro_steps[coro_step_count++] = i;
        coro_yield();
    }
    coro_steps[coro_step_count++] = 99; /* terminal marker */
}

static void test_coro(void) {
    section("coro");
    coro_step_count = 0;
    coro *c = coro_new(coro_body, NULL, 0);
    CHECK(c != NULL);
    if (!c) return;
    CHECK(coro_done(c) == 0);

    coro_resume(c);              /* run until first yield: steps[0]=0 */
    CHECK(coro_done(c) == 0);
    CHECK(coro_step_count == 1);
    CHECK(coro_steps[0] == 0);

    coro_resume(c);              /* steps[1]=1 */
    CHECK(coro_step_count == 2);
    CHECK(coro_steps[1] == 1);

    coro_resume(c);              /* steps[2]=2 */
    CHECK(coro_step_count == 3);
    CHECK(coro_steps[2] == 2);

    coro_resume(c);              /* finish: steps[3]=99, done=1 */
    CHECK(coro_done(c) == 1);
    CHECK(coro_step_count == 4);
    CHECK(coro_steps[3] == 99);

    coro_free(c);
}

/* ---------- core: M:N scheduler (M coroutine tasks on N threads) ---------- */
#define MN_COUNT 40
static int mn_runs[MN_COUNT];

static void mn_runner(task *t, scheduler *s, void *ud) {
    (void)s; (void)ud;
    int idx = (int)(intptr_t)t->userdata;
    mn_runs[idx]++;                 /* must run exactly once (yield resumes, not restarts) */
    scheduler_yield();           /* cooperative yield mid-task */
    char buf[32];
    snprintf(buf, sizeof(buf), "task-%d", idx);
    t->output = xstrdup(buf);
    t->status = TS_DONE;
}

static void test_scheduler_mn(void) {
    section("scheduler_mn");
    memset(mn_runs, 0, sizeof(mn_runs));
    scheduler *s = scheduler_new(2, mn_runner, NULL);
    for (int i = 0; i < MN_COUNT; i++)
        CHECK(scheduler_submit(s, 0, "t", (void *)(intptr_t)i, 0) >= 0);
    CHECK(scheduler_wait_idle(s, 5000) == 0);
    for (int i = 0; i < MN_COUNT; i++) {
        task *t = scheduler_get(s, i);
        CHECK(t != NULL);
        if (t) {
            CHECK(t->status == TS_DONE);
            char buf[32];
            snprintf(buf, sizeof(buf), "task-%d", i);
            CHECK_STR(t->output, buf);
            free(t->output);
            t->output = NULL;
        }
        CHECK(mn_runs[i] == 1);
    }
    CHECK(scheduler_total(s) == MN_COUNT);
    CHECK(scheduler_shutdown(s, 5000) == 0);
    scheduler_free(s);
}

/* ---------- core: state machine ---------- */
static int h_reason(state_machine *sm, void *ud, const char *in, char **out) {
    (void)sm; (void)ud;
    *out = xstrdup(in);      /* pass through */
    return 0;
}
static int h_fail(state_machine *sm, void *ud, const char *in, char **out) {
    (void)sm; (void)ud; (void)in; (void)out;
    return -1;                  /* force FAILED */
}

static void test_state_machine(void) {
    section("state_machine");
    state_machine *sm = state_machine_new();
    state_machine_set_handler(sm, ST_REASON, h_reason, NULL);
    char *res = NULL;
    state fin = state_machine_run(sm, "input", &res);
    CHECK(fin == ST_DONE);
    CHECK_STR(res, "input");
    free(res);

    state_machine *sm2 = state_machine_new();
    state_machine_set_handler(sm2, ST_ACT, h_fail, NULL);
    char *res2 = NULL;
    state fin2 = state_machine_run(sm2, "x", &res2);
    CHECK(fin2 == ST_FAILED);
    state_machine_free(sm2);
    state_machine_free(sm);
}

/* ---------- core: policy engine ---------- */
static void test_policy(void) {
    section("policy");
    policy_engine *pe = policy_engine_new();
    policy_add_rule(pe, "*", "allow", "default allow");
    CHECK(policy_check(pe, "shell", "{}", NULL) == POLICY_ALLOW);

    policy_engine *pe2 = policy_engine_new();
    policy_add_rule(pe2, "shell", "deny", "no shell");
    CHECK(policy_check(pe2, "shell", "{}", NULL) == POLICY_DENY);
    /* unmatched tool with rules present defaults to ASK */
    CHECK(policy_check(pe2, "file_read", "{}", NULL) == POLICY_ASK);
    policy_engine_free(pe2);

    int risk = policy_risk("shell", "{\"command\":\"rm -rf /\"}");
    CHECK(risk > 0 && risk <= 100);
    CHECK(policy_risk("file_read", "{\"path\":\"a.txt\"}") < risk);
    policy_engine_free(pe);
}

/* ---------- service: memory ---------- */
static void test_memory(void) {
    section("memory");
    const char *root = "state-test/memory";
    memory *m = memory_new(root);
    CHECK(m != NULL);
    memory_working_push(m, "item one");
    memory_working_push(m, "item two");
    memory_remember(m, "lang", "c");
    memory_remember(m, "x", "y");
    memory_remember(m, "x", NULL);        /* delete */
    CHECK_STR(memory_recall(m, "lang"), "c");
    CHECK(memory_recall(m, "x") == NULL);
    memory_record_experience(m, "task", "result");
    memory_flush(m);
    /* facade accessors over the sub-stores */
    CHECK(memory_working_count(m) == 2);
    CHECK_STR(memory_working_at(m, 0), "item two");
    CHECK_STR(memory_working_at(m, 1), "item one");
    char *ep = memory_episodes_json(m);
    CHECK(ep && strstr(ep, "task") != NULL && strstr(ep, "result") != NULL);
    free(ep);
    char *retr = memory_retrieve(m, "item", 3);
    CHECK(retr != NULL);
    free(retr);
    char *w = memory_working_json(m);
    CHECK(w && strstr(w, "item one") != NULL);
    free(w);
    char *l = memory_longterm_json(m);
    CHECK(l && strstr(l, "lang") != NULL);
    free(l);
    char *sr = memory_search(m, "item", 5);
    CHECK(sr != NULL);
    free(sr);
    memory_free(m);
}

/* ---------- snapshot + tx + tools ---------- */
static void test_snapshot_tx(void) {
    section("snapshot+tx");
    /* stale state from a previous run (committed file) would be pre-captured as
     * existing and rollback would restore instead of delete — clean it first. */
    fs_remove("state-test/w/f.txt");
    const char *root = "state-test/snapshot";
    snapshot *snap = snapshot_open(root);
    CHECK(snap != NULL);

    tool_registry *reg = tool_registry_new();
    tool_register_builtins(reg);
    CHECK(tool_registry_count(reg) == 9);  /* file_read/write/edit, shell, git, mcp, skill, glob, grep */
    CHECK(tool_find(reg, "file_read") != NULL);

    tx_manager *tm = tx_manager_new();
    tool_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.reg = reg;
    ctx.snapshot = snap;
    ctx.workspace = "state-test/w";
    fs_mkdirs("state-test/w");
    tx *tx = tx_begin(tm, snap, reg, &ctx);

    /* write a file inside the tx (workspace-relative path resolves to
     * state-test/w/f.txt, which the tool writes AND tx pre-captures) */
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"f.txt\",\"content\":\"hello\"}");
    int rc = tx_run(tx, "file_write", args);
    CHECK(rc == 0);
    CHECK(tx_validate(tx) == 1);

    /* verify the file exists */
    char *data = fs_read_file("state-test/w/f.txt");
    CHECK(data != NULL);
    CHECK_STR(data, "hello");
    free(data);

    /* rollback must delete it (captured as "to be created") */
    CHECK(tx_rollback(tx) == 0);
    CHECK(fs_exists("state-test/w/f.txt") == 0);

    /* commit path: write again, commit, verify persisted */
    struct tx *tx2 = tx_begin(tm, snap, reg, &ctx);
    rc = tx_run(tx2, "file_write", args);
    CHECK(rc == 0);
    CHECK(tx_commit(tx2) == 0);
    CHECK(fs_read_file("state-test/w/f.txt") != NULL);
    char *data2 = fs_read_file("state-test/w/f.txt");
    CHECK_STR(data2, "hello");
    free(data2);

    char *list = snapshot_list(snap);
    CHECK(list && strstr(list, "f.txt") != NULL);
    free(list);

    tx_free(tx2);
    tx_free(tx);
    tx_manager_free(tm);
    tool_registry_free(reg);
    snapshot_close(snap);
}

/* ---------- llm: mock provider ---------- */
static void test_llm_mock(void) {
    section("llm_mock");
    llm *llm = llm_create("mock", NULL, NULL, "mock");
    CHECK(llm != NULL);
    if (!llm) return;
    llm_message msgs[] = {
        {"system", "you are a planner"},
        {"user", "创建 test/note.txt 写入内容为 hello"},
    };
    llm_request req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.num_messages = 2;
    req.temperature = 0.0;
    llm_response resp;
    memset(&resp, 0, sizeof(resp));
    CHECK(llm_chat(llm, &req, &resp) == 0);
    CHECK(resp.content != NULL);
    /* mock should emit a plan JSON array */
    cJSON *arr = cJSON_Parse(resp.content ? resp.content : "[]");
    CHECK(arr != NULL);
    if (arr) {
        cJSON *first = arr->child;
        if (first) {
            cJSON *tool = cJSON_GetObjectItemCaseSensitive(first, "tool");
            CHECK(tool && cJSON_IsString(tool));
            if (tool) CHECK_STR(tool->valuestring, "file_write");
        }
        cJSON_Delete(arr);
    }
    free(resp.content);
    free(resp.error);
    llm_destroy(llm);
}

/* ---------- llm bridge: capabilities + cancel ---------- */
static void nop_stream_cb(const char *delta, void *ud) { (void)delta; (void)ud; }

static void test_llm_caps_cancel(void) {
    section("llm_caps_cancel");
    /* capability table per provider (no network: construction is offline) */
    llm *mock = llm_create("mock", NULL, NULL, "mock");
    CHECK(mock != NULL);
    if (mock) {
        const llm_caps *c = llm_capabilities(mock);
        CHECK(c && c->stream == 1 && c->tools == 0 && c->max_ctx == 8192);
        /* cancel set before the stream aborts it between deltas */
        llm_message msgs[] = {{"user", "hello"}};
        llm_request req;
        memset(&req, 0, sizeof(req));
        req.messages = msgs;
        req.num_messages = 1;
        llm_cancel(mock);
        CHECK(llm_stream(mock, &req, nop_stream_cb, NULL) == -1);
        /* next stream auto-clears the flag and succeeds */
        CHECK(llm_stream(mock, &req, nop_stream_cb, NULL) == 0);
        llm_destroy(mock);
    }
    llm *oai = llm_create("openai", "http://127.0.0.1:1", "k", "gpt-x");
    CHECK(oai != NULL);
    if (oai) {
        const llm_caps *c = llm_capabilities(oai);
        CHECK(c && c->stream == 1 && c->tools == 1 && c->max_ctx == 128000);
        llm_destroy(oai);
    }
    llm *ant = llm_create("anthropic", NULL, "k", NULL);
    CHECK(ant != NULL);
    if (ant) {
        const llm_caps *c = llm_capabilities(ant);
        CHECK(c && c->stream == 1 && c->tools == 1 && c->max_ctx == 200000);
        llm_destroy(ant);
    }
    CHECK(llm_capabilities(NULL) != NULL);
}

/* ---------- RAG retrieval upgrade: rerank / hybrid / MQE / retrieve_ex / HyDE ---------- */
static void test_retrieval_upgrade(void) {
    section("retrieval_upgrade");

    /* rerank: token overlap + bigram jaccard — near doc beats unrelated doc */
    const char *docs[] = {"deploy the backend service to the cluster",
                          "recipe for homemade pasta dough"};
    float rs[2] = {0, 0};
    CHECK(embed_rerank("deploy the backend service", docs, 2, rs) == 0);
    CHECK(rs[0] > rs[1]);
    CHECK(rs[0] <= 1.0f && rs[0] >= 0.0f && rs[1] >= 0.0f);
    /* public keyword score */
    float kw = -1;
    CHECK(embed_keyword_score("error log", "the error log shows a failure", &kw) == 0);
    CHECK(kw >= 0.0f && kw <= 1.0f);

    /* hybrid retrieval: keyword component rescues exact-token matches */
    vectorstore *v = vectorstore_new();
    CHECK(v != NULL);
    CHECK(vectorstore_add(v, "d1", "zeta widget calibration protocol", "w") == 0);
    CHECK(vectorstore_add(v, "d2", "calibrate the zeta widget yearly", "w") == 0);
    CHECK(vectorstore_add(v, "d3", "completely unrelated text about pasta", "w") == 0);
    CHECK(vectorstore_count(v) == 3);
    /* pure vector (w=1) */
    char *j = vectorstore_nearest_hybrid(v, "zeta widget calibration", 2, 1.0f);
    CHECK(j != NULL && strstr(j, "d1") != NULL);
    free(j);
    /* hybrid (w=0.5): keyword component keeps the exact match on top */
    j = vectorstore_nearest_hybrid(v, "zeta widget calibration", 2, 0.5f);
    CHECK(j != NULL && strstr(j, "d1") != NULL);
    free(j);
    /* clamping does not crash */
    j = vectorstore_nearest_hybrid(v, "zeta", 2, 42.0f);
    CHECK(j != NULL);
    free(j);
    /* edge cases */
    j = vectorstore_nearest_hybrid(v, "zeta", 0, 0.7f);
    CHECK(j != NULL && strcmp(j, "[]") == 0);
    free(j);

    /* multi-query merge: per-entry max score */
    const char *qs[] = {"pasta", "zeta widget"};
    j = vectorstore_nearest_multi(v, qs, 2, 3);
    CHECK(j != NULL);
    /* both query families surface; the pasta doc must appear via query 2 */
    CHECK(strstr(j, "d3") != NULL && strstr(j, "d1") != NULL);
    free(j);
    j = vectorstore_nearest_multi(v, NULL, 0, 3);
    CHECK(j != NULL && strcmp(j, "[]") == 0);
    free(j);
    vectorstore_free(v);

    /* retrieve_ex two-stage through the memory facade */
    memory *m = memory_new("state-test/retr");
    CHECK(m != NULL);
    CHECK(memory_index_document(m, "k1", "the deploy pipeline runs unit tests", "t") == 0);
    CHECK(memory_index_document(m, "k2", "kubernetes cluster autoscaling notes", "t") == 0);
    CHECK(memory_index_document(m, "k3", "deploy pipeline also builds docker images", "t") == 0);
    char *rj = memory_retrieve_ex(m, "deploy pipeline", 2, 0.7f);
    CHECK(rj != NULL && strstr(rj, "deploy pipeline") != NULL);
    free(rj);
    /* w_vec clamped extremes still work */
    rj = memory_retrieve_ex(m, "kubernetes", 1, 5.0f);
    CHECK(rj != NULL && strstr(rj, "kubernetes") != NULL);
    free(rj);
    /* MQE through the facade */
    const char *mq[] = {"docker", "unit tests"};
    rj = memory_retrieve_mqe(m, mq, 2, 2);
    CHECK(rj != NULL);
    CHECK(strstr(rj, "deploy") != NULL);
    free(rj);
    rj = memory_retrieve_mqe(m, mq, 0, 2);
    CHECK(rj != NULL && strcmp(rj, "[]") == 0);
    free(rj);
    memory_free(m);

    /* HyDE: mock llm produces a passage (offline) */
    llm *mock = llm_create("mock", NULL, NULL, "mock");
    CHECK(mock != NULL);
    char *passage = hyde_passage(mock, "how do I deploy the service?");
    CHECK(passage != NULL && *passage != '\0');
    free(passage);
    CHECK(hyde_passage(NULL, "q") == NULL);
    llm *nomock = llm_create("mock", NULL, NULL, "mock");
    CHECK(hyde_passage(nomock, NULL) == NULL);
    llm_destroy(nomock);
    llm_destroy(mock);
}

/* ---------- Context layer: unified KV/Task/Agent state store ---------- */
static void test_state_store(void) {
    section("state_store");
    state_store *s = state_store_new();
    CHECK(s != NULL);
    CHECK(state_store_count(s) == 0);
    /* generic kv */
    CHECK(state_store_set(s, "kv", "mode", "dark") == 0);
    CHECK(state_store_set(s, "kv", "volume", "70") == 0);
    CHECK(state_store_set(s, "kv", "mode", "light") == 0); /* update */
    CHECK(strcmp(state_store_get(s, "kv", "mode"), "light") == 0);
    CHECK(state_store_remove(s, "kv", "volume") == 0);
    CHECK(state_store_get(s, "kv", "volume") == NULL);
    CHECK(state_store_remove(s, "kv", "volume") == 0); /* absent: no-op */
    /* task + agent convenience slots */
    CHECK(state_store_task_set(s, 42, "DONE", "创建文件 a.txt") == 0);
    CHECK(state_store_agent_set(s, "planner", "planning", "idle") == 0);
    CHECK(state_store_count(s) == 3);
    CHECK(state_store_count_ns(s, "task") == 1);
    CHECK(state_store_count_ns(s, "agent") == 1);
    const char *tv = state_store_get(s, "task", "42");
    CHECK(tv && strncmp(tv, "DONE|", 5) == 0 && strstr(tv, "a.txt") != NULL);
    /* bad args */
    CHECK(state_store_set(NULL, "kv", "k", "v") == -1);
    CHECK(state_store_set(s, "", "k", "v") == -1);
    CHECK(state_store_set(s, "kv", "", "v") == -1);
    /* json roundtrip */
    char *js = state_store_json(s);
    CHECK(js && strstr(js, "\"task\"") && strstr(js, "\"kv\"") && strstr(js, "\"agent\""));
    state_store *s2 = state_store_new();
    CHECK(state_store_load_json(s2, js) == 3);
    CHECK(strcmp(state_store_get(s2, "kv", "mode"), "light") == 0);
    free(js);
    CHECK(state_store_load_json(s2, "not json") == -1);
    /* file save/load + auto-flush on mutation */
    fs_mkdirs("state-test");
    CHECK(state_store_save(s, "state-test/state-store.json") == 0);
    state_store *s3 = state_store_new();
    CHECK(state_store_load(s3, "state-test/state-store.json") == 0);
    CHECK(state_store_count(s3) == 3);
    CHECK(strcmp(state_store_get(s3, "agent", "planner"), "planning|idle") == 0);
    CHECK(state_store_set(s3, "kv", "auto", "flushed") == 0);
    state_store *s4 = state_store_new();
    CHECK(state_store_load(s4, "state-test/state-store.json") == 0);
    CHECK(strcmp(state_store_get(s4, "kv", "auto"), "flushed") == 0);
    state_store_free(s4);
    state_store_free(s3);
    state_store_free(s2);
    state_store_free(s);
}

/* ---------- Memory Service interface (type<->backend decoupling) ---------- */
typedef struct fake_kv { char *k[4]; char *v[4]; int n; } fake_kv;
static int fake_remember(void *impl, mem_type t, const char *key, const char *text) {
    (void)t;
    fake_kv *f = impl;
    if (f->n >= 4) return -1;
    f->k[f->n] = xstrdup(key ? key : "");
    f->v[f->n] = xstrdup(text);
    return f->k[f->n] && f->v[f->n] ? f->n++, 0 : -1;
}
static int fake_recall_key(void *impl, mem_type t, const char *key, char **text) {
    (void)t;
    fake_kv *f = impl;
    for (int i = 0; i < f->n; i++)
        if (strcmp(f->k[i], key) == 0) { *text = xstrdup(f->v[i]); return 0; }
    return -1;
}
static void fake_destroy(void *impl) {
    fake_kv *f = impl;
    for (int i = 0; i < f->n; i++) { free(f->k[i]); free(f->v[i]); }
    free(f);
}
static const memory_service_ops fake_ms_ops = {
    "fake", fake_remember, NULL, fake_recall_key, NULL, NULL, fake_destroy,
};

static void test_memory_service(void) {
    section("memory_service");
    /* default backend over the memory facade */
    memory *m = memory_new("state-test/memsvc");
    memory_service *ms = memory_service_new_default(m);
    CHECK(ms != NULL);
    CHECK(strcmp(memory_service_backend(ms), "default") == 0);
    /* working */
    CHECK(memory_service_remember(ms, MEM_WORKING, NULL, "scratch item") == 0);
    char *t = NULL;
    CHECK(memory_service_recall_key(ms, MEM_WORKING, "scratch item", &t) == 0);
    CHECK(t && strcmp(t, "scratch item") == 0);
    free(t);
    /* episodic */
    CHECK(memory_service_remember(ms, MEM_EPISODIC, "task A", "ok result") == 0);
    /* semantic */
    CHECK(memory_service_remember(ms, MEM_SEMANTIC, "lang", "c11") == 0);
    t = NULL;
    CHECK(memory_service_recall_key(ms, MEM_SEMANTIC, "lang", &t) == 0);
    CHECK(t && strcmp(t, "c11") == 0);
    free(t);
    /* procedural: key auto-prefixed with procedure. */
    CHECK(memory_service_remember(ms, MEM_PROCEDURAL, "deploy", "run build.sh") == 0);
    t = NULL;
    CHECK(memory_service_recall_key(ms, MEM_PROCEDURAL, "deploy", &t) == 0);
    CHECK(t && strcmp(t, "run build.sh") == 0);
    free(t);
    CHECK(memory_recall(m, "procedure.deploy") != NULL);
    /* already-prefixed key is not double-prefixed */
    CHECK(memory_service_remember(ms, MEM_PROCEDURAL, "procedure.x", "v") == 0);
    CHECK(memory_recall(m, "procedure.procedure.x") == NULL);
    /* forget semantic */
    CHECK(memory_service_forget(ms, MEM_SEMANTIC, "lang") == 0);
    CHECK(memory_service_recall_key(ms, MEM_SEMANTIC, "lang", &t) == -1);
    /* query recall: working / episodic filtered by kind */
    char *js = NULL;
    CHECK(memory_service_recall_query(ms, MEM_EPISODIC, "task A", 5, &js) == 0);
    CHECK(js && strstr(js, "ok result"));
    free(js);
    CHECK(memory_service_recall_query(ms, MEM_WORKING, "scratch", 5, &js) == 0);
    CHECK(js != NULL);
    free(js);
    /* stats: array covering all four types */
    CHECK(memory_service_stats(ms, &js) == 0);
    CHECK(js && strstr(js, "working") && strstr(js, "episodic") &&
          strstr(js, "semantic") && strstr(js, "procedural"));
    free(js);
    /* type helpers */
    mem_type ty;
    CHECK(mem_type_parse("procedural", &ty) == 0 && ty == MEM_PROCEDURAL);
    CHECK(mem_type_parse("nope", &ty) == -1);
    CHECK(strcmp(mem_type_name(MEM_WORKING), "working") == 0);
    memory_service_free(ms);
    memory_free(m);

    /* decoupling: a custom backend through the same interface */
    fake_kv *f = calloc(1, sizeof(*f));
    CHECK(f != NULL);
    memory_service *fs = memory_service_new(&fake_ms_ops, f);
    CHECK(fs && strcmp(memory_service_backend(fs), "fake") == 0);
    CHECK(memory_service_remember(fs, MEM_SEMANTIC, "k1", "v1") == 0);
    t = NULL;
    CHECK(memory_service_recall_key(fs, MEM_SEMANTIC, "k1", &t) == 0);
    CHECK(t && strcmp(t, "v1") == 0);
    free(t);
    CHECK(memory_service_forget(fs, MEM_SEMANTIC, "k1") == -1); /* not implemented */
    CHECK(memory_service_stats(fs, &js) == -1);                    /* not implemented */
    memory_service_free(fs); /* frees f via destroy */
}

/* ---------- process/state snapshot (export / import) ---------- */
static void test_state_snapshot(void) {
    section("state_snapshot");
    fs_mkdirs("state-test/snap");
    config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_root = "state-test/snap";
    cfg.workspace = "state-test/snap-w";
    cfg.provider = "mock";
    cfg.http_port = 0;
    runtime_ctx ctx;
    if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
    CHECK(ctx.state != NULL && ctx.memsvc != NULL);
    /* seed kv state + a long-term fact */
    CHECK(state_store_set(ctx.state, "kv", "color", "blue") == 0);
    memory_remember(ctx.memory, "fact.lang", "c11");
    CHECK(state_export(&ctx, "state-test/snap/out.json") == 0);
    char *js = fs_read_file("state-test/snap/out.json");
    CHECK(js && strstr(js, "blue") && strstr(js, "fact.lang"));
    free(js);
    /* mutate, then restore the snapshot */
    state_store_set(ctx.state, "kv", "color", "red");
    memory_remember(ctx.memory, "fact.lang", "rust");
    CHECK(state_import(&ctx, "state-test/snap/out.json") == 0);
    CHECK(strcmp(state_store_get(ctx.state, "kv", "color"), "blue") == 0);
    CHECK(strcmp(memory_recall(ctx.memory, "fact.lang"), "c11") == 0);
    /* bad args */
    CHECK(state_export(&ctx, NULL) == -1);
    CHECK(state_import(&ctx, "state-test/snap/missing.json") == -1);
    runtime_shutdown(&ctx);
}

/* ---------- Executor family: WSL / Remote routing executors ---------- */
static char g_exec_last_args[2048];
static char g_exec_last_tool[64];
static int fake_inner_execute(void *impl, const char *tool, const char *args_json,
                              executor_result **result) {
    (void)impl;
    snprintf(g_exec_last_tool, sizeof(g_exec_last_tool), "%s", tool);
    snprintf(g_exec_last_args, sizeof(g_exec_last_args), "%s", args_json ? args_json : "");
    executor_result *r = calloc(1, sizeof(*r));
    if (!r) return -1;
    r->ok = 1;
    r->output = xstrdup("inner-ok");
    *result = r;
    return 0;
}
static int fake_inner_start(void *impl) { (void)impl; return 0; }
static int fake_inner_stop(void *impl) { (void)impl; return 0; }
static void fake_inner_destroy(void *impl) { (void)impl; }
static const executor_ops fake_inner_ops = {
    "fake-inner", fake_inner_start, fake_inner_execute, fake_inner_stop,
    fake_inner_destroy, NULL, NULL,
};

static void test_executor_family(void) {
    section("executor_family");
    static int dummy_impl = 0; /* impl must be non-NULL */
    /* WSL: shell wrapped, timeout carried, non-shell passthrough */
    executor *inner = executor_new(&fake_inner_ops, &dummy_impl);
    CHECK(inner != NULL);
    executor *w = executor_new_wsl(inner, NULL);
    CHECK(w != NULL && strcmp(executor_name(w), "wsl") == 0);
    executor_result *r = NULL;
    CHECK(executor_execute(w, "shell", "{\"cmd\":\"echo hi\",\"timeout_ms\":5000}", &r) == 0);
    CHECK(r && r->ok && strcmp(r->output, "inner-ok") == 0);
    executor_result_free(r);
    CHECK(strcmp(g_exec_last_tool, "shell") == 0);
    CHECK(strstr(g_exec_last_args, "wsl.exe -e bash -c 'echo hi'") != NULL);
    CHECK(strstr(g_exec_last_args, "5000") != NULL);
    CHECK(executor_start(w) == 0 && executor_stop(w) == 0);
    /* named distro + single-quote escaping */
    executor *inner2 = executor_new(&fake_inner_ops, &dummy_impl);
    CHECK(inner2 != NULL);
    executor *w2 = executor_new_wsl(inner2, "Ubuntu-22.04");
    CHECK(w2 != NULL);
    r = NULL;
    CHECK(executor_execute(w2, "shell", "{\"cmd\":\"echo 'a b'\"}", &r) == 0);
    executor_result_free(r);
    /* the captured args are JSON — cJSON escapes each '\' as "\\" */
    CHECK(strstr(g_exec_last_args, "wsl.exe -d Ubuntu-22.04 -e bash -c 'echo '\\\\''a b'\\\\'''") != NULL);
    executor_free(w2);
    /* non-shell tools pass through unchanged */
    r = NULL;
    CHECK(executor_execute(w, "file_read", "{\"path\":\"a.txt\"}", &r) == 0);
    executor_result_free(r);
    CHECK(strcmp(g_exec_last_tool, "file_read") == 0);
    CHECK(strstr(g_exec_last_args, "a.txt") != NULL && strstr(g_exec_last_args, "wsl") == NULL);
    executor_free(w);
    /* Remote over ssh */
    executor *inner3 = executor_new(&fake_inner_ops, &dummy_impl);
    CHECK(inner3 != NULL);
    CHECK(executor_new_remote(inner3, NULL) == NULL); /* host required */
    executor *re = executor_new_remote(inner3, "root@10.0.0.9");
    CHECK(re != NULL && strcmp(executor_name(re), "remote") == 0);
    r = NULL;
    CHECK(executor_execute(re, "shell", "{\"cmd\":\"uptime\"}", &r) == 0);
    executor_result_free(r);
    CHECK(strstr(g_exec_last_args,
                 "ssh -o ConnectTimeout=5 root@10.0.0.9 bash -c 'uptime'") != NULL);
    executor_free(re);
}

/* ---------- knowledge index ---------- */
static void test_index(void) {
    section("knowledge");
    ret_index *idx = index_new();
    CHECK(idx != NULL);
    CHECK(index_add_file(idx, "src/a.c", "int widget_init(void) { return 0; }") == 0);
    /* indexing and query tokenization are consistent (underscore is a word char) */
    char *r = index_search(idx, "widget_init", 10);
    CHECK(r != NULL);
    CHECK(strstr(r, "widget_init") != NULL);
    CHECK(strstr(r, "src/a.c") != NULL);
    free(r);
    index_free(idx);
}

/* ---------- metrics ---------- */
static void test_metrics(void) {
    section("metrics");
    metrics *m = metrics_new();
    metrics_inc(m, "a.count");
    metrics_add(m, "a.count", 2);
    metrics_set(m, "a.gauge", 7);
    char *txt = metrics_render(m);
    CHECK(txt && strstr(txt, "a.count 3") != NULL);
    CHECK(txt && strstr(txt, "a.gauge 7") != NULL);
    free(txt);
    metrics_free(m);
}

static void test_config(void) {
    section("config");
    /* mirrors the init defaults + COA_* env layering */
    config *c = config_new();
    CHECK(config_apply_json(c,
        "{\"llm.provider\":\"mock\",\"llm.base_url\":\"\",\"tx.use_transaction\":true}") == 0);
    /* no env override: falls back to the flat-dotted default (empty string) */
    CHECK(config_get_str(c, "llm.base_url", NULL) != NULL);
    CHECK(strcmp(config_get_str(c, "llm.base_url", NULL), "") == 0);
    CHECK(config_get_bool(c, "tx.use_transaction", 0) == 1);
    /* env LLM_BASE_URL writes the underscore-flattened key; it must win
     * over the empty-string flat-dotted default */
    CHECK(config_apply_json(c,
        "{\"llm.base.url\":\"http://localhost:9000\",\"tx.use.transaction\":false}") == 0);
    CHECK(strcmp(config_get_str(c, "llm.base_url", NULL),
                 "http://localhost:9000") == 0);
    CHECK(config_get_bool(c, "tx.use_transaction", 1) == 0);
    /* provider/model keys have no underscore->dot collision */
    CHECK(strcmp(config_get_str(c, "llm.provider", NULL), "mock") == 0);
    config_free(c);
}

/* ---------- cognition: blackboard ---------- */
static void test_blackboard(void) {
    section("blackboard");
    blackboard *b = blackboard_new();
    CHECK(b != NULL);
    if (!b) return;
    CHECK(blackboard_count(b) == 0);
    blackboard_put(b, "k1", "v1");
    blackboard_put(b, "k2", "v2");
    blackboard_put(b, "k1", "v1b");   /* overwrite */
    CHECK(blackboard_count(b) == 2);
    char *g = blackboard_get(b, "k1");
    CHECK_STR(g, "v1b");
    free(g);
    CHECK(blackboard_get(b, "missing") == NULL);
    CHECK(blackboard_remove(b, "k1") == 1);
    CHECK(blackboard_remove(b, "k1") == 0);
    CHECK(blackboard_count(b) == 1);
    char *snap = blackboard_snapshot_json(b);
    CHECK(snap && strstr(snap, "k2") != NULL && strstr(snap, "v2") != NULL);
    free(snap);
    blackboard_free(b);
}

/* ---------- runtime: multi-agent coordinator ---------- */
static void test_agent_pool(void) {
    section("agent_pool");
    agent_pool *p = agent_pool_new();
    CHECK(p != NULL);
    if (!p) return;
    CHECK(agent_pool_add(p, "planner", "plan") >= 0);
    CHECK(agent_pool_add(p, "executor", "act") >= 0);
    CHECK(agent_pool_add(p, "planner", "dup") == -1);  /* duplicate */
    CHECK(agent_pool_count(p) == 2);
    CHECK(agent_post(p, "planner", "plan", "step1") == 0);
    CHECK(agent_post(p, "ghost", "k", "v") == -1);     /* unknown agent */
    blackboard *bb = agent_pool_blackboard(p);
    CHECK(bb != NULL);
    char *g = blackboard_get(bb, "plan");
    CHECK_STR(g, "step1");
    free(g);
    char *snap = agent_pool_snapshot_json(p);
    CHECK(snap && strstr(snap, "planner") != NULL && strstr(snap, "step1") != NULL);
    free(snap);
    /* removal: unknown name fails, known name removes, remaining agent intact */
    CHECK(agent_pool_remove(p, "ghost") == -1);
    CHECK(agent_pool_remove(p, "planner") == 0);
    CHECK(agent_pool_count(p) == 1);
    CHECK(agent_pool_find(p, "planner") == -1);
    CHECK(agent_pool_find(p, "executor") >= 0);
    CHECK(agent_post(p, "executor", "act", "done") == 0);
    agent_pool_free(p);
}

/* ---------- api: auth ---------- */
static void test_auth(void) {
    section("auth");
    auth *a = auth_new();
    CHECK(a != NULL);
    if (!a) return;
    auth_add_key(a, "secret-123");
    CHECK(auth_count(a) == 1);
    CHECK(auth_check(a, "secret-123") == 1);
    CHECK(auth_check(a, "secret-124") == 0);
    CHECK(auth_check_header(a, "Bearer secret-123") == 1);
    CHECK(auth_check_header(a, "bearer secret-123") == 1);
    CHECK(auth_check_header(a, "Bearer wrong") == 0);
    CHECK(auth_check_header(a, NULL) == 0);
    char tok[33];
    auth_generate_token(tok, 16);
    CHECK(strlen(tok) == 32);
    auth_add_key(a, tok);
    CHECK(auth_check(a, tok) == 1);
    auth_free(a);
}

/* ---------- api: websocket (SHA1 + base64 + frames) ---------- */
static void test_websocket(void) {
    section("websocket");
    static const char *hexc = "0123456789abcdef";
    unsigned char sha[20];
    char hex[41];

    sha1((const unsigned char *)"abc", 3, sha);
    for (int i = 0; i < 20; i++) { hex[i * 2] = hexc[sha[i] >> 4]; hex[i * 2 + 1] = hexc[sha[i] & 0xF]; }
    hex[40] = '\0';
    CHECK_STR(hex, "a9993e364706816aba3e25717850c26c9cd0d89d");

    sha1((const unsigned char *)"", 0, sha);
    for (int i = 0; i < 20; i++) { hex[i * 2] = hexc[sha[i] >> 4]; hex[i * 2 + 1] = hexc[sha[i] & 0xF]; }
    CHECK_STR(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    char *b64 = base64_encode((const unsigned char *)"the sample nonce", 16);
    CHECK_STR(b64, "dGhlIHNhbXBsZSBub25jZQ==");
    free(b64);

    unsigned char dec[64];
    size_t dec_len = 0;
    CHECK(base64_decode("dGhlIHNhbXBsZSBub25jZQ==", dec, sizeof(dec), &dec_len) == 0);
    CHECK(dec_len == 16);
    CHECK(memcmp(dec, "the sample nonce", 16) == 0);

    char accept[29];
    ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept);
    CHECK_STR(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    /* short frame (7-bit length, masked) */
    size_t flen = 0;
    char *f = ws_build_frame(WS_OP_TEXT, (const unsigned char *)"hello", 5, 1, &flen);
    CHECK(f != NULL);
    if (f) {
        unsigned char pay[256];
        size_t plen = 0;
        int op = 0, fin = 0;
        CHECK(ws_parse_frame((const unsigned char *)f, flen, pay, &plen, &op, &fin) == 0);
        CHECK(fin == 1 && op == WS_OP_TEXT && plen == 5);
        CHECK(memcmp(pay, "hello", 5) == 0);
        free(f);
    }

    /* medium payload (16-bit length) */
    unsigned char med[200];
    for (int i = 0; i < 200; i++) med[i] = (unsigned char)(i & 0xFF);
    f = ws_build_frame(WS_OP_BINARY, med, 200, 1, &flen);
    CHECK(f != NULL);
    if (f) {
        unsigned char *pay = (unsigned char *)malloc(200);
        size_t plen = 0;
        int op = 0, fin = 0;
        CHECK(ws_parse_frame((const unsigned char *)f, flen, pay, &plen, &op, &fin) == 0);
        CHECK(op == WS_OP_BINARY && plen == 200);
        CHECK(memcmp(pay, med, 200) == 0);
        free(pay);
        free(f);
    }

    /* large payload (64-bit length, unmasked) */
    size_t big_n = 70000;
    unsigned char *big = (unsigned char *)malloc(big_n);
    for (size_t i = 0; i < big_n; i++) big[i] = (unsigned char)((i * 7) & 0xFF);
    f = ws_build_frame(WS_OP_BINARY, big, big_n, 0, &flen);
    CHECK(f != NULL);
    if (f) {
        unsigned char *pay = (unsigned char *)malloc(big_n);
        size_t plen = 0;
        int op = 0, fin = 0;
        CHECK(ws_parse_frame((const unsigned char *)f, flen, pay, &plen, &op, &fin) == 0);
        CHECK(op == WS_OP_BINARY && plen == big_n);
        CHECK(memcmp(pay, big, big_n) == 0);
        free(pay);
        free(f);
    }
    free(big);
}

/* ---------- plugin runtime: dynamic loader smoke ---------- */
static void test_plugin_loader(void) {
    section("plugin_loader");
    plugin *p = plugin_load("this_plugin_does_not_exist_xyz.so");
    CHECK(p == NULL);
    const char *err = plugin_error();
    CHECK(err != NULL && *err != '\0');
}

/* ---------- memory: kv store ---------- */
static void test_kv(void) {
    section("kv");
    kvstore *k = kvstore_new();
    CHECK(k != NULL);
    if (!k) return;
    kvstore_set(k, "a", "1");
    kvstore_set(k, "b", "2");
    CHECK(kvstore_count(k) == 2);
    CHECK_STR(kvstore_get(k, "a"), "1");
    kvstore_set(k, "a", "11");
    CHECK_STR(kvstore_get(k, "a"), "11");
    CHECK(kvstore_remove(k, "a") == 1);
    CHECK(kvstore_get(k, "a") == NULL);
    char *j = kvstore_snapshot_json(k);
    CHECK(j && strstr(j, "b") != NULL);
    free(j);
    kvstore_free(k);
}

/* ---------- memory: episodic store ---------- */
static void test_episodic(void) {
    section("episodic");
    episodic *e = episodic_new();
    CHECK(e != NULL);
    if (!e) return;
    episodic_add(e, "t1", "r1");
    episodic_add(e, "t2", "r2");
    CHECK(episodic_count(e) == 2);
    CHECK_STR(episodic_task(e, 0), "t1");
    CHECK_STR(episodic_result(e, 1), "r2");
    char *j = episodic_json(e);
    CHECK(j && strstr(j, "t1") != NULL && strstr(j, "r2") != NULL);
    free(j);
    episodic_free(e);
}

/* ---------- memory: vector store ---------- */
static void test_vector(void) {
    section("vector");
    vectorstore *v = vectorstore_new();
    CHECK(v != NULL);
    if (!v) return;
    vectorstore_add(v, "1", "hello world foo", "m1");
    vectorstore_add(v, "2", "goodbye world bar", "m2");
    CHECK(vectorstore_count(v) == 2);
    char *n = vectorstore_nearest(v, "hello", 2);
    CHECK(n != NULL);
    cJSON *arr = cJSON_Parse(n ? n : "[]");
    CHECK(arr && cJSON_GetArraySize(arr) >= 1);
    if (arr && cJSON_GetArraySize(arr) >= 1) {
        cJSON *first = cJSON_GetArrayItem(arr, 0);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(first, "id");
        CHECK(id && cJSON_IsString(id) && strcmp(id->valuestring, "1") == 0);
    }
    if (arr) cJSON_Delete(arr);
    free(n);
    vectorstore_free(v);
}

/* ---------- memory: knowledge graph ---------- */
static void test_graph(void) {
    section("graph");
    graph *g = graph_new();
    CHECK(g != NULL);
    if (!g) return;
    CHECK(graph_add_node(g, "n1", "node one") == 0);
    CHECK(graph_add_node(g, "n2", "node two") == 0);
    CHECK(graph_add_node(g, "n1", "dup") == -1);
    CHECK(graph_node_count(g) == 2);
    CHECK(graph_add_edge(g, "n1", "n2", "rel") == 0);
    CHECK(graph_edge_count(g) == 1);
    char *nb = graph_neighbors(g, "n1");
    CHECK(nb && strstr(nb, "n2") != NULL && strstr(nb, "rel") != NULL);
    free(nb);
    char *snap = graph_snapshot_json(g);
    CHECK(snap && strstr(snap, "n1") != NULL && strstr(snap, "node one") != NULL);
    free(snap);
    graph_free(g);
}

/* ---------- retrieval: context builder ---------- */
static void test_context_builder(void) {
    section("context_builder");
    memory *m = memory_new("state-test/memory-cb");
    CHECK(m != NULL);
    if (m) {
        memory_working_push(m, "project uses the c language");
        memory_record_experience(m, "write file", "done");
        char *ctx = context_build(m, "project", 8);
        CHECK(ctx && strstr(ctx, "[") != NULL);
        free(ctx);
        memory_free(m);
    }
    char *txt = context_render_text(
        "[{\"kind\":\"working\",\"text\":\"hello\",\"result\":\"\",\"score\":1}]");
    CHECK(txt && strstr(txt, "hello") != NULL);
    free(txt);
}

/* ---------- memory: episode persistence + context shaping ---------- */
static void test_memory_persist(void) {
    section("memory persistence + shaping");
    /* episode round-trip through episodes.json */
    {
        episodic *e = episodic_new();
        CHECK(e != NULL);
        episodic_add(e, "task1", "short result");
        long long old_ts = time_now_ms() - 6LL * 86400000LL; /* 6 days ago */
        episodic_add_ts(e, "old task", "old result", old_ts);
        /* exact-duplicate task updates instead of duplicating */
        episodic_add(e, "task1", "updated result");
        CHECK(episodic_count(e) == 2);
        CHECK_STR(episodic_result(e, 0), "updated result");
        CHECK(episodic_ts(e, 1) == old_ts);
        char *j = episodic_json(e);
        CHECK(j && strstr(j, "ts") != NULL);
        free(j);
        episodic_free(e);
    }
    /* facts + episodes survive flush/reload via the memory facade */
    {
        memory *m = memory_new("state-test/memory-mp");
        CHECK(m != NULL);
        if (!m) return;
        memory_remember(m, "user.name", "qin");
        memory_record_experience(m, "check repo", "all green");
        memory_flush(m);
        memory_free(m);

        memory *m2 = memory_new("state-test/memory-mp");
        CHECK(m2 != NULL);
        if (m2) {
            const char *v = memory_recall(m2, "user.name");
            CHECK(v && strcmp(v, "qin") == 0);
            char *ej = memory_episodes_json(m2);
            CHECK(ej && strstr(ej, "check repo") != NULL);
            free(ej);
            /* reloaded episodes must be mirrored into the vector store so RAG
             * recall (retrieve/_ex, used by the context builder) still finds
             * pre-restart experience */
            char *r1 = memory_retrieve(m2, "check repo", 5);
            CHECK(r1 && strstr(r1, "check repo") != NULL);
            free(r1);
            char *r2 = memory_retrieve_ex(m2, "check repo", 5, 0.7f);
            CHECK(r2 && strstr(r2, "check repo") != NULL);
            free(r2);
            /* facts are injected into built context */
            char *ctx = context_build(m2, "anything", 8);
            CHECK(ctx && strstr(ctx, "user.name") != NULL);
            free(ctx);
            memory_free(m2);
        }
    }
    /* working ring eviction must unmirror its vector entries (bounded store) */
    {
        memory *m = memory_new("state-test/memory-wb");
        CHECK(m != NULL);
        if (m) {
            for (int i = 0; i < 300; i++) {
                char t[64];
                snprintf(t, sizeof(t), "work item %d status report", i);
                memory_working_push(m, t);
            }
            CHECK(memory_working_count(m) == 64);
            char *j = memory_retrieve(m, "work item status", 1000);
            int w = 0;
            if (j) {
                cJSON *arr = cJSON_Parse(j);
                cJSON *it;
                cJSON_ArrayForEach(it, arr) {
                    cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
                    if (id && cJSON_IsString(id) && id->valuestring &&
                        strncmp(id->valuestring, "w:", 2) == 0)
                        w++;
                }
                cJSON_Delete(arr);
            }
            CHECK(w <= 64);
            free(j);
            memory_free(m);
        }
    }
}

/* ---------- context: per-item and total caps + freshness ---------- */
static void test_context_caps(void) {
    section("context caps + freshness");
    /* oversized item text is truncated with an ellipsis marker */
    {
        char *big = (char *)malloc(6000);
        memset(big, 'a', 5000);
        big[5000] = '\0';
        char *txt = context_render_text(
            "[{\"kind\":\"working\",\"text\":\"BIG\",\"result\":\"x\",\"score\":1}]");
        free(big);
        (void)txt;
        free(txt);
        char *txt2 = context_render_text(
            "[{\"kind\":\"working\",\"text\":\"small\",\"result\":\"ok\",\"score\":1}]");
        CHECK(txt2 && strstr(txt2, "small") != NULL && strstr(txt2, "ok") != NULL);
        free(txt2);
    }
    /* stale episodes are annotated with an age hint */
    {
        long long old_ts = (double)time_now_ms() - 5.0 * 86400000.0;
        char json[256];
        snprintf(json, sizeof(json),
                 "[{\"kind\":\"experience\",\"text\":\"old thing\",\"result\":\"r\","
                 "\"score\":1,\"ts\":%lld}]", old_ts);
        char *txt = context_render_text(json);
        CHECK(txt && strstr(txt, "可能过时") != NULL);
        free(txt);
        /* fresh items carry no annotation */
        char *txt2 = context_render_text(
            "[{\"kind\":\"experience\",\"text\":\"new thing\",\"result\":\"r\",\"score\":1,\"ts\":0}]");
        CHECK(txt2 && strstr(txt2, "可能过时") == NULL);
        free(txt2);
    }
}

/* ---------- reasoning: session notes + threshold LLM compaction ---------- */
static void test_session_memory(void) {
    section("reasoning session notes + compaction");
    CHECK_STR(reasoning_session_json(NULL), "{}");
    {
        llm *llm = llm_create("mock", NULL, NULL, "mock");
        tool_registry *reg = tool_registry_new();
        if (reg) tool_register_builtins(reg); /* mock plans file_* actions */
        CHECK(llm != NULL && reg != NULL);
        if (llm && reg) {
            reasoning_config cfg = {0};
            cfg.llm = llm;
            cfg.tools = reg;
            reasoning *r = reasoning_new(&cfg);
            CHECK(r != NULL);
            if (r) {
                /* plain-text prompts (no tool keywords) -> DONE each run */
                for (int i = 0; i < 17; i++) {
                    char p[128];
                    snprintf(p, sizeof(p), "问题%d：聊聊话题%d", i, i);
                    char *ans = NULL;
                    CHECK(reasoning_run(r, p, &ans) == 0);
                    free(ans);
                }
                /* ring cap 16: the 16th recorded turn hits the threshold and
                 * compacts the oldest half (8 dropped, 8 kept); the 17th push
                 * brings it to 9. The keyword-driven mock answers the
                 * compaction prompt with arbitrary text, so only assert the
                 * summary exists, not its content. */
                char *sj = reasoning_session_json(r);
                CHECK(sj != NULL);
                if (sj) {
                    cJSON *o = cJSON_Parse(sj);
                    CHECK(o != NULL);
                    if (o) {
                        cJSON *ht = cJSON_GetObjectItemCaseSensitive(o, "history_turns");
                        CHECK(ht && cJSON_IsNumber(ht) && ht->valuedouble == 9);
                        cJSON *sum = cJSON_GetObjectItemCaseSensitive(o, "summary");
                        CHECK(sum && cJSON_IsString(sum) && sum->valuestring[0] != '\0');
                        cJSON *task = cJSON_GetObjectItemCaseSensitive(o, "task");
                        CHECK(task && cJSON_IsString(task) &&
                              strstr(task->valuestring, "问题16") != NULL);
                        cJSON_Delete(o);
                    }
                    free(sj);
                }
                /* a follow-up run still works with the compacted state */
                char *ans = NULL;
                CHECK(reasoning_run(r, "简单收尾一下", &ans) == 0);
                free(ans);
                reasoning_free(r);
            }
        }
        if (llm) llm_destroy(llm);
        if (reg) tool_registry_free(reg);
    }
}

/* ---------- cognition: planner + evaluator ---------- */
static void test_planner(void) {
    section("planner");
    llm *llm = llm_create("mock", NULL, NULL, "mock");
    CHECK(llm != NULL);
    if (!llm) return;
    planned_action *actions = NULL;
    int n = -1;
    char *raw = NULL;
    CHECK(planner_plan(llm, "创建 test/note.txt 写入内容为 hello", &actions, &n, &raw, NULL) == 0);
    CHECK(n >= 1);
    CHECK(actions != NULL);
    if (n >= 1 && actions) CHECK_STR(actions[0].tool, "file_write");
    CHECK(raw != NULL);
    planner_actions_free(actions, n);
    free(raw);
    llm_destroy(llm);
}

static void test_evaluator(void) {
    section("evaluator");
    evaluator *ev = evaluator_new();
    CHECK(evaluator_verify(ev, 1, 2, 2) == 1);
    CHECK(evaluator_verify(ev, 0, 2, 2) == 1);
    CHECK(evaluator_verify(ev, 0, 2, 0) == 0);
    CHECK(evaluator_verify(ev, 1, 0, 0) == 1);
    CHECK(evaluator_score(ev, 2, 2, 1, "ok") > 0.5);
    CHECK(evaluator_score(ev, 2, 1, 0, "FAILED") == 0.0);
    evaluator_free(ev);
}

/* ---------- plugin runtime: sandbox + capability ---------- */
static void test_sandbox(void) {
    section("sandbox");
    sandbox *sb = sandbox_new(5000);
    CHECK(sandbox_forbidden("rm -rf /") == 1);
    CHECK(sandbox_forbidden("echo hi") == 0);
    sandbox_result *r = sandbox_run(sb, "echo hello");
    CHECK(r != NULL);
    if (r) {
        CHECK(r->ok == 1);
        CHECK(r->output && strstr(r->output, "hello") != NULL);
        sandbox_result_free(r);
    }
    CHECK(sandbox_run(sb, "rm -rf /tmp/x") == NULL);
    sandbox_free(sb);
}

static void test_filetracker(void) {
    section("filetracker");
    /* registry: record / ops merge / dedup */
    filetracker *ft = filetracker_new();
    CHECK(filetracker_record(ft, "a.txt", FT_READ) == FT_READ);
    CHECK(filetracker_record(ft, "a.txt", FT_WRITE) == (FT_READ | FT_WRITE));
    CHECK(filetracker_record(ft, "b.txt", FT_EXEC) == FT_EXEC);
    CHECK(filetracker_record(ft, NULL, FT_READ) == 0);
    CHECK(filetracker_count(ft) == 2);
    CHECK(strcmp(filetracker_ops_str(FT_READ | FT_DELETE), "read,delete") == 0);
    CHECK(strcmp(filetracker_ops_str(FT_READ | FT_WRITE | FT_DELETE | FT_EXEC),
                 "read,write,delete,exec") == 0);
    CHECK(strcmp(filetracker_ops_str(0), "none") == 0);
    char *j = filetracker_json(ft);
    CHECK(j && strstr(j, "a.txt") != NULL && strstr(j, "read,write") != NULL);
    free(j);
    filetracker_clear(ft);
    CHECK(filetracker_count(ft) == 0);

    /* snapshot / diff with real files */
    fs_mkdirs("state-test/ft-w");
    fs_remove("state-test/ft-w/new.txt");
    fs_remove("state-test/ft-w/out.txt");
    fs_write_file("state-test/ft-w/base.txt", "base", 4);
    fs_write_file("state-test/ft-w/del.txt", "del", 3);
    ft_snapshot *snap = filetracker_dir_snapshot("state-test/ft-w");
    CHECK(snap != NULL);
    CHECK(filetracker_dir_diff(ft, snap, "state-test/ft-w") == 0); /* unchanged */

    fs_write_file("state-test/ft-w/new.txt", "new", 3);            /* created */
    fs_write_file("state-test/ft-w/base.txt", "base changed!", 13); /* modified */
    fs_remove("state-test/ft-w/del.txt");                           /* deleted */
    CHECK(filetracker_dir_diff(ft, snap, "state-test/ft-w") == 3);
    char *j2 = filetracker_json(ft);
    CHECK(j2 && strstr(j2, "new.txt") != NULL && strstr(j2, "write") != NULL);
    CHECK(j2 && strstr(j2, "del.txt") != NULL && strstr(j2, "delete") != NULL);
    free(j2);
    filetracker_snapshot_free(snap);

    /* command read detection */
    filetracker_clear(ft);
    CHECK(filetracker_cmd_reads(ft, "cat state-test/ft-w/base.txt", ".") == 1);
    char *j3 = filetracker_json(ft);
    CHECK(j3 && strstr(j3, "base.txt") != NULL && strstr(j3, "read") != NULL);
    free(j3);

    /* sandbox integration: workspace diff lands in result->files_json */
    sandbox *sb = sandbox_new(8000);
    CHECK(sandbox_filetracker(sb) == NULL); /* lazy until workspace set */
    sandbox_set_workspace(sb, "state-test/ft-w");
    CHECK(sandbox_filetracker(sb) != NULL);
    sandbox_result *r = sandbox_run(sb, "echo hi > state-test/ft-w/out.txt");
    CHECK(r != NULL && r->ok == 1);
    if (r) {
        CHECK(r->files_json && strstr(r->files_json, "out.txt") != NULL);
        CHECK(r->files_json && strstr(r->files_json, "write") != NULL);
        sandbox_result_free(r);
    }
    sandbox_free(sb);
    filetracker_free(ft);
}

static void test_capability(void) {
    section("capability");
    capability *c = capability_new();
    CHECK(c != NULL);
    if (!c) return;
    CHECK(capability_grant(c, "fs.read") == 0);
    CHECK(capability_grant(c, "fs.write") == 0);
    CHECK(capability_grant(c, "net") == 0);
    CHECK(capability_grant(c, "fs.read") == -1);
    CHECK(capability_count(c) == 3);
    CHECK(capability_has(c, "fs.read") == 1);
    CHECK(capability_match(c, "fs.*") == 1);
    CHECK(capability_match(c, "net.*") == 1);
    CHECK(capability_match(c, "proc.*") == 0);
    CHECK(capability_revoke(c, "fs.read") == 1);
    CHECK(capability_has(c, "fs.read") == 0);
    char *j = capability_json(c);
    CHECK(j && strstr(j, "fs.write") != NULL);
    free(j);
    capability_free(c);
}

/* ---------- plugin intelligence ---------- */
static void test_plugin_intelligence(void) {
    section("plugin_intelligence");
    char *a = analyzer_analyze(
        "{\"name\":\"p\",\"description\":\"read and write files over an http api\"}");
    CHECK(a && strstr(a, "complexity") != NULL);
    CHECK(a && strstr(a, "fs.read") != NULL);
    CHECK(a && strstr(a, "net") != NULL);
    free(a);

    char *d = architect_design("build a file sync plugin");
    CHECK(d && strstr(d, "components") != NULL && strstr(d, "interfaces") != NULL);
    free(d);

    char *cg = codegen_plugin("My Plugin", "does things");
    CHECK(cg && strstr(cg, "My_Plugin") != NULL);
    CHECK(cg && strstr(cg, "run") != NULL);
    free(cg);

    char *tp = testing_plan("{\"name\":\"p\"}");
    CHECK(tp && strstr(tp, "cases") != NULL);
    free(tp);

    char *tr = testing_run("echo ok", 5000);
    CHECK(tr && strstr(tr, "ok") != NULL);
    free(tr);

    char *sec = security_audit("system(\"rm -rf /\")");
    CHECK(sec && strstr(sec, "system(") != NULL);
    CHECK(sec && strstr(sec, "rm -rf") != NULL);
    free(sec);
}

/* ---------- observability: trace ---------- */
static void test_trace(void) {
    section("trace");
    trace *t = trace_new(8);
    CHECK(t != NULL);
    if (!t) return;
    int64_t id = trace_begin(t, "span-a");
    CHECK(id > 0);
    int64_t id2 = trace_begin(t, "span-b");
    CHECK(id2 > id);
    trace_end(t, id, 1);
    CHECK(trace_count(t) == 2);
    char *j = trace_json(t);
    CHECK(j && strstr(j, "span-a") != NULL);
    free(j);
    trace_clear(t);
    CHECK(trace_count(t) == 0);
    trace_free(t);
}

/* ---------- llm: router + usage ---------- */
static void test_router(void) {
    section("router");
    router *r = router_new();
    CHECK(r != NULL);
    if (!r) return;
    CHECK(router_add(r, "a", "openai", "https://a", "k", "gpt-4", 1.0) == 0);
    CHECK(router_add(r, "b", "anthropic", "https://b", NULL, "claude", 2.0) == 0);
    CHECK(router_count(r) == 2);
    CHECK(router_pick(r) != NULL);
    CHECK(router_pick(r) != NULL);
    char *j = router_json(r);
    CHECK(j && strstr(j, "openai") != NULL);
    free(j);
    router_free(r);
}

static void test_usage(void) {
    section("usage");
    usage *u = usage_new();
    CHECK(u != NULL);
    if (!u) return;
    usage_add(u, "gpt-4", 100, 50);
    usage_add(u, "gpt-4", 20, 10);
    usage_add(u, "claude", 5, 5);
    CHECK(usage_prompt_total(u) == 125);
    CHECK(usage_completion_total(u) == 65);
    char *j = usage_json(u);
    CHECK(j && strstr(j, "gpt-4") != NULL);
    free(j);
    usage_free(u);
}

/* ---------- plugin registry ---------- */
static void test_registry(void) {
    section("plugin_registry");
    plugin_registry *r = plugin_registry_new();
    CHECK(r != NULL);
    if (!r) return;
    plugin_meta m1;
    memset(&m1, 0, sizeof(m1));
    m1.name = "p1";
    m1.version = "1.0.0";
    char *c1[] = { "fs.read" };
    m1.caps = c1;
    m1.n_caps = 1;
    char *d1[] = { "base" };
    m1.deps = d1;
    m1.n_deps = 1;
    CHECK(plugin_registry_register(r, &m1) == 0);
    CHECK(plugin_registry_register(r, &m1) == -1); /* duplicate same-version */
    CHECK(plugin_registry_deps_met(r, "p1") == 0);

    plugin_meta base;
    memset(&base, 0, sizeof(base));
    base.name = "base";
    base.version = "1.0.0";
    CHECK(plugin_registry_register(r, &base) == 0);
    CHECK(plugin_registry_deps_met(r, "p1") == 1);
    CHECK(plugin_registry_count(r) == 2);

    const plugin_meta *f = plugin_registry_find(r, "p1");
    CHECK(f != NULL && strcmp(f->version, "1.0.0") == 0);
    CHECK(plugin_registry_set_enabled(r, "p1", 0) == 0);
    char *j = plugin_registry_json(r);
    CHECK(j && strstr(j, "p1") != NULL);
    free(j);
    CHECK(plugin_registry_unregister(r, "base") == 0);
    CHECK(plugin_registry_count(r) == 1);
    plugin_registry_free(r);
    /* persist + load round-trip must keep dependencies: without them,
     * dependency enforcement silently disappears after a restart */
    {
        plugin_registry *r2 = plugin_registry_new();
        plugin_meta base2;
        memset(&base2, 0, sizeof(base2));
        base2.name = "base";
        base2.version = "1.0.0";
        plugin_registry_register(r2, &base2);
        plugin_meta m2;
        memset(&m2, 0, sizeof(m2));
        m2.name = "p2";
        m2.version = "2.0.0";
        char *d2[] = { "base" };
        m2.deps = d2;
        m2.n_deps = 1;
        plugin_registry_register(r2, &m2);
        fs_mkdirs("state-test-plugins");
        CHECK(plugin_registry_persist(r2, "state-test-plugins") == 0);
        plugin_registry *r3 = plugin_registry_new();
        CHECK(plugin_registry_load(r3, "state-test-plugins") == 0);
        CHECK(plugin_registry_deps_met(r3, "p2") == 1);
        plugin_registry_unregister(r3, "base");
        CHECK(plugin_registry_deps_met(r3, "p2") == 0);
        plugin_registry_free(r2);
        plugin_registry_free(r3);
        fs_remove("state-test-plugins/plugins.json");
        fs_remove("state-test-plugins");
    }
}

/* ---------- skills ---------- */
static void test_skills(void) {
    section("skills");
    skill_registry *r = skill_registry_new();
    CHECK(r != NULL);
    if (!r) return;
    skill s = { "echo_hi", "print hi", "shell", "echo hi", NULL };
    CHECK(skill_register(r, &s) == 0);
    CHECK(skill_register(r, &s) == -1); /* duplicate */
    CHECK(skill_count(r) == 1);
    const skill *f = skill_find(r, "echo_hi");
    CHECK(f != NULL && strcmp(f->kind, "shell") == 0);
    skill_result *res = skill_execute(r, "echo_hi", NULL, NULL, 5000);
    CHECK(res != NULL);
    if (res) {
        CHECK(res->ok == 1);
        CHECK(res->output && strstr(res->output, "hi") != NULL);
        skill_result_free(res);
    }
    char *j = skill_list_json(r);
    CHECK(j && strstr(j, "echo_hi") != NULL);
    free(j);
    skill_registry_free(r);
}

/* ---------- mcp manager ---------- */
static void test_mcp(void) {
    section("mcp");
    mcp_manager *m = mcp_manager_new();
    CHECK(m != NULL);
    if (!m) return;
    CHECK(mcp_manager_add(m, "srv1", "http://127.0.0.1:9000/mcp", "tok") == 0);
    CHECK(mcp_manager_add(m, "srv1", "http://127.0.0.1:9001/mcp", NULL) == 0); /* update */
    CHECK(mcp_manager_count(m) == 1);
    const mcp_conn *c = mcp_manager_find(m, "srv1");
    CHECK(c != NULL && strstr(c->url, "9001") != NULL);
    char *j = mcp_manager_json(m);
    CHECK(j && strstr(j, "srv1") != NULL);
    free(j);
    /* unreachable server: call fails with a diagnostic, no crash */
    {
        char *out = NULL, *err = NULL;
        CHECK(mcp_manager_call(m, "srv1", "ping", "{}", &out, &err) != 0);
        free(out);
        free(err);
    }
    CHECK(mcp_manager_remove(m, "srv1", NULL) == 0);
    CHECK(mcp_manager_count(m) == 0);
    mcp_manager_free(m);
}

/* ---------- tools: JSON-schema validation ---------- */
static tool_result *big_out_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    (void)self; (void)ctx; (void)args_json;
    char *big = (char *)malloc(20001);
    if (!big) return tool_result_new(0, "oom");
    for (int i = 0; i < 20000; i++) big[i] = 'x';
    big[20000] = '\0';
    tool_result *r = tool_result_new(1, big);
    free(big);
    return r;
}

static void test_tool_schema(void) {
    section("tool schema validation");
    tool t;
    memset(&t, 0, sizeof(t));
    t.name = "schematool";
    t.json_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"s\":{\"type\":\"string\"},\"n\":{\"type\":\"integer\"},"
        "\"b\":{\"type\":\"boolean\"},\"o\":{\"type\":\"object\"}},"
        "\"required\":[\"s\"]}";

    char *err = NULL;
    CHECK(tool_validate_args(&t, "{\"s\":\"hi\",\"n\":5}", &err) == 0);
    free(err);
    CHECK(tool_validate_args(&t, "{\"s\":\"hi\",\"b\":true,\"o\":{\"k\":1}}", &err) == 0);
    free(err); err = NULL;
    /* missing required */
    CHECK(tool_validate_args(&t, "{\"n\":1}", &err) != 0);
    CHECK(err && strstr(err, "s") != NULL);
    free(err); err = NULL;
    /* wrong type */
    CHECK(tool_validate_args(&t, "{\"s\":42}", &err) != 0);
    CHECK(err && strstr(err, "s") != NULL);
    free(err); err = NULL;
    CHECK(tool_validate_args(&t, "{\"s\":\"x\",\"n\":\"no\"}", &err) != 0);
    free(err); err = NULL;
    /* invalid args JSON */
    CHECK(tool_validate_args(&t, "not-json", &err) != 0);
    free(err); err = NULL;
    /* NULL schema always validates */
    tool t2; memset(&t2, 0, sizeof(t2));
    t2.name = "noschema";
    CHECK(tool_validate_args(&t2, "{\"anything\":1}", &err) == 0);
    free(err);
}

static void test_tool_truncate(void) {
    section("tool result truncation");
    tool_registry *reg = tool_registry_new();
    CHECK(reg != NULL);
    if (!reg) return;
    tool t;
    memset(&t, 0, sizeof(t));
    t.name = "bigout";
    t.execute = big_out_exec;
    CHECK(tool_register(reg, &t) == 0);

    tool_result *r = tool_execute(reg, "bigout", "{}", NULL);
    CHECK(r != NULL);
    if (r) {
        CHECK(r->ok == 1);
        CHECK(r->output && strlen(r->output) < 20000);
        CHECK(r->output && strstr(r->output, "[truncated") != NULL);
        tool_result_free(r);
    }
    tool_registry_free(reg);
}

/* ---------- skills: {{placeholder}} args binding ---------- */
static void test_skill_args(void) {
    section("skill args binding");
    skill_registry *r = skill_registry_new();
    CHECK(r != NULL);
    if (!r) return;
    skill s = { "greet_test", "greet someone", "shell", "echo hello {{who}}", NULL };
    CHECK(skill_register(r, &s) == 0);
    skill_result *res = skill_execute(r, "greet_test", "{\"who\":\"claude\"}", NULL, 8000);
    CHECK(res != NULL);
    if (res) {
        CHECK(res->ok == 1);
        CHECK(res->output && strstr(res->output, "hello claude") != NULL);
        skill_result_free(res);
    }
    /* missing key: placeholder stays and a hint is appended */
    res = skill_execute(r, "greet_test", "{}", NULL, 8000);
    CHECK(res != NULL);
    if (res) {
        CHECK(res->output && strstr(res->output, "{{who}}") != NULL);
        skill_result_free(res);
    }
    skill_registry_free(r);
}

/* ---------- capability gate on plugin skills ---------- */
static void test_caps_gate(void) {
    section("skill capability gate");
    skill_registry *r = skill_registry_new();
    CHECK(r != NULL);
    if (!r) return;
    skill deny = { "caps_deny", "write denied", "shell",
                      "echo gate-data > caps_gate_out.txt", "fs.read" };
    skill allow = { "caps_allow", "write allowed", "shell",
                       "echo gate-data > caps_gate_out.txt", "fs.write" };
    CHECK(skill_register(r, &deny) == 0);
    CHECK(skill_register(r, &allow) == 0);

    /* fs.write operation not covered by "fs.read" -> denied with reason */
    skill_result *res = skill_execute(r, "caps_deny", NULL, NULL, 8000);
    CHECK(res != NULL);
    if (res) {
        CHECK(res->ok == 0);
        CHECK(res->output && strstr(res->output, "capability denied") != NULL);
        CHECK(res->output && strstr(res->output, "fs.write") != NULL);
        skill_result_free(res);
    }
    /* covered -> runs */
    res = skill_execute(r, "caps_allow", NULL, NULL, 8000);
    CHECK(res != NULL);
    if (res) {
        CHECK(res->ok == 1);
        skill_result_free(res);
    }
    /* wildcard "fs.*" also covers fs.write */
    skill wild = { "caps_wild", "wildcard", "shell",
                      "echo gate-data > caps_gate_out.txt", "fs.*" };
    CHECK(skill_register(r, &wild) == 0);
    res = skill_execute(r, "caps_wild", NULL, NULL, 8000);
    CHECK(res != NULL);
    if (res) {
        CHECK(res->ok == 1);
        skill_result_free(res);
    }
    skill_registry_free(r);
}

/* ---------- generated-plugin tool binding (self-evolution loop) ---------- */
static void test_generated_tool(void) {
    section("generated plugin -> tool binding");
    skill_registry *skills = skill_registry_new();
    tool_registry *reg = tool_registry_new();
    CHECK(skills != NULL && reg != NULL);
    if (!skills || !reg) { skill_registry_free(skills); tool_registry_free(reg); return; }

    /* run the generation pipeline offline (mock design) and bind the produced
     * plugin as a callable tool, like reasoning.c does on missing capability */
    plugin_gen_deps gd;
    memset(&gd, 0, sizeof(gd));
    gd.skills = skills;
    char *gjson = plugin_generate_deps(&gd, "读取配置 config 文件");
    CHECK(gjson != NULL);
    char name[128] = "";
    if (gjson) {
        cJSON *g = cJSON_Parse(gjson);
        cJSON *okj = g ? cJSON_GetObjectItemCaseSensitive(g, "ok") : NULL;
        cJSON *pj = g ? cJSON_GetObjectItemCaseSensitive(g, "plugin") : NULL;
        cJSON *nj = pj ? cJSON_GetObjectItemCaseSensitive(pj, "name") : NULL;
        CHECK(okj && cJSON_IsTrue(okj));
        if (nj && cJSON_IsString(nj)) snprintf(name, sizeof(name), "%s", nj->valuestring);
        if (g) cJSON_Delete(g);
        free(gjson);
    }
    CHECK(name[0] != '\0');
    CHECK(tool_register_generated(reg, skills, "auto_fixed_tool", name) == 0);

    const tool *t = tool_find(reg, "auto_fixed_tool");
    CHECK(t != NULL);
    if (t) {
        CHECK(strstr(t->description, "generated plugin") != NULL);
        tool_ctx tctx;
        memset(&tctx, 0, sizeof(tctx));
        tctx.reg = reg;
        tctx.skills = skills;
        tool_result *res = tool_execute(reg, "auto_fixed_tool", "{}", &tctx);
        CHECK(res != NULL);
        if (res) {
            CHECK(res->ok == 1);
            CHECK(res->output && strstr(res->output, "plugin:") != NULL);
            tool_result_free(res);
        }
    }
    /* rebinding the same tool name is idempotent (already registered) */
    CHECK(tool_register_generated(reg, skills, "auto_fixed_tool", name) == 0);
    CHECK(tool_find(reg, "auto_fixed_tool") != NULL);

    /* bad args */
    CHECK(tool_register_generated(NULL, skills, "x", name) == -1);
    CHECK(tool_register_generated(reg, NULL, "x", name) == -1);
    CHECK(tool_register_generated(reg, skills, NULL, name) == -1);
    CHECK(tool_register_generated(reg, skills, "y", "no_such_skill") == -1);

    tool_registry_free(reg);
    skill_registry_free(skills);
}

/* ---------- memory graph + consolidation ---------- */
static void test_memory_graph(void) {
    section("memory graph + consolidation");
    memory *m = memory_new(NULL);
    CHECK(m != NULL);
    if (!m) return;

    memory_record_edge(m, "taskA", "file_write", "used_tool");
    memory_record_edge(m, "file_write", "a.txt", "touched");
    memory_record_edge(m, "taskA", "file_write", "used_tool"); /* dup folded */

    char *gj = memory_graph_json(m);
    CHECK(gj != NULL);
    CHECK(gj && strstr(gj, "taskA") && strstr(gj, "used_tool"));
    free(gj);

    /* token-based recall: query "taskA" finds both edges */
    char *rel = memory_graph_related(m, "taskA", 10);
    CHECK(rel != NULL);
    CHECK(rel && strstr(rel, "used_tool") != NULL);
    free(rel);
    /* unrelated query -> empty array */
    rel = memory_graph_related(m, "zzz_nothing", 10);
    CHECK(rel != NULL);
    CHECK(rel && strcmp(rel, "[]") == 0);
    free(rel);

    /* consolidation: a token appearing in >= 3 episode tasks becomes a fact */
    memory_record_experience(m, "fix the parser bug", "done");
    memory_record_experience(m, "parser cleanup task", "done");
    memory_record_experience(m, "extend parser tests", "done");
    int facts = memory_consolidate(m);
    CHECK(facts >= 1);
    const char *fact = memory_recall(m, "topic.parser");
    CHECK(fact != NULL);
    CHECK(fact && strstr(fact, "3") != NULL);

    memory_free(m);
}

/* ---------- file_edit / glob / grep tools (Claude Code ports) ---------- */
static void test_edit_search(void) {
    section("file_edit + glob + grep");
    /* hermetic start: a killed earlier run may have left files behind */
    fs_remove("tw-edit/a.txt");
    fs_remove("tw-edit/b.txt");
    fs_remove("tw-edit/sub/c.txt");
#ifdef _WIN32
    RemoveDirectoryA("tw-edit/sub");
    RemoveDirectoryA("tw-edit");
#else
    rmdir("tw-edit/sub");
    rmdir("tw-edit");
#endif
    tool_registry *reg = tool_registry_new();
    tool_register_builtins(reg);
    CHECK(tool_find(reg, "file_edit") != NULL);
    CHECK(tool_find(reg, "glob") != NULL);
    CHECK(tool_find(reg, "grep") != NULL);

    tool_ctx tctx;
    memset(&tctx, 0, sizeof(tctx));
    tctx.reg = reg;
    tctx.workspace = "tw-edit";
    fs_mkdirs("tw-edit");

    /* seed files */
    tool_result *r = tool_execute(
        reg, "file_write",
        "{\"path\":\"a.txt\",\"content\":\"foo bar\\nfoo baz\\n\"}", &tctx);
    CHECK(r != NULL && r->ok == 1);
    tool_result_free(r);
    r = tool_execute(reg, "file_write",
                        "{\"path\":\"b.txt\",\"content\":\"bar only\\n\"}", &tctx);
    CHECK(r != NULL && r->ok == 1);
    tool_result_free(r);
    r = tool_execute(reg, "file_write",
                        "{\"path\":\"sub/c.txt\",\"content\":\"nested bar\\n\"}", &tctx);
    CHECK(r != NULL && r->ok == 1);
    tool_result_free(r);

    /* --- file_edit --- */
    /* unique replacement */
    r = tool_execute(reg, "file_edit",
                        "{\"path\":\"a.txt\",\"old_string\":\"bar\",\"new_string\":\"qux\"}",
                        &tctx);
    CHECK(r != NULL && r->ok == 1);
    tool_result_free(r);
    char *d = fs_read_file("tw-edit/a.txt");
    CHECK(d != NULL && strstr(d, "foo qux") != NULL && strstr(d, "foo baz") != NULL);
    free(d);
    /* old_string not found -> error */
    r = tool_execute(reg, "file_edit",
                        "{\"path\":\"a.txt\",\"old_string\":\"nope\",\"new_string\":\"x\"}",
                        &tctx);
    CHECK(r != NULL && r->ok == 0);
    CHECK(r && r->output && strstr(r->output, "not found") != NULL);
    tool_result_free(r);
    /* not unique (two "foo") -> error without replace_all */
    r = tool_execute(reg, "file_edit",
                        "{\"path\":\"a.txt\",\"old_string\":\"foo\",\"new_string\":\"x\"}",
                        &tctx);
    CHECK(r != NULL && r->ok == 0);
    CHECK(r && r->output && strstr(r->output, "not unique") != NULL);
    tool_result_free(r);
    /* replace_all replaces every instance */
    r = tool_execute(reg, "file_edit",
                        "{\"path\":\"a.txt\",\"old_string\":\"foo\",\"new_string\":\"x\",\"replace_all\":true}",
                        &tctx);
    CHECK(r != NULL && r->ok == 1);
    tool_result_free(r);
    d = fs_read_file("tw-edit/a.txt");
    CHECK(d != NULL && strstr(d, "x qux") != NULL && strstr(d, "x baz") != NULL &&
          strstr(d, "foo") == NULL);
    free(d);
    /* missing file -> error mentioning file_write */
    r = tool_execute(reg, "file_edit",
                        "{\"path\":\"missing.txt\",\"old_string\":\"a\",\"new_string\":\"b\"}",
                        &tctx);
    CHECK(r != NULL && r->ok == 0);
    CHECK(r && r->output && strstr(r->output, "file_write") != NULL);
    tool_result_free(r);

    /* --- glob --- */
    r = tool_execute(reg, "glob", "{\"pattern\":\"**/*.txt\"}", &tctx);
    CHECK(r != NULL && r->ok == 1);
    CHECK(r && r->output && strstr(r->output, "a.txt") != NULL &&
          r->output && strstr(r->output, "sub/c.txt") != NULL);
    tool_result_free(r);
    /* top-level-only pattern excludes nested files */
    r = tool_execute(reg, "glob", "{\"pattern\":\"*.txt\"}", &tctx);
    CHECK(r != NULL && r->ok == 1);
    CHECK(r && r->output && strstr(r->output, "a.txt") != NULL &&
          (!r->output || strstr(r->output, "sub/c.txt") == NULL));
    tool_result_free(r);
    /* no match */
    r = tool_execute(reg, "glob", "{\"pattern\":\"**/*.xyz\"}", &tctx);
    CHECK(r != NULL && r->ok == 1);
    CHECK(r && r->output && strstr(r->output, "No files found") != NULL);
    tool_result_free(r);

    /* --- grep --- */
    /* default mode: files_with_matches (a.txt no longer contains bar) */
    r = tool_execute(reg, "grep", "{\"pattern\":\"bar\"}", &tctx);
    CHECK(r != NULL && r->ok == 1);
    CHECK(r && r->output && strstr(r->output, "b.txt") != NULL &&
          r->output && strstr(r->output, "sub/c.txt") != NULL);
    tool_result_free(r);
    /* content mode: path:line:text */
    r = tool_execute(reg, "grep",
                        "{\"pattern\":\"baz\",\"output_mode\":\"content\"}", &tctx);
    CHECK(r != NULL && r->ok == 1);
    CHECK(r && r->output && strstr(r->output, "a.txt:2:x baz") != NULL);
    tool_result_free(r);
    /* count mode */
    r = tool_execute(reg, "grep", "{\"pattern\":\"bar\",\"output_mode\":\"count\"}",
                        &tctx);
    CHECK(r != NULL && r->ok == 1);
    CHECK(r && r->output && strstr(r->output, "b.txt:1") != NULL &&
          r->output && strstr(r->output, "sub/c.txt:1") != NULL);
    tool_result_free(r);
    /* case-insensitive */
    r = tool_execute(reg, "grep",
                        "{\"pattern\":\"BAZ\",\"ignore_case\":true,\"output_mode\":\"content\"}",
                        &tctx);
    CHECK(r != NULL && r->ok == 1);
    CHECK(r && r->output && strstr(r->output, "a.txt") != NULL);
    tool_result_free(r);
    /* glob filter narrows to one file */
    r = tool_execute(reg, "grep",
                        "{\"pattern\":\"bar\",\"glob\":\"b.txt\",\"output_mode\":\"count\"}",
                        &tctx);
    CHECK(r != NULL && r->ok == 1);
    CHECK(r && r->output && strstr(r->output, "b.txt:1") != NULL &&
          (!r->output || strstr(r->output, "sub/c.txt") == NULL));
    tool_result_free(r);
    /* head_limit truncates */
    r = tool_execute(reg, "grep",
                        "{\"pattern\":\"bar\",\"output_mode\":\"content\",\"head_limit\":1}",
                        &tctx);
    CHECK(r != NULL && r->ok == 1);
    CHECK(r && r->output && strstr(r->output, "truncated") != NULL);
    tool_result_free(r);
    /* no match */
    r = tool_execute(reg, "grep", "{\"pattern\":\"zzznothing\"}", &tctx);
    CHECK(r != NULL && r->ok == 1);
    CHECK(r && r->output && strstr(r->output, "No matches found") != NULL);
    tool_result_free(r);

    /* schema validation: required args enforced */
    char *err = NULL;
    CHECK(tool_validate_args(tool_find(reg, "file_edit"),
                                "{\"path\":\"a.txt\"}", &err) == -1);
    free(err);

    /* cleanup */
    fs_remove("tw-edit/a.txt");
    fs_remove("tw-edit/b.txt");
    fs_remove("tw-edit/sub/c.txt");
#ifdef _WIN32
    RemoveDirectoryA("tw-edit/sub");
    RemoveDirectoryA("tw-edit");
#else
    rmdir("tw-edit/sub");
    rmdir("tw-edit");
#endif
    tool_registry_free(reg);
}

/* ---------- snapshot: large-file guard + git-managed bypass ---------- */
static void test_snapshot_bigfile(void) {
    section("snapshot big-file guard + git bypass");
    /* shrink the capture limit so a small file exercises the skip path */
#ifdef _WIN32
    _putenv("COA_SNAPSHOT_MAX_FILE=1024");
#else
    setenv("COA_SNAPSHOT_MAX_FILE", "1024", 1);
#endif

    snapshot *snap = snapshot_open("state-test/snap-big");
    CHECK(snap != NULL);
    if (!snap) return;

    /* --- small file (under the 1KB limit) still round-trips --- */
    const char *small = "state-test/snap-big/small.txt";
    fs_write_file(small, "original-small", 14);
    CHECK(snapshot_capture(snap, small) == 0);
    fs_write_file(small, "modified-small!!", 16);
    CHECK(snapshot_restore_pending(snap) == 0);
    char *d = fs_read_file(small);
    CHECK(d != NULL && strcmp(d, "original-small") == 0);
    free(d);

    /* --- large file (over the limit): capture is skipped, and rollback
     * leaves the (modified) file alone instead of deleting it --- */
    const char *big = "state-test/snap-big/big.txt";
    char *bigbuf = (char *)malloc(4096);
    memset(bigbuf, 'A', 4096);
    fs_write_file(big, bigbuf, 4096);
    long long sz = fs_file_size(big);
    CHECK(sz == 4096);
    CHECK(snapshot_capture(snap, big) == 0); /* must not read the file */
    CHECK(fs_file_size(big) == 4096);        /* no temp copies either */
    memset(bigbuf, 'B', 4096);
    fs_write_file(big, bigbuf, 4096);
    CHECK(snapshot_restore_pending(snap) == 0);
    d = fs_read_file(big);
    CHECK(d != NULL && strlen(d) == 4096);      /* still there... */
    CHECK(d != NULL && d[0] == 'B');            /* ...with modified content (not deletable-restore) */
    free(d);
    free(bigbuf);
    fs_remove(big);
    fs_remove(small);
    snapshot_close(snap);
#ifdef _WIN32
    _putenv("COA_SNAPSHOT_MAX_FILE=");
#else
    unsetenv("COA_SNAPSHOT_MAX_FILE");
#endif

    /* --- git-managed workspace: tx does not snapshot, rollback is a no-op --- */
    fs_mkdirs("state-test/tw-git/.git");
    tool_registry *reg = tool_registry_new();
    tool_register_builtins(reg);
    tx_manager *tm = tx_manager_new();

    snapshot *snap2 = snapshot_open("state-test/snap-big2");
    CHECK(snap2 != NULL);

    tool_ctx gctx;
    memset(&gctx, 0, sizeof(gctx));
    gctx.reg = reg;
    gctx.workspace = "state-test/tw-git";
    tx *gtx = tx_begin(tm, snap2, reg, &gctx);
    CHECK(tx_run(gtx, "file_write", "{\"path\":\"f.txt\",\"content\":\"git-ver\"}") == 0);
    d = fs_read_file("state-test/tw-git/f.txt");
    CHECK(d != NULL && strcmp(d, "git-ver") == 0);
    free(d);
    /* rollback: no snapshot was taken, so the file stays as written */
    CHECK(tx_rollback(gtx) == 0);
    d = fs_read_file("state-test/tw-git/f.txt");
    CHECK(d != NULL && strcmp(d, "git-ver") == 0);
    free(d);
    tx_free(gtx);

    /* --- non-git workspace: same tx DOES snapshot and rollback removes --- */
    tool_ctx pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.reg = reg;
    pctx.workspace = "state-test/tw-plain";
    tx *ptx = tx_begin(tm, snap2, reg, &pctx);
    CHECK(tx_run(ptx, "file_write", "{\"path\":\"f.txt\",\"content\":\"plain-ver\"}") == 0);
    CHECK(tx_rollback(ptx) == 0);
    CHECK(fs_exists("state-test/tw-plain/f.txt") == 0); /* restored to absent */
    tx_free(ptx);

    tx_manager_free(tm);
    tool_registry_free(reg);
    snapshot_close(snap2);
    fs_remove("state-test/tw-git/f.txt");
}

/* ---------- agent loop: bounded multi-round plan->act->replan ---------- */
static void test_agent_loop(void) {
    section("agent loop (multi-round)");
    fs_mkdirs("state-test/loop-w");

    /* default rounds (8): analyze -> fix -> final text answer */
    {
        const char *f = "state-test/loop-w/a.txt";
        fs_write_file(f, "fixme OLD fixme", 15);
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/loop";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        char *ans = NULL;
        CHECK(reasoning_run(ctx.reasoning, "分析 a.txt 并修复其中的 OLD", &ans) == 0);
        CHECK(ans != NULL);
        CHECK(ans && strstr(ans, "[file_read]") != NULL);   /* round 1 observed */
        CHECK(ans && strstr(ans, "[file_edit]") != NULL);   /* round 2 applied */
        CHECK(ans && strstr(ans, "任务完成") != NULL);       /* final text */
        free(ans);
        /* the fix really landed on disk */
        char *content = fs_read_file(f);
        CHECK(content && strstr(content, "NEW") != NULL && strstr(content, "OLD") == NULL);
        free(content);
        runtime_shutdown(&ctx);
        fs_remove(f);
    }

    /* max_rounds=1 via <state_root>/cognitive-os-agent.json: single-shot — the loop runs
     * one round (the analyze read) and stops without ever fixing the file */
    {
        const char *f = "state-test/loop-w/b.txt";
        fs_write_file(f, "fixme OLD fixme", 15);
        fs_mkdirs("state-test/loop1");
        fs_write_file("state-test/loop1/cognitive-os-agent.json",
                         "{\"reasoning.max_rounds\":1}", 26);
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/loop1";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        char *ans = NULL;
        CHECK(reasoning_run(ctx.reasoning, "分析 b.txt 并修复其中的 OLD", &ans) == 0);
        CHECK(ans && strstr(ans, "[file_read]") != NULL);
        /* budget exhausted → forced tool-free synthesis instead of a dead note */
        CHECK(ans && strstr(ans, "综合回答") != NULL);
        free(ans);
        char *content = fs_read_file(f);
        CHECK(content && strstr(content, "OLD") != NULL); /* untouched */
        free(content);
        runtime_shutdown(&ctx);
        fs_remove(f);
        fs_remove("state-test/loop1/cognitive-os-agent.json");
    }

    /* plain chat is unchanged: no plan on round 1 -> answer is the LLM text */
    {
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/loop-chat";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        char *ans = NULL;
        CHECK(reasoning_run(ctx.reasoning, "你好", &ans) == 0);
        CHECK(ans != NULL && strstr(ans, "[") == NULL); /* no action lines */
        free(ans);
        runtime_shutdown(&ctx);
    }
}

/* ---------- chat history + upload RAG + self-evolution restart rebind ---------- */
static void test_chat_upload_evolve(void) {
    section("chat history / upload RAG / evolve rebind");

    /* multi-turn history is readable across runs (oldest first) */
    {
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/chat-hist";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        char *a1 = NULL, *a2 = NULL;
        CHECK(reasoning_run(ctx.reasoning, "你好", &a1) == 0);
        CHECK(reasoning_run(ctx.reasoning, "继续聊天", &a2) == 0);
        free(a1); free(a2);
        char *hj = reasoning_history_json(ctx.reasoning, 10);
        CHECK(hj && strstr(hj, "你好") != NULL && strstr(hj, "继续聊天") != NULL);
        if (hj) {
            cJSON *arr = cJSON_Parse(hj);
            CHECK(arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 2);
            if (arr) {
                cJSON *first = cJSON_GetArrayItem(arr, 0);
                cJSON *q = first ? cJSON_GetObjectItemCaseSensitive(first, "q") : NULL;
                CHECK(q && cJSON_IsString(q) && strstr(q->valuestring, "你好") != NULL);
                cJSON_Delete(arr);
            }
        }
        free(hj);
        runtime_shutdown(&ctx);
    }

    /* multi-session chat: two named sessions carry isolated histories; the
     * default session is untouched; session_clear wipes one session only */
    {
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/chat-sess";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        char *a = NULL;
        CHECK(reasoning_run_ex(ctx.reasoning, "tab-a", "苹果多少钱", &a) == 0);
        free(a); a = NULL;
        CHECK(reasoning_run_ex(ctx.reasoning, "tab-b", "天气怎么样", &a) == 0);
        free(a); a = NULL;
        CHECK(reasoning_run_ex(ctx.reasoning, "default", "第三个话题", &a) == 0);
        free(a); a = NULL;

        char *ha = reasoning_history_json_ex(ctx.reasoning, "tab-a", 10);
        CHECK(ha && strstr(ha, "苹果多少钱") && !strstr(ha, "天气怎么样")
              && !strstr(ha, "第三个话题"));
        free(ha);
        char *hb = reasoning_history_json_ex(ctx.reasoning, "tab-b", 10);
        CHECK(hb && strstr(hb, "天气怎么样") && !strstr(hb, "苹果多少钱"));
        free(hb);
        char *hd = reasoning_history_json(ctx.reasoning, 10); /* default */
        CHECK(hd && strstr(hd, "第三个话题") && !strstr(hd, "苹果多少钱"));
        free(hd);

        char *ss = reasoning_sessions_json(ctx.reasoning);
        CHECK(ss && strstr(ss, "tab-a") && strstr(ss, "tab-b")
              && strstr(ss, "\"default\""));
        free(ss);

        CHECK(reasoning_session_clear(ctx.reasoning, "tab-a") == 0);
        char *ha2 = reasoning_history_json_ex(ctx.reasoning, "tab-a", 10);
        CHECK(ha2 && strstr(ha2, "苹果多少钱") == NULL);
        free(ha2);
        /* clearing one session leaves the other intact */
        char *hb2 = reasoning_history_json_ex(ctx.reasoning, "tab-b", 10);
        CHECK(hb2 && strstr(hb2, "天气怎么样") != NULL);
        free(hb2);
        CHECK(reasoning_session_clear(ctx.reasoning, "no-such") == -1);

        runtime_shutdown(&ctx);
    }

    /* uploaded documents are recallable via the vector store (Chinese text
     * exercises the CJK bigram fallback of the local embedder) */
    {
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/up-rag";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        CHECK(ctx.memory != NULL);
        /* two long paragraphs (each > the ~600-byte chunk target) -> 2 chunks */
        strbuf doc;
        strbuf_init(&doc);
        for (int i = 0; i < 20; i++)
            strbuf_append(&doc, "埃菲尔铁塔位于法国巴黎，是著名的地标建筑。");
        strbuf_append(&doc, "\n\n");
        for (int i = 0; i < 20; i++)
            strbuf_append(&doc, "今天股市收盘上涨百分之二，成交量明显放大。");
        int n = memory_index_text(ctx.memory, "upload:notes.txt", doc.buf);
        strbuf_free(&doc);
        CHECK(n == 2);
        char *hits = memory_retrieve(ctx.memory, "埃菲尔铁塔在哪里", 3);
        CHECK(hits && strstr(hits, "埃菲尔铁塔") != NULL);
        free(hits);
        runtime_shutdown(&ctx);
    }

    /* missing-capability generation persists: after the drill, the tool is
     * re-bound from generated_tools.json in a FRESH context (no regeneration) */
    {
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/evo";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        char *ans = NULL;
        CHECK(reasoning_run(ctx.reasoning, "北京今天天气怎么样", &ans) == 0);
        free(ans);
        /* mapping persisted for the generated capability */
        char *gmap = tool_generated_load_mapping("state-test/evo");
        CHECK(gmap && strstr(gmap, "weather_lookup") != NULL);
        free(gmap);
        runtime_shutdown(&ctx);

        /* fresh init: the tool re-binds without a new generation run */
        runtime_ctx ctx2;
        if (init(&ctx2, &cfg) != 0) { CHECK(0); return; }
        CHECK(tool_find(ctx2.tools, "weather_lookup") != NULL);
        runtime_shutdown(&ctx2);
    }
}

/* ---------- policy rules: persistence + hard enforcement ---------- */
static void test_policy_rules(void) {
    section("policy rules (persist + deny hard-block)");

    /* save/load round-trip with decision strings */
    {
        policy_engine *pe = policy_engine_new();
        policy_add_rule(pe, "file_write", "deny", "readonly mode");
        policy_add_rule(pe, "*", "allow", NULL);
        fs_mkdirs("state-test/policy-round");
        CHECK(policy_save_file(pe, "state-test/policy-round/policy.json") == 0);
        policy_engine *pe2 = policy_engine_new();
        CHECK(policy_load_file(pe2, "state-test/policy-round/policy.json") == 2);
        /* exact deny beats wildcard allow regardless of order */
        CHECK(policy_check(pe2, "file_write", "{}", NULL) == POLICY_DENY);
        CHECK(policy_check(pe2, "file_read", "{}", NULL) == POLICY_ALLOW);
        const char *tool = NULL, *action = NULL, *reason = NULL;
        CHECK(policy_rule_get(pe2, 0, &tool, &action, &reason) == 0);
        CHECK(strcmp(tool, "file_write") == 0 && strcmp(action, "deny") == 0 &&
              reason && strcmp(reason, "readonly mode") == 0);
        /* removal works */
        policy_remove_rule(pe2, 0);
        CHECK(policy_rule_count(pe2) == 1);
        CHECK(policy_check(pe2, "file_write", "{}", NULL) == POLICY_ALLOW);
        policy_engine_free(pe);
        policy_engine_free(pe2);
    }

    /* e2e: a persisted deny rule keeps file_write out of the run — the mock
     * planner still emits the action, the execution layer must hard-block it */
    {
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/policy-e2e";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        policy_add_rule(ctx.policy, "file_write", "deny", "readonly guard");

        char *ans = NULL;
        CHECK(reasoning_run(ctx.reasoning, "创建 blocked.txt 写入内容 x", &ans) == 0);
        CHECK(ans && strstr(ans, "denied by policy") != NULL);
        free(ans);
        /* the file must NOT exist (hard block, not just a warning) */
        char *data = fs_read_file("state-test/loop-w/blocked.txt");
        CHECK(data == NULL);
        free(data);
        runtime_shutdown(&ctx);
    }
}

/* ---------- multi-agent orchestration: decompose -> execute -> merge ---------- */
static void test_orchestrate(void) {
    section("multi-agent orchestration");

    /* full pipeline: mock decompose assigns to "alpha", the plan compiles to a
     * Flow DAG, flow_run executes it (the worker writes the file), the merge
     * synthesizes the final answer; the flow trace lands on the board */
    {
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/orch";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        CHECK(agent_pool_add(ctx.agents, "alpha", "writer") >= 0);

        char *ans = NULL, *trace = NULL;
        CHECK(orchestrate(&ctx, "创建 orch.txt 写入内容为 orch-ok", &ans, &trace) == 0);
        CHECK(ans && strstr(ans, "综合完成") != NULL);
        CHECK(trace && strstr(trace, "\"agent\":\"alpha\"") != NULL &&
              strstr(trace, "\"status\":\"ok\"") != NULL);
        free(ans); free(trace);

        /* the worker actually executed (file written) and results hit the board */
        char *data = fs_read_file("state-test/loop-w/orch.txt");
        CHECK(data && strstr(data, "orch-ok") != NULL);
        free(data);
        char *tr = blackboard_get(ctx.blackboard, "flow/trace");
        CHECK(tr && strstr(tr, "alpha") != NULL);
        free(tr);
        char *fin = blackboard_get(ctx.blackboard, "flow/final");
        CHECK(fin && strstr(fin, "综合完成") != NULL);
        free(fin);

        /* per-agent blackboard keys don't collide between agents */
        CHECK(agent_pool_add(ctx.agents, "beta", "reviewer") >= 0);
        char *a2 = NULL;
        CHECK(agent_run(&ctx, "alpha", "纯聊天模式回复即可", &a2) == 0);
        char *k1 = blackboard_get(ctx.blackboard, "result:alpha");
        CHECK(k1 != NULL);
        free(k1);
        free(a2);
        runtime_shutdown(&ctx);
    }

    /* fallback: no registered agents -> plain single-agent run */
    {
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/orch-fb";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        char *ans = NULL;
        CHECK(orchestrate(&ctx, "你好", &ans, NULL) == 0);
        CHECK(ans && *ans != '\0');
        free(ans);
        runtime_shutdown(&ctx);
    }
}

/* decompose-only: task compiles to an inspectable Flow DAG (no execution) */
static void test_flow_decompose(void) {
    section("flow decompose (LLM plan -> DAG)");
    config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_root = "state-test/flow-decomp";
    cfg.workspace = "state-test/loop-w";
    cfg.provider = "mock";
    cfg.http_port = 0;
    runtime_ctx ctx;
    if (init(&ctx, &cfg) != 0) { CHECK(0); return; }

    /* no agents -> no plan */
    char *dag = NULL;
    CHECK(flow_decompose(&ctx, "创建 a.txt 写入内容为 x", &dag) == -1);
    CHECK(dag == NULL);

    /* with agents -> valid DAG, nothing executed */
    CHECK(agent_pool_add(ctx.agents, "alpha", "writer") >= 0);
    CHECK(flow_decompose(&ctx, "创建 a.txt 写入内容为 x", &dag) == 0);
    CHECK(dag && strstr(dag, "\"agent\":\"alpha\"") != NULL &&
          strstr(dag, "\"nodes\"") != NULL);
    free(dag);
    char *data = fs_read_file("state-test/loop-w/a.txt");
    CHECK(data == NULL); /* decompose must not run the nodes */
    free(data);
    runtime_shutdown(&ctx);
}

/* ---------- Flow Compiler: DAG validation + topological execution ---------- */
static void test_flow(void) {
    section("flow compiler (DAG)");

    /* validation: cycle detection, duplicate ids, unknown edge endpoints */
    {
        char *err = NULL;
        CHECK(flow_validate("{\"nodes\":[{\"id\":\"a\",\"agent\":\"x\","
                               "\"task\":\"t\"},{\"id\":\"b\",\"agent\":\"x\","
                               "\"task\":\"t\"}],\"edges\":[{\"from\":\"a\","
                               "\"to\":\"b\"}]}", &err) == 0);
        free(err);

        err = NULL;
        CHECK(flow_validate("{\"nodes\":[{\"id\":\"a\",\"agent\":\"x\","
                               "\"task\":\"t\"},{\"id\":\"b\",\"agent\":\"x\","
                               "\"task\":\"t\"}],\"edges\":[{\"from\":\"a\","
                               "\"to\":\"b\"},{\"from\":\"b\",\"to\":\"a\"}]}",
                               &err) == -1);
        CHECK(err && strstr(err, "cycle") != NULL);
        free(err);

        err = NULL;
        CHECK(flow_validate("{\"nodes\":[{\"id\":\"a\",\"agent\":\"x\","
                               "\"task\":\"t\"},{\"id\":\"a\",\"agent\":\"x\","
                               "\"task\":\"t\"}]}", &err) == -1);
        CHECK(err && strstr(err, "duplicate") != NULL);
        free(err);

        err = NULL;
        CHECK(flow_validate("{\"nodes\":[{\"id\":\"a\",\"agent\":\"x\","
                               "\"task\":\"t\"}],\"edges\":[{\"from\":\"a\","
                               "\"to\":\"ghost\"}]}", &err) == -1);
        CHECK(err && strstr(err, "unknown node") != NULL);
        free(err);
    }

    /* execution: 2-node chain with {{a}} substitution; results on the board */
    {
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/flow";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        CHECK(agent_pool_add(ctx.agents, "alpha", "writer") >= 0);
        CHECK(agent_pool_add(ctx.agents, "beta", "reviewer") >= 0);

        const char *dag =
            "{\"nodes\":["
            "{\"id\":\"a\",\"agent\":\"alpha\",\"task\":\"创建 flow-a.txt 写入内容为 flow-ok\"},"
            "{\"id\":\"b\",\"agent\":\"beta\",\"task\":\"总结以下内容: {{a}}\"}],"
            "\"edges\":[{\"from\":\"a\",\"to\":\"b\"}]}";

        char *ans = NULL, *trace = NULL;
        CHECK(flow_run(&ctx, dag, &ans, &trace) == 0);
        CHECK(ans && *ans != '\0');
        /* both nodes ran; {{a}} was substituted away in b's recorded task */
        CHECK(trace && strstr(trace, "\"id\":\"a\"") != NULL &&
              strstr(trace, "\"id\":\"b\"") != NULL &&
              strstr(trace, "\"status\":\"ok\"") != NULL);
        CHECK(trace && strstr(trace, "{{a}}") == NULL);
        free(ans); free(trace);

        /* node a actually executed its file write */
        char *data = fs_read_file("state-test/loop-w/flow-a.txt");
        CHECK(data && strstr(data, "flow-ok") != NULL);
        free(data);
        char *tr = blackboard_get(ctx.blackboard, "flow/trace");
        CHECK(tr && strstr(tr, "beta") != NULL);
        free(tr);
        runtime_shutdown(&ctx);
    }

    /* run rejects unregistered agents and cycles */
    {
        config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_root = "state-test/flow-bad";
        cfg.workspace = "state-test/loop-w";
        cfg.provider = "mock";
        cfg.http_port = 0;
        runtime_ctx ctx;
        if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
        char *ans = NULL;
        CHECK(flow_run(&ctx,
            "{\"nodes\":[{\"id\":\"a\",\"agent\":\"ghost\",\"task\":\"t\"}]}",
            &ans, NULL) == -1);
        CHECK(flow_run(&ctx,
            "{\"nodes\":[{\"id\":\"a\",\"agent\":\"alpha\",\"task\":\"t\"},"
            "{\"id\":\"b\",\"agent\":\"alpha\",\"task\":\"t\"}],"
            "\"edges\":[{\"from\":\"a\",\"to\":\"b\"},{\"from\":\"b\",\"to\":\"a\"}]}",
            &ans, NULL) == -1);
        free(ans);
        runtime_shutdown(&ctx);
    }
}

/* ---------- MCP: node availability + live mock server ---------- */
static int node_available(void) {
    proc_result *r = proc_run("node --version", 5000);
    int ok = r && r->exit_code == 0;
    proc_result_free(r);
    return ok;
}

static int python_available(void) {
    proc_result *r = proc_run("python --version", 5000);
    int ok = r && r->exit_code == 0;
    proc_result_free(r);
    return ok;
}

static void wait_for_mcp(mcp_manager *m, const char *server) {
    for (int i = 0; i < 40; i++) { /* up to ~4s */
        char *out = NULL, *err = NULL;
        int rc = mcp_manager_call(m, server, "echo", "{\"text\":\"ping\"}", &out, &err);
        free(out); free(err);
        if (rc == 0) return;
        time_sleep_ms(100);
    }
}

static void test_mcp_standard(void) {
    section("mcp standard protocol (http)");
    if (!node_available()) { printf("  (node not available, skipped)\n"); return; }
    /* spawn detached so it survives this call */
    CHECK(proc_spawn_detached("node tools/mock_mcp_server.js --port 9321") == 0);

    mcp_manager *m = mcp_manager_new();
    CHECK(m != NULL);
    if (!m) return;
    CHECK(mcp_manager_add(m, "mock", "http://127.0.0.1:9321/mcp", NULL) == 0);
    wait_for_mcp(m, "mock");

    /* standard tools/call over http */
    {
        char *out = NULL, *err = NULL;
        int rc = mcp_manager_call(m, "mock", "echo", "{\"text\":\"hi\"}", &out, &err);
        CHECK(rc == 0);
        CHECK(out && strstr(out, "echo: hi") != NULL);
        free(out); free(err);
    }
    /* unknown tool -> error result */
    {
        char *out = NULL, *err = NULL;
        CHECK(mcp_manager_call(m, "mock", "nope", "{}", &out, &err) != 0);
        free(out); free(err);
    }
    /* dynamic registration: mcp__mock__echo lands in the registry with schema */
    tool_registry *reg = tool_registry_new();
    CHECK(reg != NULL);
    int n = mcp_manager_sync_tools(m, reg);
    CHECK(n >= 1);
    const tool *et = reg ? tool_find(reg, "mcp__mock__echo") : NULL;
    CHECK(et != NULL);
    CHECK(et && et->json_schema && strstr(et->json_schema, "text") != NULL);
    /* execute through the normal tool path */
    if (et) {
        tool_ctx tctx;
        memset(&tctx, 0, sizeof(tctx));
        tctx.reg = reg;
        tctx.mcp = m;
        tool_result *r = tool_execute(reg, "mcp__mock__echo", "{\"text\":\"toolpath\"}", &tctx);
        CHECK(r != NULL && r->ok == 1 && r->output && strstr(r->output, "echo: toolpath") != NULL);
        tool_result_free(r);
    }
    /* persist + reload round-trip */
    fs_mkdirs("state-test-mcp");
    CHECK(mcp_manager_persist(m, "state-test-mcp") == 0);
    {
        mcp_manager *m2 = mcp_manager_new();
        CHECK(mcp_manager_load(m2, "state-test-mcp") == 0);
        const mcp_conn *c = mcp_manager_find(m2, "mock");
        CHECK(c != NULL && c->url && strstr(c->url, "9321") != NULL);
        if (c) CHECK(strcmp(c->transport, "http") == 0);
        mcp_manager_free(m2);
    }
    /* tokens must survive persist/load (auth silently broken after restart) */
    {
        mcp_manager *mt = mcp_manager_new();
        mcp_conn tc;
        memset(&tc, 0, sizeof(tc));
        tc.name = (char *)"toksrv";
        tc.transport = (char *)"http";
        tc.url = (char *)"http://127.0.0.1:9321/mcp";
        tc.token = (char *)"secret-token-123";
        CHECK(mcp_manager_add_ex(mt, &tc) == 0);
        CHECK(mcp_manager_persist(mt, "state-test-mcp") == 0);
        mcp_manager *m2 = mcp_manager_new();
        CHECK(mcp_manager_load(m2, "state-test-mcp") == 0);
        const mcp_conn *ct = mcp_manager_find(m2, "toksrv");
        CHECK(ct && ct->token && strcmp(ct->token, "secret-token-123") == 0);
        mcp_manager_free(m2);
        mcp_manager_free(mt);
    }
    fs_remove("state-test-mcp/mcp.json");
    fs_remove("state-test-mcp");

    /* repeated re-sync must not disturb the registry */
    CHECK(mcp_manager_sync_tools(m, reg) >= 1);
    CHECK(tool_find(reg, "mcp__mock__echo") != NULL);
    CHECK(mcp_manager_sync_tools(m, reg) >= 1);
    CHECK(tool_find(reg, "mcp__mock__echo") != NULL);

    /* graceful shutdown of the mock server via its shutdown tool */
    {
        char *out = NULL, *err = NULL;
        mcp_manager_call(m, "mock", "shutdown", "{}", &out, &err);
        free(out); free(err);
    }
    /* removing the server must unregister its dynamic tools (no zombie
     * tools left callable by the agent) */
    CHECK(mcp_manager_remove(m, "mock", reg) == 0);
    CHECK(tool_find(reg, "mcp__mock__echo") == NULL);
    tool_registry_free(reg);
    mcp_manager_free(m);
}

static void test_mcp_stdio(void) {
    section("mcp stdio transport");
    if (!node_available()) { printf("  (node not available, skipped)\n"); return; }
    mcp_manager *m = mcp_manager_new();
    CHECK(m != NULL);
    if (!m) return;
    mcp_conn c;
    memset(&c, 0, sizeof(c));
    c.name = (char *)"mocks";
    c.transport = (char *)"stdio";
    c.command = (char *)"node";
    /* comma-separated args with no spaces: argv sizing must account for
     * every separator, not just spaces */
    c.args_csv = (char *)"tools/mock_mcp_server.js,--stdio,--probe-a,--probe-b,--probe-c";
    CHECK(mcp_manager_add_ex(m, &c) == 0);

    /* first call spawns the child lazily and runs the handshake */
    char *out = NULL, *err = NULL;
    int rc = -1;
    for (int i = 0; i < 10 && rc != 0; i++) {
        rc = mcp_manager_call(m, "mocks", "echo", "{\"text\":\"stdio-test\"}", &out, &err);
        if (rc != 0) {
            printf("  stdio call attempt %d failed: %s\n", i, err ? err : "?");
            free(out); free(err); out = NULL; err = NULL; time_sleep_ms(200);
        }
    }
    CHECK(rc == 0);
    CHECK(out && strstr(out, "echo: stdio-test") != NULL);
    free(out); free(err);

    /* second call reuses the persistent child */
    out = err = NULL;
    CHECK(mcp_manager_call(m, "mocks", "echo", "{\"text\":\"again\"}", &out, &err) == 0);
    CHECK(out && strstr(out, "echo: again") != NULL);
    free(out); free(err);

    /* dynamic registration over stdio */
    tool_registry *reg = tool_registry_new();
    CHECK(reg != NULL);
    CHECK(mcp_manager_sync_tools(m, reg) >= 1);
    CHECK(reg && tool_find(reg, "mcp__mocks__echo") != NULL);

    tool_registry_free(reg);
    mcp_manager_free(m); /* kills the child */
}

/* ---------- cluster ---------- */
static void test_cluster(void) {
    section("cluster");
    cluster *c = cluster_new();
    CHECK(c != NULL);
    if (!c) return;
    CHECK(cluster_upsert(c, "n1", "10.0.0.1", 8080, "worker") == 0);
    CHECK(cluster_upsert(c, "n2", "10.0.0.2", 8080, "coordinator") == 0);
    CHECK(cluster_count(c) == 2);
    CHECK(cluster_up_count(c) == 2);

    cluster_mark_down(c, -1); /* force every node stale */
    CHECK(cluster_up_count(c) == 0);
    const cluster_node *n2 = cluster_find(c, "n2");
    CHECK(n2 != NULL && strcmp(n2->status, "down") == 0);

    CHECK(cluster_heartbeat(c, "n1") == 0);
    CHECK(cluster_up_count(c) == 1);
    const cluster_node *n1 = cluster_find(c, "n1");
    CHECK(n1 != NULL && strcmp(n1->status, "up") == 0);

    char *j = cluster_json(c);
    CHECK(j && strstr(j, "n2") != NULL);
    free(j);
    CHECK(cluster_remove(c, "n1") == 0);
    CHECK(cluster_count(c) == 1);
    cluster_free(c);

    /* capability tags + heartbeat-driven liveness cycle (3 missed periods) */
    c = cluster_new();
    if (!c) { CHECK(0); return; }
    CHECK(cluster_upsert_ex(c, "w1", "10.0.0.5", 9000, "worker",
                               "llm,tools,mcp") == 0);
    const cluster_node *w = cluster_find(c, "w1");
    CHECK(w != NULL && strcmp(w->caps, "llm,tools,mcp") == 0);
    CHECK(cluster_upsert_ex(c, "w2", "10.0.0.6", 9001, "worker", NULL) == 0);
    w = cluster_find(c, "w2");
    CHECK(w != NULL && w->caps && *w->caps == '\0');
    /* simulate missed heartbeats: everything stale except freshly-beaten w1 */
    cluster_mark_down(c, -1);
    CHECK(cluster_up_count(c) == 0);
    CHECK(cluster_heartbeat(c, "w1") == 0);
    CHECK(cluster_up_count(c) == 1);
    /* unknown node heartbeat rejected */
    CHECK(cluster_heartbeat(c, "ghost") == -1);
    char *j2 = cluster_json(c);
    CHECK(j2 && strstr(j2, "\"caps\":\"llm,tools,mcp\"") != NULL &&
          strstr(j2, "\"caps\":\"\"") != NULL);
    free(j2);
    CHECK(cluster_remove(c, "w1") == 0);
    CHECK(cluster_remove(c, "w2") == 0);
    CHECK(cluster_count(c) == 0);
    cluster_free(c);
}

/* ---------- attention ---------- */
/* ---------- runtime: horizontal hook system ---------- */
static int hk_block(const char *event, const char *payload, void *ud) {
    (void)event; (void)payload;
    return *(int *)ud; /* nonzero = block */
}
static int hk_seen_event(const char *event, const char *payload, void *ud) {
    (void)payload;
    strbuf_append((strbuf *)ud, event);
    strbuf_append((strbuf *)ud, ",");
    return 0;
}

static void test_hook(void) {
    section("hook");
    hook_registry *h = hook_registry_new();
    CHECK(h != NULL);
    if (!h) return;

    /* exact-event matching: fires only for its own event */
    strbuf seen;
    strbuf_init(&seen);
    CHECK(hook_register(h, "exec.after_execute", hk_seen_event, &seen) > 0);
    CHECK(hook_dispatch(h, "exec.after_execute", "{\"tool\":\"t\"}") == 0);
    CHECK(seen.buf && strstr(seen.buf, "exec.after_execute"));
    CHECK(hook_dispatch(h, "exec.before_execute", NULL) == 0);
    CHECK(seen.buf && strstr(seen.buf, ",") && !strstr(seen.buf, "before"));

    /* wildcard "*" receives everything */
    strbuf wild;
    strbuf_init(&wild);
    CHECK(hook_register(h, "*", hk_seen_event, &wild) > 0);
    hook_dispatch(h, "agent.before_run", "{}");
    hook_dispatch(h, "exec.after_execute", NULL);
    CHECK(wild.buf && strstr(wild.buf, "agent.before_run"));
    CHECK(wild.buf && strstr(wild.buf, "exec.after_execute"));

    /* blocking hook: nonzero return => dispatch reports 1 */
    int block = 1;
    CHECK(hook_register(h, "exec.before_execute", hk_block, &block) > 0);
    CHECK(hook_dispatch(h, "exec.before_execute", "{}") == 1);
    block = 0;
    CHECK(hook_dispatch(h, "exec.before_execute", "{}") == 0);

    /* registry listing + unregister */
    char *js = hook_registry_json(h);
    CHECK(js && strstr(js, "exec.after_execute") && strstr(js, "\"*\""));
    free(js);
    CHECK(hook_unregister(h, 999) == -1);
    CHECK(hook_unregister(h, 1) == 0);
    CHECK(hook_unregister(h, 1) == -1); /* already removed */
    strbuf_free(&seen);
    strbuf_free(&wild);
    hook_registry_free(h);

    /* builtin audit hook writes JSONL */
    const char *path = "state-hook-test.jsonl";
    fs_remove(path);
    hook_registry *h2 = hook_registry_new();
    CHECK(h2 != NULL);
    CHECK(hook_register(h2, "*", hook_audit_file, (void *)path) > 0);
    hook_dispatch(h2, "agent.after_run", "{\"status\":\"done\"}");
    hook_registry_free(h2);
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (f) {
        char buf[512];
        int ok = fgets(buf, sizeof buf, f) != NULL &&
                 strstr(buf, "agent.after_run") && strstr(buf, "ts_ms");
        CHECK(ok);
        fclose(f);
    }
    fs_remove(path);
}

/* ---------- llm: routing policy (cost / latency / capability) ---------- */
static void test_router_policy(void) {
    section("router policy");
    router *r = router_new();
    CHECK(r != NULL);
    if (!r) return;
    CHECK(router_add_ex(r, "cheap", "openai", "https://c", "k", "m1", 1.0,
                           1, 0, "text") == 0);
    CHECK(router_add_ex(r, "fast", "openai", "https://f", "k", "m2", 1.0,
                           3, 80, "text,json") == 0);
    CHECK(router_add_ex(r, "vision", "openai", "https://v", "k", "m3", 1.0,
                           2, 200, "vision,json") == 0);

    /* cost: cheapest first */
    CHECK(router_set_policy(r, "cost") == 0);
    const route *p = router_pick(r);
    CHECK(p && strcmp(p->name, "cheap") == 0);
    p = router_pick(r);
    CHECK(p && strcmp(p->name, "cheap") == 0); /* single best keeps winning */

    /* latency: fastest (fast, 80ms) beats cheap (unknown) and vision (200) */
    CHECK(router_set_policy(r, "latency") == 0);
    p = router_pick(r);
    CHECK(p && strcmp(p->name, "fast") == 0);

    /* capability: only routes carrying the tag are picked */
    CHECK(router_set_policy(r, "capability:vision") == 0);
    for (int i = 0; i < 3; i++) {
        p = router_pick(r);
        CHECK(p && strcmp(p->name, "vision") == 0);
    }
    /* capability with no carrier degrades to full rotation */
    CHECK(router_set_policy(r, "capability:audio") == 0);
    p = router_pick(r);
    CHECK(p != NULL);

    /* unknown policy rejected; round_robin rotates */
    CHECK(router_set_policy(r, "bogus") == -1);
    CHECK(router_set_policy(r, "round_robin") == 0);
    const char *a = router_pick(r)->name;
    const char *b2 = router_pick(r)->name;
    CHECK(strcmp(a, b2) != 0);

    /* json + persistence carry the new fields */
    char *j = router_json(r);
    CHECK(j && strstr(j, "cost_rank") && strstr(j, "latency_ms") && strstr(j, "caps"));
    free(j);
    router_save_file(r, "state-test/routes-pol.json");
    router *r2 = router_new();
    CHECK(router_load_file(r2, "state-test/routes-pol.json") == 0);
    const route *g = router_get(r2, 0);
    CHECK(g && g->cost_rank == 1 && g->latency_ms == 0 && g->caps &&
          strcmp(g->caps, "text") == 0);
    router_free(r2);
    fs_remove("state-test/routes-pol.json");
    router_free(r);
}

/* ---------- memory: automatic consolidation (threshold + interval) ---------- */
static void test_consolidation(void) {
    section("memory consolidation auto");
    /* clean persisted state so the test is re-runnable (episodes persist) */
    fs_remove("state-test/consol/memory/facts.json");
    fs_remove("state-test/consol/memory/episodes.json");
    fs_remove("state-test/consol/memory/graph.json");
    fs_remove("state-test/consol/memory/vectors.json");
    memory *m = memory_new("state-test/consol");
    CHECK(m != NULL);
    if (!m) return;
    /* below threshold: skipped (episodes dedup by task string — use unique names) */
    memory_record_experience(m, "write report alpha one", "ok");
    memory_record_experience(m, "write report beta two", "ok");
    CHECK(memory_maybe_consolidate(m, 10, 0) == 0);
    /* reach the threshold: the pass runs */
    for (int i = 0; i < 9; i++) {
        char task[64];
        snprintf(task, sizeof(task), "write report gamma %d", i);
        memory_record_experience(m, task, "ok");
    }
    CHECK(memory_maybe_consolidate(m, 10, 0) == 1);
    CHECK(memory_consolidation_count(m) == 1);
    /* semantic fact distilled from the recurring theme */
    const char *fact = memory_recall(m, "topic.report");
    CHECK(fact && strstr(fact, "tasks") != NULL);
    /* procedural fact from the recurring used_tool edges */
    memory_record_edge(m, "write report gamma", "file_write", "used_tool");
    memory_record_edge(m, "write report delta", "file_write", "used_tool");
    memory_record_edge(m, "write report eps", "file_write", "used_tool");
    memory_record_experience(m, "write report zeta", "ok");
    CHECK(memory_maybe_consolidate(m, 1, 0) == 1);
    fact = memory_recall(m, "procedure.file_write");
    CHECK(fact && strstr(fact, "used in") != NULL);
    /* interval gate: too soon even with new episodes */
    memory_record_experience(m, "write report eta", "ok");
    CHECK(memory_maybe_consolidate(m, 1, 3600000) == 0);
    CHECK(memory_consolidation_count(m) == 2);
    memory_free(m);
}

/* ---------- memory lifecycle: reinforce / decay / forget / archive ---------- */
static void test_memory_lifecycle(void) {
    section("memory lifecycle");
    /* clean persisted state for re-runnability */
    fs_remove("state-test/lc-mem/memory/archive.jsonl");
    fs_remove("state-test/lc-mem/memory/episodes.json");
    fs_remove("state-test/lc-mem/memory/facts.json");
    fs_remove("state-test/lc-mem/memory/graph.json");
    fs_remove("state-test/lc-mem/memory/vectors.json");

    /* --- facade: reinforce (dedup = +1) + archive/forget below threshold --- */
    memory *m = memory_new("state-test/lc-mem");
    CHECK(m != NULL);
    if (!m) return;
    CHECK(memory_episode_count(m) == 0);
    memory_record_experience(m, "kept task", "ok");
    memory_record_experience(m, "kept task", "ok again");   /* reinforce -> 2.0 */
    memory_record_experience(m, "forgotten task", "ok");    /* stays 1.0 */
    memory_reinforce(m, "kept task");                        /* -> 3.0 */
    CHECK(memory_episode_count(m) == 2);
    CHECK(memory_lifecycle_pass(m, NULL) == 0);              /* no config: no-op */

    memory_lifecycle_cfg lc;
    memset(&lc, 0, sizeof(lc));
    lc.min_strength = 1.5;
    lc.archive = 1;
    int dropped = memory_lifecycle_pass(m, &lc);
    CHECK(dropped == 1);
    CHECK(memory_episode_count(m) == 1);
    CHECK(memory_recall(m, "ignored") == NULL); /* borrow-check filler */
    char *arch = fs_read_file("state-test/lc-mem/memory/archive.jsonl");
    CHECK(arch && strstr(arch, "forgotten task") != NULL);
    free(arch);
    memory_flush(m); /* persist the post-forget roster */
    memory_free(m);

    /* --- episode level: decay by age + drop_below + strength round-trip --- */
    episodic *e = episodic_new();
    CHECK(e != NULL);
    if (!e) return;
    long long now = time_now_ms();
    episodic_add_full(e, "old task", "r", now - 2 * 3600000LL, 1.0); /* 2h old */
    episodic_add_full(e, "new task", "r", now, 1.0);                 /* fresh */
    episodic_add_full(e, "strong task", "r", now - 2 * 3600000LL, 9.0);
    CHECK(episodic_count(e) == 3);
    CHECK(episodic_decay(e, now, 0, 0.001) == 0);     /* half_life<=0: no-op */
    CHECK(episodic_decay(e, now, 3600000LL, 0.001) == 2); /* old+strong decayed */
    CHECK(episodic_strength(e, 0) == 0.25);           /* 1.0 / 2^2 */
    CHECK(episodic_strength(e, 1) == 1.0);            /* age < half_life */
    CHECK(episodic_strength(e, 2) == 2.25);           /* 9.0 / 2^2 */
    /* reinforce refreshes strength */
    episodic_reinforce(e, "new task");
    CHECK(episodic_strength(e, 1) == 2.0);
    /* archive payload lists exactly the below-threshold episodes */
    char *below = episodic_below_json(e, 0.5);
    CHECK(below && strstr(below, "old task") != NULL);
    CHECK(below && strstr(below, "new task") == NULL);
    free(below);
    CHECK(episodic_drop_below(e, 0.5) == 1);
    CHECK(episodic_count(e) == 2);
    /* strength survives the JSON round-trip */
    char *j = episodic_json(e);
    CHECK(j && strstr(j, "strength") != NULL);
    free(j);
    episodic_free(e);

    /* --- persistence: strength restored through memory reload --- */
    memory *m2 = memory_new("state-test/lc-mem"); /* sees kept task (3.0) */
    CHECK(m2 != NULL);
    if (m2) {
        CHECK(memory_episode_count(m2) == 1);
        memory_free(m2);
    }
}

/* ---------- context MMU: explicit budgets with auto-degradation ---------- */
static void test_context_budget(void) {
    section("context budget");
    fs_mkdirs("state-test/budget");
    /* tiny budgets force every tier to degrade; the run must still work */
    static const char budget_cfg[] =
        "{\"context.budget_hot\":64,"
        "\"context.budget_warm\":64,"
        "\"context.budget_cold\":80}";
    fs_write_file("state-test/budget/cognitive-os-agent.json", budget_cfg,
                     sizeof(budget_cfg) - 1);
    config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_root = "state-test/budget";
    cfg.workspace = "state-test";
    cfg.provider = "mock";
    cfg.http_port = 0;
    runtime_ctx ctx;
    if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
    /* seed enough history to overflow the hot budget */
    for (int i = 0; i < 3; i++) {
        char *ans = NULL;
        CHECK(reasoning_run(ctx.reasoning, "你好", &ans) == 0);
        free(ans);
    }
    /* per-tier accounting is exposed as gauges */
    char *mx = metrics_render(ctx.metrics);
    CHECK(mx && strstr(mx, "context.bytes_hot") != NULL);
    CHECK(mx && strstr(mx, "context.bytes_warm") != NULL);
    CHECK(mx && strstr(mx, "context.bytes_cold") != NULL);
    free(mx);
    /* RAG-indexed content must respect the cold budget: retrieved section
     * (header + truncation marker) stays small */
    memory_index_document(ctx.memory, "doc1",
                             "long document about taxes and budgets and more",
                             "upload");
    char *ans = NULL;
    CHECK(reasoning_run(ctx.reasoning, "税收文档", &ans) == 0);
    CHECK(ans != NULL);
    free(ans);
    runtime_shutdown(&ctx);
    fs_remove("state-test/budget/cognitive-os-agent.json");
}

static void test_attention(void) {
    section("attention");
    attention *a = attention_new();
    CHECK(a != NULL);
    if (!a) return;
    attention_candidate cands[3];
    cands[0].text = "the weather in paris is sunny"; cands[0].tags = "weather"; cands[0].boost = 0.0;
    cands[1].text = "stock market report";           cands[1].tags = "finance"; cands[1].boost = 0.0;
    cands[2].text = "paris travel guide";            cands[2].tags = "travel";  cands[2].boost = 0.0;
    CHECK(attention_score(a, "paris weather", &cands[0]) >
          attention_score(a, "paris weather", &cands[1]));
    attention_result out[3];
    int k = attention_select(a, "paris weather", cands, 3, out, 3);
    CHECK(k == 3);
    CHECK(out[0].index == 0);
    CHECK(out[0].score >= out[1].score && out[1].score >= out[2].score);
    attention_free(a);
}

/* ---------- infra: lock-free ring buffer ---------- */
static void test_ringbuf(void) {
    section("ringbuf");
    ringbuf *r = ringbuf_new(8);
    CHECK(r != NULL);
    if (!r) return;
    /* basic FIFO */
    void *out = NULL;
    CHECK(ringbuf_pop(r, &out) == 0);            /* empty */
    for (int i = 1; i <= 8; i++) CHECK(ringbuf_push(r, (void *)(intptr_t)(size_t)i) == 1);
    CHECK(ringbuf_push(r, (void *)(intptr_t)9) == 0);  /* full */
    for (int i = 1; i <= 8; i++) {
        CHECK(ringbuf_pop(r, &out) == 1);
        CHECK((intptr_t)(size_t)out == i);
    }
    CHECK(ringbuf_pop(r, &out) == 0);
    /* wrap-around */
    for (int i = 1; i <= 4; i++) CHECK(ringbuf_push(r, (void *)(intptr_t)(size_t)i) == 1);
    for (int i = 1; i <= 4; i++) CHECK(ringbuf_pop(r, &out) == 1);
    for (int i = 5; i <= 12; i++) CHECK(ringbuf_push(r, (void *)(intptr_t)(size_t)i) == 1);
    for (int i = 5; i <= 12; i++) {
        CHECK(ringbuf_pop(r, &out) == 1);
        CHECK((intptr_t)(size_t)out == i);
    }
    ringbuf_free(r);
}

#define RB_NPROD 4
#define RB_PER   2000
#define RB_TOTAL (RB_NPROD * RB_PER)
static _Atomic int rb_seen[RB_TOTAL];
static _Atomic int rb_dup;
static _Atomic int rb_err;
static ringbuf *rb_shared;

static void rb_producer(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int k = 0; k < RB_PER; k++) {
        int v = id * RB_PER + k + 1;
        int i = 0;
        while (ringbuf_push(rb_shared, (void *)(intptr_t)(size_t)v) != 1 && i < 100000) { i++; time_sleep_ms(1); }
        if (i >= 100000) atomic_fetch_add(&rb_err, 1);
    }
}
static void rb_consumer(void *arg) {
    ringbuf *r = (ringbuf *)arg;
    int seen = 0;
    int64_t deadline = time_now_ms() + 8000;
    while (seen < RB_TOTAL && time_now_ms() < deadline) {
        void *out = NULL;
        if (ringbuf_pop(r, &out) == 1) {
            int v = (int)(intptr_t)(size_t)out;
            if (v < 1 || v > RB_TOTAL) { atomic_fetch_add(&rb_err, 1); continue; }
            int prev = atomic_fetch_add(&rb_seen[v - 1], 1);
            if (prev != 0) atomic_fetch_add(&rb_dup, 1);
            seen++;
        } else {
            time_sleep_ms(1);
        }
    }
    atomic_fetch_add(&rb_err, RB_TOTAL - seen);
}

static void test_ringbuf_mpmc(void) {
    section("ringbuf_mpmc");
    ringbuf *r = ringbuf_new(64);
    CHECK(r != NULL);
    if (!r) return;
    rb_shared = r;
    thread_t *prods[RB_NPROD];
    for (int i = 0; i < RB_NPROD; i++)
        prods[i] = thread_create(rb_producer, (void *)(intptr_t)i);
    thread_t *cons = thread_create(rb_consumer, r);
    for (int i = 0; i < RB_NPROD; i++) thread_join(prods[i]);
    thread_join(cons);
    ringbuf_free(r);
    rb_shared = NULL;
    CHECK(atomic_load(&rb_dup) == 0);
    CHECK(atomic_load(&rb_err) == 0);
    int distinct = 0;
    for (int i = 0; i < RB_TOTAL; i++)
        if (atomic_load(&rb_seen[i]) == 1) distinct++;
    CHECK(distinct == RB_TOTAL);
}

/* ---------- retrieval: embedding + rerank ---------- */
static void test_embedding(void) {
    section("embedding");
    embedding_use_local();
    CHECK_STR(embedding_provider_name(), "local");
    float a[EMBED_DIM], b[EMBED_DIM], c[EMBED_DIM];
    embed_text("hello world foo bar", a);
    embed_text("hello world foo bar", b);
    embed_text("completely different text here", c);
    CHECK(embed_cosine(a, b, EMBED_DIM) > 0.99f);
    CHECK(embed_cosine(a, c, EMBED_DIM) < embed_cosine(a, b, EMBED_DIM));
    /* rerank: relevant doc scores higher than unrelated doc */
    const char *docs[2] = {
        "how to create a file with hello content",
        "quantum entanglement of distant stars"
    };
    float scores[2];
    CHECK(embed_rerank("create file hello", docs, 2, scores) == 0);
    CHECK(scores[0] > scores[1]);
}

/* ---------- im: instant messaging store ---------- */
static void test_im(void) {
    section("im");
    const char *root = "state-im-test";
    char store[600];
    snprintf(store, sizeof(store), "%s/im/sessions.json", root);
    fs_remove(store);   /* remove stale store from a previous run */
    fs_remove(root);    /* best-effort (fails on non-empty dir) */
    im *im = im_new(root);
    CHECK(im != NULL);
    if (!im) return;
    int64_t s1 = im_create_session(im, "测试会话");
    CHECK(s1 > 0);
    int64_t s2 = im_create_session(im, "general");
    CHECK(s2 > 0 && s2 != s1);
    /* group session with members */
    const char *members[] = {"alice", "bob", "carol"};
    int64_t g1 = im_create_session_ex(im, "研发群", "group", members, 3);
    CHECK(g1 > 0);
    CHECK(im_send(im, s1, "user", "你好") > 0);
    CHECK(im_send_ex(im, s1, "assistant", "你好！", "cognitive-os-agent") > 0);
    CHECK(im_send_ex(im, g1, "user", "群聊消息 hello-group", "alice") > 0);
    CHECK(im_send_ex(im, g1, "assistant", "收到 hello-group", "cognitive-os-agent") > 0);
    CHECK(im_send(im, 9999, "user", "x") < 0);   /* unknown session */
    size_t n = 0;
    im_message *msgs = im_messages(im, s1, &n);
    CHECK(msgs != NULL && n == 2);
    if (msgs) {
        CHECK_STR(msgs[0].role, "user");
        CHECK_STR(msgs[0].content, "你好");
        CHECK(msgs[0].sender == NULL);
        CHECK_STR(msgs[1].role, "assistant");
        CHECK_STR(msgs[1].sender, "cognitive-os-agent");
    }
    im_messages_free(msgs, n);
    /* group membership surfaced */
    size_t ns = 0;
    im_session *sess = im_list_sessions(im, &ns);
    CHECK(sess != NULL && ns == 3);
    if (sess) {
        for (size_t i = 0; i < ns; i++) {
            if (sess[i].id == g1) {
                CHECK_STR(sess[i].kind, "group");
                CHECK(sess[i].n_members == 3);
                CHECK_STR(sess[i].members[1], "bob");
            }
        }
    }
    im_sessions_free(sess, ns);
    CHECK(im_total_messages(im) == 4);
    CHECK(im_delete_session(im, s2) == 1);
    CHECK(im_delete_session(im, 9999) == 0);
    im_free(im);
    /* reload from disk: kind/members/sender persisted */
    struct im *im2 = im_new(root);
    CHECK(im2 != NULL);
    if (im2) {
        size_t ns2 = 0;
        im_session *ss2 = im_list_sessions(im2, &ns2);
        CHECK(ss2 != NULL && ns2 == 2);
        if (ss2) {
            CHECK(ss2[0].id == s1);
            CHECK_STR(ss2[0].name, "测试会话");
            for (size_t i = 0; i < ns2; i++) {
                if (ss2[i].id == g1) {
                    CHECK_STR(ss2[i].kind, "group");
                    CHECK(ss2[i].n_members == 3);
                    CHECK_STR(ss2[i].members[2], "carol");
                }
            }
        }
        im_sessions_free(ss2, ns2);
        char *j = im_sessions_json(im2);
        CHECK(j && strstr(j, "测试会话") != NULL);
        CHECK(j && strstr(j, "研发群") != NULL);
        free(j);
        im_free(im2);
    }
    fs_remove(store);
    fs_remove(root);
}

static void test_im_search(void) {
    section("im_search");
    const char *root = "state-im-search-test";
    char store[600];
    snprintf(store, sizeof(store), "%s/im/sessions.json", root);
    fs_remove(store);
    im *im = im_new(root);
    CHECK(im != NULL);
    if (!im) return;
    int64_t s = im_create_session(im, "会议");
    CHECK(s > 0);
    CHECK(im_send(im, s, "user", "今天部署 v2 到生产") > 0);
    CHECK(im_send(im, s, "assistant", "确认，v2 已上线") > 0);
    CHECK(im_send(im, s, "user", "下午复盘 QEMU crash 日志") > 0);
    /* case-insensitive substring search */
    char *r = im_search(im, "v2", 20);
    CHECK(r && strstr(r, "部署 v2") != NULL && strstr(r, "已上线") != NULL);
    free(r);
    r = im_search(im, "qemu", 20);
    CHECK(r && strstr(r, "QEMU crash") != NULL);
    free(r);
    r = im_search(im, "不存在词", 20);
    CHECK(r && strcmp(r, "[]") == 0);
    free(r);
    im_free(im);
    fs_remove(store);
    fs_remove(root);
}

/* ---------- IM channel bridge (registry + session linkage, no network) ---------- */
static void test_im_bridge(void) {
    section("im_bridge");
    const char *root = "state-im-bridge-test";
    /* fs_remove only deletes single files (not dirs), so purge both
     * persisted store files explicitly for a deterministic run. */
    char sf[700], cf[700];
    snprintf(sf, sizeof(sf), "%s/im/sessions.json", root);
    snprintf(cf, sizeof(cf), "%s/im/channels.json", root);
    fs_remove(sf);
    fs_remove(cf);
    im *im = im_new(root);
    CHECK(im != NULL);
    if (!im) return;
    int64_t s1 = im_create_session(im, "手机会话");
    int64_t s2 = im_create_session(im, "普通会话");
    CHECK(s1 > 0 && s2 > 0);

    im_channels *cs = im_channels_new(root);
    CHECK(cs != NULL);
    if (cs) {
        im_channel ch;
        memset(&ch, 0, sizeof(ch));
        ch.name = "phone";
        ch.type = "telegram";
        ch.endpoint = "https://api.telegram.org";
        ch.token = "SECRET";
        ch.target = "999";
        ch.enabled = 1;
        CHECK(im_channel_register(cs, &ch) == 0);
        ch.name = "webhook";
        ch.type = "generic";
        ch.endpoint = "http://127.0.0.1:9000/hook";
        ch.token = NULL;
        ch.target = NULL;
        CHECK(im_channel_register(cs, &ch) == 0);

        CHECK(im_channel_count(cs) == 2);
        im_channel *f = im_channel_find(cs, "phone");
        CHECK(f != NULL && strcmp(f->type, "telegram") == 0);
        CHECK(strcmp(f->token, "SECRET") == 0);

        /* linkage: session -> channel */
        CHECK(im_session_set_channel(im, s1, "phone") == 0);
        CHECK(im_session_set_channel(im, s2, NULL) == 0);
        const char *chn = im_session_channel(im, s1);
        CHECK(chn && strcmp(chn, "phone") == 0);
        CHECK(im_session_channel(im, s2) == NULL);
        CHECK(im_session_by_channel(im, "phone") == s1);
        CHECK(im_session_by_channel(im, "webhook") < 0);
        CHECK(im_session_by_channel(im, "nope") < 0);

        char *json = im_channels_json(cs);
        CHECK(json && strstr(json, "\"phone\"") != NULL && strstr(json, "\"telegram\"") != NULL);
        free(json);

        /* removal drops the channel (session unlink is performed by the API
         * layer, which holds both the channel registry and the IM store) */
        CHECK(im_channel_remove(cs, "phone") == 0);
        CHECK(im_channel_count(cs) == 1);

        /* persistence: reload keeps the remaining channel */
        im_channels *cs2 = im_channels_new(root);
        CHECK(cs2 && im_channel_count(cs2) == 1);
        im_channels_free(cs2);
        im_channels_free(cs);
    }
    im_free(im);
    fs_remove(root);
}

/* ---------- plugin intelligence: AI plugin generation (mock mode) ---------- */
static void test_plugin_generate(void) {
    section("plugin_generate");
    const char *root = "state-plugin-test";
    fs_remove(root);
    config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_root = root;
    cfg.workspace = ".";
    cfg.provider = "mock";
    cfg.http_port = 0;
    runtime_ctx ctx;
    if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
    char *res = plugin_generate(&ctx, "创建读取配置文件 config.json 的插件");
    CHECK(res != NULL);
    if (res) {
        cJSON *j = cJSON_Parse(res);
        CHECK(j != NULL);
        if (j) {
            cJSON *ok = cJSON_GetObjectItemCaseSensitive(j, "ok");
            CHECK(ok && cJSON_IsTrue(ok));
            cJSON *plugin = cJSON_GetObjectItemCaseSensitive(j, "plugin");
            cJSON *name = plugin ? cJSON_GetObjectItemCaseSensitive(plugin, "name") : NULL;
            cJSON *script = cJSON_GetObjectItemCaseSensitive(j, "script");
            CHECK(name && cJSON_IsString(name) && strlen(name->valuestring) > 0);
            CHECK(script && cJSON_IsString(script));
        }
        free(res);
    }
    /* edge cases: missing/empty description are rejected up front */
    char *bad1 = plugin_generate(&ctx, "");
    CHECK(bad1 != NULL && strstr(bad1, "ok\":false") != NULL && strstr(bad1, "missing description") != NULL);
    free(bad1);
    char *bad2 = plugin_generate(&ctx, NULL);
    CHECK(bad2 != NULL && strstr(bad2, "ok\":false") != NULL);
    free(bad2);

    /* sandbox forbidden list guards the security gate (generator rejects these) */
    CHECK(sandbox_forbidden("rm -rf /") == 1);
    CHECK(sandbox_forbidden("rm -fr /tmp/x") == 1);
    CHECK(sandbox_forbidden("mkfs.ext4 /dev/sda") == 1);
    CHECK(sandbox_forbidden("echo hi") == 0);

    /* registered in the plugin registry */
    CHECK(plugin_registry_count(ctx.registry) >= 1);
    /* and runnable as a skill */
    CHECK(skill_count(ctx.skills) >= 1);
    skill_result *sr = NULL;
    for (size_t i = 0; i < (size_t)skill_count(ctx.skills); i++) {
        const skill *sk = skill_get(ctx.skills, i);
        if (sk && (strncmp(sk->name, "cap", 3) == 0 || strstr(sk->name, "config") != NULL)) {
            sr = skill_execute(ctx.skills, sk->name, NULL, ".", 10000);
            break;
        }
    }
    CHECK(sr != NULL && sr->ok);
    if (sr) skill_result_free(sr);
    runtime_shutdown(&ctx);
    fs_remove(root);
}

/* ---------- sandbox: wasm3 runner ---------- */
static const unsigned char WASM_ADD[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01,
    0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09,
    0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b
};

static void test_sandbox_wasm(void) {
    section("sandbox_wasm");
    /* reset any runner registered earlier in the suite (init wires
     * wasm3 automatically) so the "unsupported" seam is testable in any order */
    sandbox_set_wasm_runner(NULL);
    CHECK(sandbox_wasm_supported() == 0);
    char *we = sandbox_run_wasm("\0asm", 4, "add", "{}");
    CHECK(we != NULL && strstr(we, "not registered") != NULL);
    free(we);

    /* register the wasm3-backed runner */
    sandbox_set_wasm_runner(wasm3_run);
    CHECK(sandbox_wasm_supported() == 1);
    char *r = sandbox_run_wasm(WASM_ADD, sizeof(WASM_ADD), "add", "[2,40]");
    CHECK(r != NULL && strstr(r, "\"result\":42") != NULL);
    free(r);
    r = sandbox_run_wasm(WASM_ADD, sizeof(WASM_ADD), "add", "{\"a\":10,\"b\":32}");
    CHECK(r != NULL && strstr(r, "\"result\":42") != NULL);
    free(r);
    /* missing function -> error json */
    r = sandbox_run_wasm(WASM_ADD, sizeof(WASM_ADD), "nope", "[]");
    CHECK(r != NULL && strstr(r, "ok\":false") != NULL);
    free(r);
}

/* ---------- runtime: task lifecycle ---------- */
static void test_task(void) {
    section("task");
    task *t = task_new(42, 3, "hello task", 1000);
    CHECK(t != NULL);
    if (!t) return;
    CHECK(t->status == TS_QUEUED);
    CHECK_STR(t->input, "hello task");
    CHECK_STR(task_status_name(TS_FAILED), "failed");
    task_transition(t, TS_RUNNING, 0);
    CHECK(t->started_ms > 0);
    task_transition(t, TS_DONE, 0);
    CHECK(t->finished_ms > 0);
    CHECK(t->status == TS_DONE);
    char *j = task_to_json(t);
    CHECK(j && strstr(j, "hello task") != NULL && strstr(j, "done") != NULL);
    free(j);
    task_free(t);
}

/* ---------- infra: model/MCP catalog JSON ---------- */
static void test_catalog(void) {
    section("catalog");
    char *m = catalog_models_json();
    CHECK(m != NULL);
    if (m) {
        CHECK(strstr(m, "\"ollama\"") != NULL && strstr(m, "\"groq\"") != NULL &&
              strstr(m, "\"deepseek\"") != NULL && strstr(m, "\"gemini\"") != NULL);
        /* free-key signup links + local-runtime flag (#60) */
        CHECK(strstr(m, "\"signup_url\":\"https://console.groq.com/keys\"") != NULL);
        CHECK(strstr(m, "\"signup_url\":\"https://platform.deepseek.com/api_keys\"") != NULL);
        CHECK(strstr(m, "\"ollama\"") != NULL && strstr(m, "\"local\":true") != NULL);
        CHECK(strstr(m, "\"groq\"") != NULL && strstr(m, "\"local\":false") != NULL);
        free(m);
    }
    char *mc = catalog_mcp_json();
    CHECK(mc != NULL);
    if (mc) {
        CHECK(strstr(mc, "\"mock-echo\"") != NULL && strstr(mc, "\"github\"") != NULL);
        /* GitHub 热门 MCP 应用条目（含 repo 链接） */
        CHECK(strstr(mc, "\"fetch\"") != NULL && strstr(mc, "\"memory\"") != NULL);
        CHECK(strstr(mc, "\"sequential-thinking\"") != NULL && strstr(mc, "\"puppeteer\"") != NULL);
        CHECK(strstr(mc, "github.com/modelcontextprotocol/servers") != NULL);
        CHECK(strstr(mc, "\"repo\":\"https://github.com/github/github-mcp-server\"") != NULL);
        /* 广场扩充：browserbase + 参考类条目（transport=reference） */
        CHECK(strstr(mc, "\"browserbase\"") != NULL);
        CHECK(strstr(mc, "browserbase/mcp-server-browserbase") != NULL);
        CHECK(strstr(mc, "punkpeye/awesome-mcp-servers") != NULL);
        CHECK(strstr(mc, "wong2/mcp-cli") != NULL);
        CHECK(strstr(mc, "\"transport\":\"reference\"") != NULL);
        free(mc);
    }
    char *sc = catalog_skills_json();
    CHECK(sc != NULL);
    if (sc) {
        /* 可运行技能 + 参考条目（fabric/skillhub/cursorrules） */
        CHECK(strstr(sc, "\"greet\"") != NULL && strstr(sc, "\"py_uuid\"") != NULL);
        CHECK(strstr(sc, "\"type\":\"skill\"") != NULL);
        CHECK(strstr(sc, "\"type\":\"reference\"") != NULL);
        CHECK(strstr(sc, "github.com/danielmiessler/fabric") != NULL);
        CHECK(strstr(sc, "github.com/anthropics/skills") != NULL);
        CHECK(strstr(sc, "github.com/PatrickJS/awesome-cursorrules") != NULL);
        CHECK(strstr(sc, "skillhub.cn") != NULL);
        /* regression: prompt-skill bodies contain newlines — all three catalog
         * JSON payloads must be fully parseable, not just substring-matchable */
        char *cats[3] = { catalog_models_json(), catalog_mcp_json(), sc };
        for (int ci = 0; ci < 3; ci++) {
            cJSON *parsed = cJSON_Parse(cats[ci]);
            CHECK(parsed != NULL && cJSON_IsArray(parsed));
            cJSON_Delete(parsed);
            free(cats[ci]);
        }
    }
    /* GitHub remote skills: JSON validity + lookup + fetch plumbing */
    char *rs = catalog_remote_skills_json();
    CHECK(rs != NULL);
    if (rs) {
        cJSON *arr = cJSON_Parse(rs);
        CHECK(arr != NULL && cJSON_IsArray(arr));
        CHECK(cJSON_GetArraySize(arr) >= 15);
        cJSON_Delete(arr);
        CHECK(strstr(rs, "\"fabric\"") != NULL && strstr(rs, "anthropics") != NULL &&
              strstr(rs, "cursorrules") != NULL);
        CHECK(strstr(rs, "ghproxy.net") != NULL);
        free(rs);
    }
    CHECK(catalog_remote_skill_find("fabric", "fab_summarize") != NULL);
    CHECK(catalog_remote_skill_find(NULL, "ant_pdf") != NULL);
    CHECK(catalog_remote_skill_find("fabric", "nope") == NULL);
    CHECK(catalog_remote_skill_find("skillhub", NULL) == NULL);
    /* 迭代器一致性 */
    CHECK(catalog_skill_count() >= 10);
    CHECK(catalog_skill_at(0) != NULL && catalog_skill_at(-1) == NULL &&
          catalog_skill_at(catalog_skill_count()) == NULL);
}

/* ---------- infra: install + execute EVERY curated skills-plaza entry ---------- */
static void test_catalog_skills_run(void) {
    section("catalog skills install + execute all");
    int py_ok = python_available();
    if (!py_ok) printf("  (python not available, python-kind skills skipped)\n");
    skill_registry *r = skill_registry_new();
    CHECK(r != NULL);
    if (!r) return;
    int n = catalog_skill_count();
    int ran = 0;
    for (int i = 0; i < n; i++) {
        const catalog_skill *cs = catalog_skill_at(i);
        CHECK(cs != NULL && cs->id && cs->name && cs->kind);
        if (!cs) continue;
        if (strcmp(cs->kind, "reference") == 0) {
            /* reference entries point at upstream repos/marketplaces, nothing to run */
            CHECK(cs->source && cs->source[0] == 'h' && strstr(cs->source, "://") != NULL);
            continue;
        }
        if (strcmp(cs->kind, "prompt") == 0) {
            /* prompt skills are LLM templates: validate shape, don't execute */
            CHECK(cs->body && cs->body[0]);
            CHECK(strstr(cs->body, "{{") != NULL);
            continue;
        }
        if (strcmp(cs->kind, "python") == 0 && !py_ok) continue;
        CHECK(cs->body && cs->body[0]);
        skill sk;
        memset(&sk, 0, sizeof(sk));
        sk.name = cs->id;  /* runtime name = ASCII id, same as /v1/skills/install */
        sk.description = cs->description;
        sk.kind = cs->kind;
        sk.body = cs->body;
        CHECK(skill_register_ex(r, &sk, 1) == 0);
        skill_result *res = skill_execute(
            r, cs->id, (cs->test_args && cs->test_args[0]) ? cs->test_args : NULL,
            NULL, 10000);
        CHECK(res != NULL);
        if (res) {
            CHECK(res->ok == 1);
            CHECK(res->output && res->output[0]);
            skill_result_free(res);
        }
        ran++;
    }
    printf("  catalog skills executed: %d (of %d entries)\n", ran, n);
    CHECK(ran == 2 + (py_ok ? 5 : 0));  /* 2 shell always + 5 python when available */
    skill_registry_free(r);
}

/* ---------- infra: audit JSONL ---------- */
static void test_audit(void) {
    section("audit");
    const char *path = "state-audit-test.jsonl";   /* flat file: audit_open is fopen(path,"a") */
    fs_remove(path);   /* remove stale file from a previous run */
    audit *a = audit_open(path);
    CHECK(a != NULL);
    if (!a) return;
    audit_log(a, "task.create", "task-1", "ok", "{\"prompt\":\"p\"}");
    audit_log(a, "tool.exec", "file_write", "ok", "a.txt");
    audit_close(a);
    /* reopen and verify JSONL lines were written */
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (f) {
        char buf[512];
        int ok1 = fgets(buf, sizeof buf, f) != NULL && strstr(buf, "task.create") && strstr(buf, "task-1");
        int ok2 = fgets(buf, sizeof buf, f) != NULL && strstr(buf, "tool.exec") && strstr(buf, "file_write");
        CHECK(ok1 && ok2);
        fclose(f);
    }
    fs_remove(path);
}

/* ---------- LLM adapters against the bundled HTTP server (no external net) ---------- */
static void th_serve_http(void *arg) { http_server_serve((http_server *)arg); }
static void th_serve_ctx(void *arg)  { serve((runtime_ctx *)arg); }

static int fake_openai_json(const http_request *req, http_response *resp, void *ud) {
    (void)req; (void)ud;
    http_resp_json(resp, "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello from openai-fake\"}}]}");
    return 0;
}
static int fake_anthropic_json(const http_request *req, http_response *resp, void *ud) {
    (void)req; (void)ud;
    http_resp_json(resp, "{\"content\":[{\"type\":\"text\",\"text\":\"hello from anthropic-fake\"}]}");
    return 0;
}
static int fake_openai_sse(const http_request *req, http_response *resp, void *ud) {
    (void)req; (void)ud;
    http_resp_json(resp,
        "data: {\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
        "data: [DONE]\n\n");
    return 0;
}
static int fake_anthropic_sse(const http_request *req, http_response *resp, void *ud) {
    (void)req; (void)ud;
    http_resp_json(resp,
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"text\":\"hi\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"text\":\" there\"}}\n\n"
        "data: [DONE]\n\n");
    return 0;
}
static void stream_accum(const char *delta, void *ud) {
    strbuf *sb = (strbuf *)ud;
    strbuf_append(sb, delta);
}

static void test_llm_adapters_http(void) {
    section("llm_adapters_http");
    http_server *json_srv = http_server_new_bind("127.0.0.1", 18212);
    http_server *sse_srv  = http_server_new_bind("127.0.0.1", 18213);
    CHECK(json_srv != NULL && sse_srv != NULL);
    if (!json_srv || !sse_srv) return;
    http_server_route(json_srv, "POST", "/v1/chat/completions", fake_openai_json, NULL);
    http_server_route(json_srv, "POST", "/v1/messages", fake_anthropic_json, NULL);
    http_server_route(sse_srv, "POST", "/v1/chat/completions", fake_openai_sse, NULL);
    http_server_route(sse_srv, "POST", "/v1/messages", fake_anthropic_sse, NULL);
    thread_t *tj = thread_create(th_serve_http, json_srv);
    thread_t *ts = thread_create(th_serve_http, sse_srv);
    time_sleep_ms(300);

    const llm_message msgs[1] = { { "user", "hi" } };
    llm_request q;
    memset(&q, 0, sizeof q);
    q.messages = msgs; q.num_messages = 1; q.max_tokens = 32;

    /* openai chat */
    llm *oai = llm_create("openai", "http://127.0.0.1:18212", "test-key", "m");
    CHECK(oai != NULL);
    if (oai) {
        llm_response out; memset(&out, 0, sizeof out);
        CHECK(llm_chat(oai, &q, &out) == 0);
        CHECK_STR(out.content, "hello from openai-fake");
        free(out.content); free(out.error);
        llm_destroy(oai);
    }
    /* openai stream */
    oai = llm_create("openai", "http://127.0.0.1:18213", "test-key", "m");
    CHECK(oai != NULL);
    if (oai) {
        strbuf sb; strbuf_init(&sb);
        CHECK(llm_stream(oai, &q, stream_accum, &sb) == 0);
        CHECK_STR(sb.buf, "hello");
        strbuf_free(&sb);
        llm_destroy(oai);
    }
    /* anthropic chat */
    llm *ant = llm_create("anthropic", "http://127.0.0.1:18212", "test-key", "m");
    CHECK(ant != NULL);
    if (ant) {
        llm_response out; memset(&out, 0, sizeof out);
        CHECK(llm_chat(ant, &q, &out) == 0);
        CHECK_STR(out.content, "hello from anthropic-fake");
        free(out.content); free(out.error);
        llm_destroy(ant);
    }
    /* anthropic stream */
    ant = llm_create("anthropic", "http://127.0.0.1:18213", "test-key", "m");
    CHECK(ant != NULL);
    if (ant) {
        strbuf sb; strbuf_init(&sb);
        CHECK(llm_stream(ant, &q, stream_accum, &sb) == 0);
        CHECK_STR(sb.buf, "hi there");
        strbuf_free(&sb);
        llm_destroy(ant);
    }

    http_server_stop(json_srv); http_server_stop(sse_srv);
    thread_join(tj); thread_join(ts);
    http_server_free(json_srv); http_server_free(sse_srv);
}

/* ---------- HTTP API over the live server (exercises http_server + api_rest + os_socket) ---------- */
/* Minimal HTTP/1.1 client over the raw socket primitives (avoids pulling in the
 * client-side http.h, whose http_response clashes with http_server.h's). */
typedef struct { int status; char body[65536]; size_t body_len; } raw_http;

static int raw_http_request(uint16_t port, const char *method, const char *path,
                            const char *body, raw_http *out) {
    sock *c = sock_connect("127.0.0.1", port, 3000);
    if (!c) return -1;
    char req[8192];
    int n = body
        ? snprintf(req, sizeof req,
                   "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nContent-Type: application/json\r\n"
                   "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                   method, path, port, strlen(body), body)
        : snprintf(req, sizeof req,
                   "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nConnection: close\r\n\r\n",
                   method, path, port);
    int sent = 0;
    while (sent < n) {
        int w = sock_send(c, req + sent, (size_t)(n - sent));
        if (w <= 0) { sock_close(c); return -1; }
        sent += w;
    }
    char hdr[2048]; size_t hn = 0;
    while (hn < sizeof hdr - 1) {
        int rr = sock_recv(c, hdr + hn, 1);
        if (rr <= 0) break;
        hn++; hdr[hn] = '\0';
        if (hn >= 4 && memcmp(hdr + hn - 4, "\r\n\r\n", 4) == 0) break;
    }
    int status = 0;
    sscanf(hdr, "HTTP/1.1 %d", &status);
    size_t bl = 0;
    while (bl < sizeof out->body - 1) {
        int rr = sock_recv(c, out->body + bl, sizeof out->body - 1 - bl);
        if (rr <= 0) break;
        bl += (size_t)rr;
    }
    out->body[bl] = '\0';
    out->body_len = bl;
    out->status = status;
    sock_close(c);
    return 0;
}

static void test_http_api(void) {
    section("http_api");
    const char *root = "state-http-test";
    fs_remove(root);
    config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.state_root = root;
    cfg.workspace = ".";
    cfg.provider = "mock";
    cfg.http_port = 18211;
    cfg.workers = 2;
    runtime_ctx ctx;
    if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
    thread_t *th = thread_create(th_serve_ctx, &ctx);
    time_sleep_ms(400);

    raw_http r;
    CHECK(raw_http_request(18211, "GET", "/v1/tools", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "file_write") != NULL);

    CHECK(raw_http_request(18211, "GET", "/v1/catalog/models", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "groq") != NULL);

    CHECK(raw_http_request(18211, "GET", "/v1/catalog/mcp", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "github") != NULL);
    CHECK(strstr(r.body, "github.com/modelcontextprotocol/servers") != NULL);
    CHECK(strstr(r.body, "\"browserbase\"") != NULL);
    CHECK(strstr(r.body, "\"transport\":\"reference\"") != NULL);

    /* MCP one-shot test endpoint: handshake + tools/list without registering */
    if (node_available()) {
        CHECK(raw_http_request(18211, "POST", "/v1/mcp/test",
                               "{\"transport\":\"stdio\",\"command\":\"node\","
                               "\"args\":\"tools/mock_mcp_server.js --stdio\"}",
                               &r) == 0 && r.status == 200);
        CHECK(strstr(r.body, "\"ok\":true") != NULL &&
              strstr(r.body, "\"echo\"") != NULL);
    }
    CHECK(raw_http_request(18211, "POST", "/v1/mcp/test",
                           "{\"transport\":\"stdio\",\"command\":\"definitely-not-a-real-cmd\"}",
                           &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"ok\":false") != NULL);
    CHECK(raw_http_request(18211, "POST", "/v1/mcp/test", "{}", &r) == 0 && r.status == 400);

    /* skills plaza: catalog listing + one-click install + run */
    CHECK(raw_http_request(18211, "GET", "/v1/catalog/skills", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"greet\"") != NULL);
    CHECK(strstr(r.body, "github.com/danielmiessler/fabric") != NULL);
    CHECK(raw_http_request(18211, "POST", "/v1/skills/install",
                           "{\"id\":\"no-such-id\"}", &r) == 0 && r.status == 404);
    CHECK(raw_http_request(18211, "POST", "/v1/skills/install",
                           "{\"id\":\"fabric\"}", &r) == 0 && r.status == 400);
    CHECK(raw_http_request(18211, "POST", "/v1/skills/install",
                           "{\"id\":\"greet\"}", &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"ok\":true") != NULL);
    CHECK(raw_http_request(18211, "POST", "/v1/skills/run",
                           "{\"name\":\"greet\",\"args\":\"{\\\"who\\\":\\\"plaza\\\"}\"}",
                           &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"ok\":true") != NULL && strstr(r.body, "plaza") != NULL);

    /* prompt-kind skills run through the LLM backend (mock provider at 18211) */
    CHECK(raw_http_request(18211, "POST", "/v1/skills/install",
                           "{\"id\":\"summarize\"}", &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"ok\":true") != NULL);
    CHECK(raw_http_request(18211, "POST", "/v1/skills/run",
                           "{\"name\":\"summarize\",\"args\":\"{\\\"content\\\":\\\"hello world\\\"}\"}",
                           &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"ok\":true") != NULL &&
          strstr(r.body, "\"output\"") != NULL);

    /* GitHub remote skills: catalog + validation paths (real fetch is
     * network-dependent: assert 200 or 502, log which, no hard CHECK) */
    CHECK(raw_http_request(18211, "GET", "/v1/catalog/github-skills", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "fab_summarize") != NULL);
    CHECK(raw_http_request(18211, "POST", "/v1/skills/install-remote", "{}", &r) == 0 && r.status == 400);
    CHECK(raw_http_request(18211, "POST", "/v1/skills/install-remote",
                           "{\"repo\":\"fabric\",\"id\":\"nope\"}", &r) == 0 && r.status == 404);
    int fetched = 0;
    if (raw_http_request(18211, "POST", "/v1/skills/install-remote",
                         "{\"repo\":\"fabric\",\"id\":\"fab_summarize\"}", &r) == 0 &&
        (r.status == 200 || r.status == 502)) {
        if (r.status == 200 && strstr(r.body, "\"ok\":true") != NULL) fetched = 1;
        /* fetch failed (offline/blocked): 502, acceptable in this suite */
    } else {
        CHECK(0); /* unexpected status/transport error */
    }
    printf("  remote skill fetch: %s\n", fetched ? "ok (installed from GitHub)"
                                               : "skipped (network unavailable)");

    CHECK(raw_http_request(18211, "GET", "/v1/skills/market", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "templates") != NULL);
    /* GitHub 热门应用 section */
    CHECK(strstr(r.body, "\"github\"") != NULL);
    CHECK(strstr(r.body, "https://github.com/jqlang/jq") != NULL);
    CHECK(strstr(r.body, "https://github.com/yt-dlp/yt-dlp") != NULL);

    CHECK(raw_http_request(18211, "GET", "/v1/plugins/market", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "templates") != NULL);
    /* GitHub 热门插件 section */
    CHECK(strstr(r.body, "\"github\"") != NULL);
    CHECK(strstr(r.body, "https://github.com/koalaman/shellcheck") != NULL);
    CHECK(strstr(r.body, "https://github.com/gitleaks/gitleaks") != NULL);

    CHECK(raw_http_request(18211, "GET", "/v1/config/llm", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "mock") != NULL);

    CHECK(raw_http_request(18211, "GET", "/v1/im/channels", NULL, &r) == 0 && r.status == 200);
    CHECK(raw_http_request(18211, "GET", "/v1/memory", NULL, &r) == 0 && r.status == 200);
    CHECK(raw_http_request(18211, "GET", "/v1/blackboard", NULL, &r) == 0 && r.status == 200);
    CHECK(raw_http_request(18211, "GET", "/v1/agents", NULL, &r) == 0 && r.status == 200);
    CHECK(raw_http_request(18211, "GET", "/v1/snapshots", NULL, &r) == 0 && r.status == 200);
    CHECK(raw_http_request(18211, "GET", "/metrics", NULL, &r) == 0 && r.status == 200);
    CHECK(raw_http_request(18211, "GET", "/v1/routes", NULL, &r) == 0 && r.status == 200);
    CHECK(raw_http_request(18211, "GET", "/v1/usage", NULL, &r) == 0 && r.status == 200);

    /* POST a task -> runs the full reasoning pipeline via the mock provider */
    CHECK(raw_http_request(18211, "POST", "/v1/tasks",
                           "{\"prompt\":\"创建 a.txt 写入内容为 hello\"}", &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"id\"") != NULL);
    {
        cJSON *j = cJSON_Parse(r.body);
        int64_t id = -1;
        if (j) {
            cJSON *idj = cJSON_GetObjectItemCaseSensitive(j, "id");
            if (idj && cJSON_IsNumber(idj)) id = (int64_t)idj->valuedouble;
            cJSON_Delete(j);
        }
        if (id >= 0) {
            int finished = 0;
            for (int i = 0; i < 60 && !finished; i++) {
                char path[128];
                snprintf(path, sizeof path, "/v1/tasks/%lld", (long long)id);
                CHECK(raw_http_request(18211, "GET", path, NULL, &r) == 0 && r.status == 200);
                cJSON *tj = cJSON_Parse(r.body);
                const char *st = tj ? cJSON_GetObjectItemCaseSensitive(tj, "status")->valuestring : "?";
                if (st && (strcmp(st, "DONE") == 0 || strcmp(st, "FAILED") == 0)) {
                    CHECK_STR(st, "DONE");
                    const char *out = tj ? cJSON_GetObjectItemCaseSensitive(tj, "output")->valuestring : NULL;
                    CHECK(out != NULL && out[0]);
                    finished = 1;
                }
                if (tj) cJSON_Delete(tj);
                if (!finished) time_sleep_ms(100);
            }
            /* mock pipeline should have created a.txt with the expected content */
            FILE *af = fopen("a.txt", "r");
            CHECK(af != NULL);
            if (af) {
                char b[64]; size_t bn = fread(b, 1, sizeof b - 1, af); b[bn] = '\0';
                CHECK(strstr(b, "hello") != NULL);
                fclose(af);
            }
        }
    }

    /* error paths */
    CHECK(raw_http_request(18211, "POST", "/v1/tasks", "{}", &r) == 0 && r.status == 400);
    CHECK(raw_http_request(18211, "GET", "/v1/nope", NULL, &r) == 0 && r.status == 404);

    stop(&ctx);
    thread_join(th);
    runtime_shutdown(&ctx);
    fs_remove("a.txt");
    fs_remove(root);
}

/* ---------- WebSocket server round-trip (ws_server hub) ---------- */
static char g_ws_recv[256];
static void ws_on_msg(const char *text, void *ud) {
    (void)ud;
    snprintf(g_ws_recv, sizeof g_ws_recv, "%s", text);
}

static void test_ws_roundtrip(void) {
    section("ws_roundtrip");
    http_server *s = http_server_new_bind("127.0.0.1", 18214);
    CHECK(s != NULL);
    if (!s) return;
    http_server_ws_route(s, "/ws", ws_on_msg, NULL);
    thread_t *ts = thread_create(th_serve_http, s);
    time_sleep_ms(300);

    g_ws_recv[0] = '\0';
    sock *c = sock_connect("127.0.0.1", 18214, 3000);
    CHECK(c != NULL);
    if (c) {
        /* handshake */
        char hs[512];
        int n = snprintf(hs, sizeof hs,
            "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:18214\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n");
        CHECK(sock_send(c, hs, (size_t)n) == n);
        char resp[1024]; size_t rn = 0; int safe = 0;
        while (rn < sizeof resp - 1 && safe++ < 8) {
            if (sock_wait_readable(c, 1000) <= 0) break;
            int rr = sock_recv(c, resp + rn, sizeof resp - 1 - rn);
            if (rr <= 0) break;
            rn += (size_t)rr; resp[rn] = '\0';
            if (strstr(resp, "\r\n\r\n")) break;
        }
        CHECK(rn > 0);
        if (rn > 0) {
            CHECK(strstr(resp, "101") != NULL);
            CHECK(strstr(resp, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
        }
        /* send a masked text frame */
        size_t flen = 0;
        char *f = ws_build_frame(WS_OP_TEXT, (const unsigned char *)"hello", 5, 1, &flen);
        CHECK(f != NULL);
        if (f) {
            CHECK(sock_send(c, f, flen) == (int)flen);
            free(f);
        }
        for (int i = 0; i < 20 && g_ws_recv[0] == '\0'; i++) time_sleep_ms(100);
        CHECK_STR(g_ws_recv, "hello");

        /* server broadcast -> client receives a text frame */
        http_server_ws_broadcast(s, "{\"x\":1}");
        unsigned char buf[512]; int got = 0;
        for (int i = 0; i < 30; i++) {
            if (sock_wait_readable(c, 200) > 0) {
                int nn = sock_recv(c, buf, sizeof buf);
                if (nn > 0) { got = nn; break; }
            }
        }
        CHECK(got > 0);
        if (got > 0) {
            unsigned char pay[256]; size_t plen = 0; int op = 0, fin = 0;
            CHECK(ws_parse_frame(buf, (size_t)got, pay, &plen, &op, &fin) == 0);
            CHECK(fin == 1 && op == WS_OP_TEXT && plen == 7);
            CHECK(plen == 7 && memcmp(pay, "{\"x\":1}", 7) == 0);
        }
        sock_close(c);
    }
    http_server_stop(s);
    thread_join(ts);
    http_server_free(s);
}

/* ---------- networked marketplace (merge remote catalog + best-effort publish) ---------- */
static int g_market_push = 0;
static int fake_market_ping(const http_request *req, http_response *resp, void *ud) {
    (void)req; (void)ud;
    http_resp_json(resp, "{\"ok\":true}");
    return 0;
}
static int fake_market_skills(const http_request *req, http_response *resp, void *ud) {
    (void)req; (void)ud;
    http_resp_json(resp,
        "{\"templates\":[{\"name\":\"remote-skill-a\",\"description\":\"来自远端市场\","
        "\"kind\":\"shell\",\"body\":\"echo remote\"}],"
        "\"github\":[{\"name\":\"remote-gh-tool\",\"repo\":\"https://github.com/example/remote-tool\"}]}");
    return 0;
}
static int fake_market_plugins(const http_request *req, http_response *resp, void *ud) {
    (void)req; (void)ud;
    http_resp_json(resp,
        "{\"templates\":[{\"name\":\"remote-plugin-a\",\"description\":\"来自远端插件市场\"}],\"github\":[]}");
    return 0;
}
static int fake_market_skills_publish(const http_request *req, http_response *resp, void *ud) {
    (void)req; (void)ud;
    g_market_push++;
    http_resp_json(resp, "{\"ok\":true}");
    return 0;
}
static int fake_market_plugins_publish(const http_request *req, http_response *resp, void *ud) {
    (void)req; (void)ud;
    g_market_push++;
    http_resp_json(resp, "{\"ok\":true}");
    return 0;
}

static void test_market_remote(void) {
    section("market_remote");
    const char *root = "state-market-test";
    {
        char p[600];
        snprintf(p, sizeof p, "%s/skills.json", root); fs_remove(p);
        snprintf(p, sizeof p, "%s/plugins.json", root); fs_remove(p);
        snprintf(p, sizeof p, "%s/cognitive-os-agent.json", root); fs_remove(p);
    }
    fs_remove(root);
    http_server *mkt = http_server_new_bind("127.0.0.1", 18216);
    CHECK(mkt != NULL);
    if (!mkt) return;
    http_server_route(mkt, "GET", "/v1/market/ping", fake_market_ping, NULL);
    http_server_route(mkt, "GET", "/v1/skills/market", fake_market_skills, NULL);
    http_server_route(mkt, "GET", "/v1/plugins/market", fake_market_plugins, NULL);
    http_server_route(mkt, "POST", "/v1/skills/publish", fake_market_skills_publish, NULL);
    http_server_route(mkt, "POST", "/v1/plugins/publish", fake_market_plugins_publish, NULL);
    thread_t *tm = thread_create(th_serve_http, mkt);
    time_sleep_ms(300);

    config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.state_root = root;
    cfg.workspace = ".";
    cfg.provider = "mock";
    cfg.market_url = "http://127.0.0.1:18216";
    cfg.http_port = 18217;
    cfg.workers = 2;
    runtime_ctx ctx;
    if (init(&ctx, &cfg) != 0) {
        CHECK(0);
        http_server_stop(mkt);
        thread_join(tm);
        http_server_free(mkt);
        return;
    }
    thread_t *th = thread_create(th_serve_ctx, &ctx);
    time_sleep_ms(400);

    raw_http r;
    g_market_push = 0;

    /* market/status reports configuration + reachability */
    CHECK(raw_http_request(18217, "GET", "/v1/market/status", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"configured\":true") != NULL);
    CHECK(strstr(r.body, "\"online\":true") != NULL);
    CHECK(strstr(r.body, "18216") != NULL);

    /* skills market merges the remote catalog, tagged source=remote */
    CHECK(raw_http_request(18217, "GET", "/v1/skills/market", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"market_online\":true") != NULL);
    CHECK(strstr(r.body, "remote-skill-a") != NULL);
    CHECK(strstr(r.body, "\"source\":\"remote\"") != NULL);
    CHECK(strstr(r.body, "remote-gh-tool") != NULL);

    /* plugins market merges the remote catalog too */
    CHECK(raw_http_request(18217, "GET", "/v1/plugins/market", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"market_online\":true") != NULL);
    CHECK(strstr(r.body, "remote-plugin-a") != NULL);
    CHECK(strstr(r.body, "\"source\":\"remote\"") != NULL);

    /* publish pushes to the networked market (best-effort) */
    CHECK(raw_http_request(18217, "POST", "/v1/skills/publish",
        "{\"name\":\"pubskill-x\",\"kind\":\"shell\",\"description\":\"t\",\"body\":\"echo hi\"}",
        &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"pushed_to_market\":true") != NULL);
    CHECK(g_market_push == 1);

    CHECK(raw_http_request(18217, "POST", "/v1/plugins/publish",
        "{\"name\":\"pubplugin-x\",\"kind\":\"shell\",\"description\":\"t\",\"body\":\"echo hi\"}",
        &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"pushed_to_market\":true") != NULL);
    CHECK(g_market_push == 2);

    stop(&ctx);
    thread_join(th);
    runtime_shutdown(&ctx);
    http_server_stop(mkt);
    thread_join(tm);
    http_server_free(mkt);
    /* fs_remove only unlinks files on Windows; remove the persisted state
     * files explicitly so a second run doesn't reload stale registry entries. */
    {
        char p[600];
        snprintf(p, sizeof p, "%s/skills.json", root); fs_remove(p);
        snprintf(p, sizeof p, "%s/plugins.json", root); fs_remove(p);
        snprintf(p, sizeof p, "%s/cognitive-os-agent.json", root); fs_remove(p);
    }
    fs_remove(root);
}

/* ---------- local model runtimes (free, no key): status + start ---------- */
static void test_local_model(void) {
    section("local_model");
    const char *root = "state-local-test";
    fs_remove(root);
    config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.state_root = root;
    cfg.workspace = ".";
    cfg.provider = "mock";
    cfg.http_port = 18218;
    cfg.workers = 2;
    runtime_ctx ctx;
    if (init(&ctx, &cfg) != 0) { CHECK(0); return; }
    thread_t *th = thread_create(th_serve_ctx, &ctx);
    time_sleep_ms(400);

    raw_http r;
    /* status: probes both engines, no side effects */
    CHECK(raw_http_request(18218, "GET", "/v1/local/status", NULL, &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"ollama\"") != NULL);
    CHECK(strstr(r.body, "\"llamacpp\"") != NULL);
    CHECK(strstr(r.body, "\"running\"") != NULL);

    /* unknown engine -> structured error, no spawn */
    CHECK(raw_http_request(18218, "POST", "/v1/local/start",
        "{\"engine\":\"nope\"}", &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"ok\":false") != NULL);
    CHECK(strstr(r.body, "unknown") != NULL);

    /* llamacpp without a configured start command -> deterministic error */
    CHECK(raw_http_request(18218, "POST", "/v1/local/start",
        "{\"engine\":\"llamacpp\"}", &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "\"ok\":false") != NULL);
    CHECK(strstr(r.body, "llamacpp_cmd") != NULL);

    stop(&ctx);
    thread_join(th);
    runtime_shutdown(&ctx);
    fs_remove(root);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered: survive crashes mid-run */
    printf("cognitive-os-agent unit tests\n");
    test_util();
    test_event_bus();
    test_ringbuf();
    test_ringbuf_mpmc();
    test_embedding();
    test_scheduler();
    test_coro();
    test_scheduler_mn();
    test_state_machine();
    test_policy();
    test_memory();
    test_snapshot_tx();
    test_llm_mock();
    test_llm_caps_cancel();
    test_retrieval_upgrade();
    test_state_store();
    test_memory_service();
    test_state_snapshot();
    test_executor_family();
    test_index();
    test_metrics();
    test_config();
    test_blackboard();
    test_agent_pool();
    test_auth();
    test_websocket();
    test_plugin_loader();
    test_kv();
    test_episodic();
    test_vector();
    test_graph();
    test_context_builder();
    test_memory_persist();
    test_context_caps();
    test_session_memory();
    test_planner();
    test_evaluator();
    test_sandbox();
    test_filetracker();
    test_capability();
    test_plugin_intelligence();
    test_trace();
    test_router();
    test_usage();
    test_registry();
    test_skills();
    test_skill_args();
    test_caps_gate();
    test_generated_tool();
    test_memory_graph();
    test_edit_search();
    test_snapshot_bigfile();
    test_mcp();
    test_tool_schema();
    test_tool_truncate();
    test_mcp_standard();
    test_mcp_stdio();
    test_cluster();
    test_hook();
    test_router_policy();
    test_consolidation();
    test_memory_lifecycle();
    test_context_budget();
    test_attention();
    test_sandbox_wasm();
    test_agent_loop(); /* full-runtime init: keep after the sandbox seam test */
    test_chat_upload_evolve();
    test_orchestrate();
    test_flow();
    test_flow_decompose();
    test_policy_rules();
    test_im();
    test_im_search();
    test_im_bridge();
    test_plugin_generate();
    test_task();
    test_catalog();
    test_catalog_skills_run();
    test_audit();
    test_llm_adapters_http();
    test_http_api();
    test_ws_roundtrip();
    test_market_remote();
    test_local_model();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
