/* test_all.c — unit tests for c-agent modules.
 * Build with: zig cc -Iinclude -Ithird_party/cJSON $(find src third_party -name "*.c")
 * or via build.sh test. Exit 0 = all green, non-zero = failure. */
#include "cagent/runtime/event_bus.h"
#include "cagent/runtime/scheduler.h"
#include "cagent/runtime/state_machine.h"
#include "cagent/runtime/policy_engine.h"
#include "cagent/memory/memory.h"
#include "cagent/snapshot/snapshot.h"
#include "cagent/action/tools.h"
#include "cagent/tx/tx.h"
#include "cagent/llm/llm.h"
#include "cagent/retrieval/engine.h"
#include "cagent/cognition/blackboard.h"
#include "cagent/runtime/agent.h"
#include "cagent/api/auth.h"
#include "cagent/api/websocket.h"
#include "cagent/plugin_runtime/manager.h"
#include "cagent/infra/metrics.h"
#include "cagent/infra/util.h"
#include "cagent/infra/config.h"
#include "cagent/os/os_fs.h"
#include "cagent/os/os_coro.h"
#include "cagent/memory/kv.h"
#include "cagent/memory/episode.h"
#include "cagent/memory/vector.h"
#include "cagent/memory/graph.h"
#include "cagent/retrieval/context_builder.h"
#include "cagent/cognition/planner.h"
#include "cagent/cognition/evaluator.h"
#include "cagent/plugin_runtime/sandbox.h"
#include "cagent/plugin_runtime/capability.h"
#include "cagent/plugin_runtime/registry.h"
#include "cagent/plugin_intelligence/analyzer.h"
#include "cagent/plugin_intelligence/architect.h"
#include "cagent/plugin_intelligence/codegen.h"
#include "cagent/plugin_intelligence/testing.h"
#include "cagent/plugin_intelligence/security.h"
#include "cagent/action/skill.h"
#include "cagent/action/mcp_conn.h"
#include "cagent/cluster/node.h"
#include "cagent/cognition/attention.h"
#include "cagent/infra/trace.h"
#include "cagent/llm/router.h"
#include "cagent/llm/usage.h"
#include "cagent/runtime/task.h"
#include "cagent/infra/ringbuf.h"
#include "cagent/retrieval/embedding.h"
#include "cagent/im/im.h"
#include "cagent/plugin_intelligence/generator.h"
#include "cagent/plugin_runtime/wasm_runner.h"
#include "cagent/cagent.h"
#include "cagent/os/os_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
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
    ca_path_join(out, sizeof(out), "a", "b.txt");
#if defined(_WIN32)
    CHECK_STR(out, "a\\b.txt");
#else
    CHECK_STR(out, "a/b.txt");
#endif
    /* alias-safe: out may be the same buffer as the input base */
    char p[256];
    snprintf(p, sizeof(p), "x/y");
    ca_path_join(p, sizeof(p), p, "z.txt");
    CHECK(strstr(p, "z.txt") != NULL);

    /* resolve against a workspace */
    char r[256];
    ca_path_resolve(r, sizeof(r), "w", "sub/f.txt");
    CHECK(strstr(r, "sub/f.txt") != NULL);
    ca_path_resolve(r, sizeof(r), "w", "/abs/path");
    CHECK_STR(r, "/abs/path");

    CHECK(ca_hash64("hello", 5) != 0);
    char hex[17];
    ca_hash_hex(hex, 0xDEADBEEFDEADBEEFULL);
    CHECK(strlen(hex) == 16);

    ca_strbuf b;
    ca_strbuf_init(&b);
    ca_strbuf_append(&b, "a");
    ca_strbuf_appendf(&b, "-%d", 42);
    CHECK_STR(b.buf, "a-42");
    char *det = ca_strbuf_detach(&b);
    CHECK_STR(det, "a-42");
    free(det);

    ca_strmap m;
    memset(&m, 0, sizeof(m));
    ca_strmap_set(&m, "k", "v1");
    ca_strmap_set(&m, "k", "v2");
    CHECK_STR(ca_strmap_get(&m, "k"), "v2");
    CHECK(ca_strmap_get(&m, "missing") == NULL);
    ca_strmap_free(&m);
}

/* ---------- core: event bus ---------- */
static int ev_count = 0;
static int ev_types[16];
static void on_ev(const ca_event *ev, void *ud) {
    (void)ud;
    if (ev_count < 16) ev_types[ev_count] = (int)ev->type;
    ev_count++;
}

static void test_event_bus(void) {
    section("event_bus");
    ca_event_bus *b = ca_event_bus_new();
    CHECK(b != NULL);
    ca_event_bus_subscribe(b, -1, on_ev, NULL);
    ca_event_bus_publish_json(b, CA_EV_TOOL, "test", "{\"tool\":\"file_read\"}");
    ca_event_bus_publish_json(b, CA_EV_MEMORY, "test", "{\"k\":\"v\"}");
    CHECK(ev_count == 2);
    CHECK(ev_types[0] == CA_EV_TOOL);
    CHECK(ev_types[1] == CA_EV_MEMORY);
    ca_event_bus_free(b);
}

/* ---------- core: scheduler ---------- */
static void run_fast(ca_task *t, ca_scheduler *s, void *ud) {
    (void)s; (void)ud;
    t->output = ca_strdup("ran");
    t->status = CA_TS_DONE;
}

static void test_scheduler(void) {
    section("scheduler");
    ca_scheduler *s = ca_scheduler_new(2, run_fast, NULL);
    int64_t id = ca_scheduler_submit(s, 0, "job", NULL, 0);
    CHECK(id >= 0);
    CHECK(ca_scheduler_wait_idle(s, 3000) == 0);
    ca_task *t = ca_scheduler_get(s, id);
    CHECK(t != NULL);
    CHECK(t->status == CA_TS_DONE);
    if (t) {
        CHECK_STR(t->output, "ran");
        free(t->output);
        t->output = NULL;
    }
    CHECK(ca_scheduler_total(s) == 1);
    CHECK(ca_scheduler_shutdown(s, 3000) == 0);
    ca_scheduler_free(s);
}

/* ---------- os: stackful coroutine ---------- */
static int coro_steps[8];
static int coro_step_count = 0;

static void coro_body(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) {
        coro_steps[coro_step_count++] = i;
        ca_coro_yield();
    }
    coro_steps[coro_step_count++] = 99; /* terminal marker */
}

static void test_coro(void) {
    section("coro");
    coro_step_count = 0;
    ca_coro *c = ca_coro_new(coro_body, NULL, 0);
    CHECK(c != NULL);
    if (!c) return;
    CHECK(ca_coro_done(c) == 0);

    ca_coro_resume(c);              /* run until first yield: steps[0]=0 */
    CHECK(ca_coro_done(c) == 0);
    CHECK(coro_step_count == 1);
    CHECK(coro_steps[0] == 0);

    ca_coro_resume(c);              /* steps[1]=1 */
    CHECK(coro_step_count == 2);
    CHECK(coro_steps[1] == 1);

    ca_coro_resume(c);              /* steps[2]=2 */
    CHECK(coro_step_count == 3);
    CHECK(coro_steps[2] == 2);

    ca_coro_resume(c);              /* finish: steps[3]=99, done=1 */
    CHECK(ca_coro_done(c) == 1);
    CHECK(coro_step_count == 4);
    CHECK(coro_steps[3] == 99);

    ca_coro_free(c);
}

/* ---------- core: M:N scheduler (M coroutine tasks on N threads) ---------- */
#define MN_COUNT 40
static int mn_runs[MN_COUNT];

static void mn_runner(ca_task *t, ca_scheduler *s, void *ud) {
    (void)s; (void)ud;
    int idx = (int)(intptr_t)t->userdata;
    mn_runs[idx]++;                 /* must run exactly once (yield resumes, not restarts) */
    ca_scheduler_yield();           /* cooperative yield mid-task */
    char buf[32];
    snprintf(buf, sizeof(buf), "task-%d", idx);
    t->output = ca_strdup(buf);
    t->status = CA_TS_DONE;
}

static void test_scheduler_mn(void) {
    section("scheduler_mn");
    memset(mn_runs, 0, sizeof(mn_runs));
    ca_scheduler *s = ca_scheduler_new(2, mn_runner, NULL);
    for (int i = 0; i < MN_COUNT; i++)
        CHECK(ca_scheduler_submit(s, 0, "t", (void *)(intptr_t)i, 0) >= 0);
    CHECK(ca_scheduler_wait_idle(s, 5000) == 0);
    for (int i = 0; i < MN_COUNT; i++) {
        ca_task *t = ca_scheduler_get(s, i);
        CHECK(t != NULL);
        if (t) {
            CHECK(t->status == CA_TS_DONE);
            char buf[32];
            snprintf(buf, sizeof(buf), "task-%d", i);
            CHECK_STR(t->output, buf);
            free(t->output);
            t->output = NULL;
        }
        CHECK(mn_runs[i] == 1);
    }
    CHECK(ca_scheduler_total(s) == MN_COUNT);
    CHECK(ca_scheduler_shutdown(s, 5000) == 0);
    ca_scheduler_free(s);
}

/* ---------- core: state machine ---------- */
static int h_reason(ca_state_machine *sm, void *ud, const char *in, char **out) {
    (void)sm; (void)ud;
    *out = ca_strdup(in);      /* pass through */
    return 0;
}
static int h_fail(ca_state_machine *sm, void *ud, const char *in, char **out) {
    (void)sm; (void)ud; (void)in; (void)out;
    return -1;                  /* force FAILED */
}

static void test_state_machine(void) {
    section("state_machine");
    ca_state_machine *sm = ca_state_machine_new();
    ca_state_machine_set_handler(sm, CA_ST_REASON, h_reason, NULL);
    char *res = NULL;
    ca_state fin = ca_state_machine_run(sm, "input", &res);
    CHECK(fin == CA_ST_DONE);
    CHECK_STR(res, "input");
    free(res);

    ca_state_machine *sm2 = ca_state_machine_new();
    ca_state_machine_set_handler(sm2, CA_ST_ACT, h_fail, NULL);
    char *res2 = NULL;
    ca_state fin2 = ca_state_machine_run(sm2, "x", &res2);
    CHECK(fin2 == CA_ST_FAILED);
    ca_state_machine_free(sm2);
    ca_state_machine_free(sm);
}

/* ---------- core: policy engine ---------- */
static void test_policy(void) {
    section("policy");
    ca_policy_engine *pe = ca_policy_engine_new();
    ca_policy_add_rule(pe, "*", "allow", "default allow");
    CHECK(ca_policy_check(pe, "shell", "{}", NULL) == CA_POLICY_ALLOW);

    ca_policy_engine *pe2 = ca_policy_engine_new();
    ca_policy_add_rule(pe2, "shell", "deny", "no shell");
    CHECK(ca_policy_check(pe2, "shell", "{}", NULL) == CA_POLICY_DENY);
    /* unmatched tool with rules present defaults to ASK */
    CHECK(ca_policy_check(pe2, "file_read", "{}", NULL) == CA_POLICY_ASK);
    ca_policy_engine_free(pe2);

    int risk = ca_policy_risk("shell", "{\"command\":\"rm -rf /\"}");
    CHECK(risk > 0 && risk <= 100);
    CHECK(ca_policy_risk("file_read", "{\"path\":\"a.txt\"}") < risk);
    ca_policy_engine_free(pe);
}

/* ---------- service: memory ---------- */
static void test_memory(void) {
    section("memory");
    const char *root = "state-test/memory";
    ca_memory *m = ca_memory_new(root);
    CHECK(m != NULL);
    ca_memory_working_push(m, "item one");
    ca_memory_working_push(m, "item two");
    ca_memory_remember(m, "lang", "c");
    ca_memory_remember(m, "x", "y");
    ca_memory_remember(m, "x", NULL);        /* delete */
    CHECK_STR(ca_memory_recall(m, "lang"), "c");
    CHECK(ca_memory_recall(m, "x") == NULL);
    ca_memory_record_experience(m, "task", "result");
    ca_memory_flush(m);
    /* facade accessors over the sub-stores */
    CHECK(ca_memory_working_count(m) == 2);
    CHECK_STR(ca_memory_working_at(m, 0), "item two");
    CHECK_STR(ca_memory_working_at(m, 1), "item one");
    char *ep = ca_memory_episodes_json(m);
    CHECK(ep && strstr(ep, "task") != NULL && strstr(ep, "result") != NULL);
    free(ep);
    char *retr = ca_memory_retrieve(m, "item", 3);
    CHECK(retr != NULL);
    free(retr);
    char *w = ca_memory_working_json(m);
    CHECK(w && strstr(w, "item one") != NULL);
    free(w);
    char *l = ca_memory_longterm_json(m);
    CHECK(l && strstr(l, "lang") != NULL);
    free(l);
    char *sr = ca_memory_search(m, "item", 5);
    CHECK(sr != NULL);
    free(sr);
    ca_memory_free(m);
}

/* ---------- snapshot + tx + tools ---------- */
static void test_snapshot_tx(void) {
    section("snapshot+tx");
    /* stale state from a previous run (committed file) would be pre-captured as
     * existing and rollback would restore instead of delete — clean it first. */
    ca_fs_remove("state-test/w/f.txt");
    const char *root = "state-test/snapshot";
    ca_snapshot *snap = ca_snapshot_open(root);
    CHECK(snap != NULL);

    ca_tool_registry *reg = ca_tool_registry_new();
    ca_tool_register_builtins(reg);
    CHECK(ca_tool_registry_count(reg) == 5);
    CHECK(ca_tool_find(reg, "file_read") != NULL);

    ca_tx_manager *tm = ca_tx_manager_new();
    ca_tool_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.reg = reg;
    ctx.snapshot = snap;
    ctx.workspace = "state-test/w";
    ca_fs_mkdirs("state-test/w");
    ca_tx *tx = ca_tx_begin(tm, snap, reg, &ctx);

    /* write a file inside the tx (workspace-relative path resolves to
     * state-test/w/f.txt, which the tool writes AND tx pre-captures) */
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"f.txt\",\"content\":\"hello\"}");
    int rc = ca_tx_run(tx, "file_write", args);
    CHECK(rc == 0);
    CHECK(ca_tx_validate(tx) == 1);

    /* verify the file exists */
    char *data = ca_fs_read_file("state-test/w/f.txt");
    CHECK(data != NULL);
    CHECK_STR(data, "hello");
    free(data);

    /* rollback must delete it (captured as "to be created") */
    CHECK(ca_tx_rollback(tx) == 0);
    CHECK(ca_fs_exists("state-test/w/f.txt") == 0);

    /* commit path: write again, commit, verify persisted */
    ca_tx *tx2 = ca_tx_begin(tm, snap, reg, &ctx);
    rc = ca_tx_run(tx2, "file_write", args);
    CHECK(rc == 0);
    CHECK(ca_tx_commit(tx2) == 0);
    CHECK(ca_fs_read_file("state-test/w/f.txt") != NULL);
    char *data2 = ca_fs_read_file("state-test/w/f.txt");
    CHECK_STR(data2, "hello");
    free(data2);

    char *list = ca_snapshot_list(snap);
    CHECK(list && strstr(list, "f.txt") != NULL);
    free(list);

    ca_tx_free(tx2);
    ca_tx_free(tx);
    ca_tx_manager_free(tm);
    ca_tool_registry_free(reg);
    ca_snapshot_close(snap);
}

/* ---------- llm: mock provider ---------- */
static void test_llm_mock(void) {
    section("llm_mock");
    ca_llm *llm = ca_llm_create("mock", NULL, NULL, "mock");
    CHECK(llm != NULL);
    if (!llm) return;
    ca_llm_message msgs[] = {
        {"system", "you are a planner"},
        {"user", "创建 test/note.txt 写入内容为 hello"},
    };
    ca_llm_request req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.num_messages = 2;
    req.temperature = 0.0;
    ca_llm_response resp;
    memset(&resp, 0, sizeof(resp));
    CHECK(ca_llm_chat(llm, &req, &resp) == 0);
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
    ca_llm_destroy(llm);
}

/* ---------- knowledge index ---------- */
static void test_index(void) {
    section("knowledge");
    ca_index *idx = ca_index_new();
    CHECK(idx != NULL);
    CHECK(ca_index_add_file(idx, "src/a.c", "int ca_widget_init(void) { return 0; }") == 0);
    /* indexing and query tokenization are consistent (underscore is a word char) */
    char *r = ca_index_search(idx, "ca_widget_init", 10);
    CHECK(r != NULL);
    CHECK(strstr(r, "ca_widget_init") != NULL);
    CHECK(strstr(r, "src/a.c") != NULL);
    free(r);
    ca_index_free(idx);
}

/* ---------- metrics ---------- */
static void test_metrics(void) {
    section("metrics");
    ca_metrics *m = ca_metrics_new();
    ca_metrics_inc(m, "a.count");
    ca_metrics_add(m, "a.count", 2);
    ca_metrics_set(m, "a.gauge", 7);
    char *txt = ca_metrics_render(m);
    CHECK(txt && strstr(txt, "a.count 3") != NULL);
    CHECK(txt && strstr(txt, "a.gauge 7") != NULL);
    free(txt);
    ca_metrics_free(m);
}

static void test_config(void) {
    section("config");
    /* mirrors the cagent_init defaults + CA_* env layering */
    ca_config *c = ca_config_new();
    CHECK(ca_config_apply_json(c,
        "{\"llm.provider\":\"mock\",\"llm.base_url\":\"\",\"tx.use_transaction\":true}") == 0);
    /* no env override: falls back to the flat-dotted default (empty string) */
    CHECK(ca_config_get_str(c, "llm.base_url", NULL) != NULL);
    CHECK(strcmp(ca_config_get_str(c, "llm.base_url", NULL), "") == 0);
    CHECK(ca_config_get_bool(c, "tx.use_transaction", 0) == 1);
    /* env CA_LLM_BASE_URL writes the underscore-flattened key; it must win
     * over the empty-string flat-dotted default */
    CHECK(ca_config_apply_json(c,
        "{\"llm.base.url\":\"http://localhost:9000\",\"tx.use.transaction\":false}") == 0);
    CHECK(strcmp(ca_config_get_str(c, "llm.base_url", NULL),
                 "http://localhost:9000") == 0);
    CHECK(ca_config_get_bool(c, "tx.use_transaction", 1) == 0);
    /* provider/model keys have no underscore->dot collision */
    CHECK(strcmp(ca_config_get_str(c, "llm.provider", NULL), "mock") == 0);
    ca_config_free(c);
}

/* ---------- cognition: blackboard ---------- */
static void test_blackboard(void) {
    section("blackboard");
    ca_blackboard *b = ca_blackboard_new();
    CHECK(b != NULL);
    if (!b) return;
    CHECK(ca_blackboard_count(b) == 0);
    ca_blackboard_put(b, "k1", "v1");
    ca_blackboard_put(b, "k2", "v2");
    ca_blackboard_put(b, "k1", "v1b");   /* overwrite */
    CHECK(ca_blackboard_count(b) == 2);
    char *g = ca_blackboard_get(b, "k1");
    CHECK_STR(g, "v1b");
    free(g);
    CHECK(ca_blackboard_get(b, "missing") == NULL);
    CHECK(ca_blackboard_remove(b, "k1") == 1);
    CHECK(ca_blackboard_remove(b, "k1") == 0);
    CHECK(ca_blackboard_count(b) == 1);
    char *snap = ca_blackboard_snapshot_json(b);
    CHECK(snap && strstr(snap, "k2") != NULL && strstr(snap, "v2") != NULL);
    free(snap);
    ca_blackboard_free(b);
}

/* ---------- runtime: multi-agent coordinator ---------- */
static void test_agent_pool(void) {
    section("agent_pool");
    ca_agent_pool *p = ca_agent_pool_new();
    CHECK(p != NULL);
    if (!p) return;
    CHECK(ca_agent_pool_add(p, "planner", "plan") >= 0);
    CHECK(ca_agent_pool_add(p, "executor", "act") >= 0);
    CHECK(ca_agent_pool_add(p, "planner", "dup") == -1);  /* duplicate */
    CHECK(ca_agent_pool_count(p) == 2);
    CHECK(ca_agent_post(p, "planner", "plan", "step1") == 0);
    CHECK(ca_agent_post(p, "ghost", "k", "v") == -1);     /* unknown agent */
    ca_blackboard *bb = ca_agent_pool_blackboard(p);
    CHECK(bb != NULL);
    char *g = ca_blackboard_get(bb, "plan");
    CHECK_STR(g, "step1");
    free(g);
    char *snap = ca_agent_pool_snapshot_json(p);
    CHECK(snap && strstr(snap, "planner") != NULL && strstr(snap, "step1") != NULL);
    free(snap);
    ca_agent_pool_free(p);
}

/* ---------- api: auth ---------- */
static void test_auth(void) {
    section("auth");
    ca_auth *a = ca_auth_new();
    CHECK(a != NULL);
    if (!a) return;
    ca_auth_add_key(a, "secret-123");
    CHECK(ca_auth_count(a) == 1);
    CHECK(ca_auth_check(a, "secret-123") == 1);
    CHECK(ca_auth_check(a, "secret-124") == 0);
    CHECK(ca_auth_check_header(a, "Bearer secret-123") == 1);
    CHECK(ca_auth_check_header(a, "bearer secret-123") == 1);
    CHECK(ca_auth_check_header(a, "Bearer wrong") == 0);
    CHECK(ca_auth_check_header(a, NULL) == 0);
    char tok[33];
    ca_auth_generate_token(tok, 16);
    CHECK(strlen(tok) == 32);
    ca_auth_add_key(a, tok);
    CHECK(ca_auth_check(a, tok) == 1);
    ca_auth_free(a);
}

/* ---------- api: websocket (SHA1 + base64 + frames) ---------- */
static void test_websocket(void) {
    section("websocket");
    static const char *hexc = "0123456789abcdef";
    unsigned char sha[20];
    char hex[41];

    ca_sha1((const unsigned char *)"abc", 3, sha);
    for (int i = 0; i < 20; i++) { hex[i * 2] = hexc[sha[i] >> 4]; hex[i * 2 + 1] = hexc[sha[i] & 0xF]; }
    hex[40] = '\0';
    CHECK_STR(hex, "a9993e364706816aba3e25717850c26c9cd0d89d");

    ca_sha1((const unsigned char *)"", 0, sha);
    for (int i = 0; i < 20; i++) { hex[i * 2] = hexc[sha[i] >> 4]; hex[i * 2 + 1] = hexc[sha[i] & 0xF]; }
    CHECK_STR(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    char *b64 = ca_base64_encode((const unsigned char *)"the sample nonce", 16);
    CHECK_STR(b64, "dGhlIHNhbXBsZSBub25jZQ==");
    free(b64);

    unsigned char dec[64];
    size_t dec_len = 0;
    CHECK(ca_base64_decode("dGhlIHNhbXBsZSBub25jZQ==", dec, sizeof(dec), &dec_len) == 0);
    CHECK(dec_len == 16);
    CHECK(memcmp(dec, "the sample nonce", 16) == 0);

    char accept[29];
    ca_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept);
    CHECK_STR(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    /* short frame (7-bit length, masked) */
    size_t flen = 0;
    char *f = ca_ws_build_frame(CA_WS_OP_TEXT, (const unsigned char *)"hello", 5, 1, &flen);
    CHECK(f != NULL);
    if (f) {
        unsigned char pay[256];
        size_t plen = 0;
        int op = 0, fin = 0;
        CHECK(ca_ws_parse_frame((const unsigned char *)f, flen, pay, &plen, &op, &fin) == 0);
        CHECK(fin == 1 && op == CA_WS_OP_TEXT && plen == 5);
        CHECK(memcmp(pay, "hello", 5) == 0);
        free(f);
    }

    /* medium payload (16-bit length) */
    unsigned char med[200];
    for (int i = 0; i < 200; i++) med[i] = (unsigned char)(i & 0xFF);
    f = ca_ws_build_frame(CA_WS_OP_BINARY, med, 200, 1, &flen);
    CHECK(f != NULL);
    if (f) {
        unsigned char *pay = (unsigned char *)malloc(200);
        size_t plen = 0;
        int op = 0, fin = 0;
        CHECK(ca_ws_parse_frame((const unsigned char *)f, flen, pay, &plen, &op, &fin) == 0);
        CHECK(op == CA_WS_OP_BINARY && plen == 200);
        CHECK(memcmp(pay, med, 200) == 0);
        free(pay);
        free(f);
    }

    /* large payload (64-bit length, unmasked) */
    size_t big_n = 70000;
    unsigned char *big = (unsigned char *)malloc(big_n);
    for (size_t i = 0; i < big_n; i++) big[i] = (unsigned char)((i * 7) & 0xFF);
    f = ca_ws_build_frame(CA_WS_OP_BINARY, big, big_n, 0, &flen);
    CHECK(f != NULL);
    if (f) {
        unsigned char *pay = (unsigned char *)malloc(big_n);
        size_t plen = 0;
        int op = 0, fin = 0;
        CHECK(ca_ws_parse_frame((const unsigned char *)f, flen, pay, &plen, &op, &fin) == 0);
        CHECK(op == CA_WS_OP_BINARY && plen == big_n);
        CHECK(memcmp(pay, big, big_n) == 0);
        free(pay);
        free(f);
    }
    free(big);
}

/* ---------- plugin runtime: dynamic loader smoke ---------- */
static void test_plugin_loader(void) {
    section("plugin_loader");
    ca_plugin *p = ca_plugin_load("this_plugin_does_not_exist_xyz.so");
    CHECK(p == NULL);
    const char *err = ca_plugin_error();
    CHECK(err != NULL && *err != '\0');
}

/* ---------- memory: kv store ---------- */
static void test_kv(void) {
    section("kv");
    ca_kvstore *k = ca_kvstore_new();
    CHECK(k != NULL);
    if (!k) return;
    ca_kvstore_set(k, "a", "1");
    ca_kvstore_set(k, "b", "2");
    CHECK(ca_kvstore_count(k) == 2);
    CHECK_STR(ca_kvstore_get(k, "a"), "1");
    ca_kvstore_set(k, "a", "11");
    CHECK_STR(ca_kvstore_get(k, "a"), "11");
    CHECK(ca_kvstore_remove(k, "a") == 1);
    CHECK(ca_kvstore_get(k, "a") == NULL);
    char *j = ca_kvstore_snapshot_json(k);
    CHECK(j && strstr(j, "b") != NULL);
    free(j);
    ca_kvstore_free(k);
}

/* ---------- memory: episodic store ---------- */
static void test_episodic(void) {
    section("episodic");
    ca_episodic *e = ca_episodic_new();
    CHECK(e != NULL);
    if (!e) return;
    ca_episodic_add(e, "t1", "r1");
    ca_episodic_add(e, "t2", "r2");
    CHECK(ca_episodic_count(e) == 2);
    CHECK_STR(ca_episodic_task(e, 0), "t1");
    CHECK_STR(ca_episodic_result(e, 1), "r2");
    char *j = ca_episodic_json(e);
    CHECK(j && strstr(j, "t1") != NULL && strstr(j, "r2") != NULL);
    free(j);
    ca_episodic_free(e);
}

/* ---------- memory: vector store ---------- */
static void test_vector(void) {
    section("vector");
    ca_vectorstore *v = ca_vectorstore_new();
    CHECK(v != NULL);
    if (!v) return;
    ca_vectorstore_add(v, "1", "hello world foo", "m1");
    ca_vectorstore_add(v, "2", "goodbye world bar", "m2");
    CHECK(ca_vectorstore_count(v) == 2);
    char *n = ca_vectorstore_nearest(v, "hello", 2);
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
    ca_vectorstore_free(v);
}

/* ---------- memory: knowledge graph ---------- */
static void test_graph(void) {
    section("graph");
    ca_graph *g = ca_graph_new();
    CHECK(g != NULL);
    if (!g) return;
    CHECK(ca_graph_add_node(g, "n1", "node one") == 0);
    CHECK(ca_graph_add_node(g, "n2", "node two") == 0);
    CHECK(ca_graph_add_node(g, "n1", "dup") == -1);
    CHECK(ca_graph_node_count(g) == 2);
    CHECK(ca_graph_add_edge(g, "n1", "n2", "rel") == 0);
    CHECK(ca_graph_edge_count(g) == 1);
    char *nb = ca_graph_neighbors(g, "n1");
    CHECK(nb && strstr(nb, "n2") != NULL && strstr(nb, "rel") != NULL);
    free(nb);
    char *snap = ca_graph_snapshot_json(g);
    CHECK(snap && strstr(snap, "n1") != NULL && strstr(snap, "node one") != NULL);
    free(snap);
    ca_graph_free(g);
}

/* ---------- retrieval: context builder ---------- */
static void test_context_builder(void) {
    section("context_builder");
    ca_memory *m = ca_memory_new("state-test/memory-cb");
    CHECK(m != NULL);
    if (m) {
        ca_memory_working_push(m, "project uses the c language");
        ca_memory_record_experience(m, "write file", "done");
        char *ctx = ca_context_build(m, "project", 8);
        CHECK(ctx && strstr(ctx, "[") != NULL);
        free(ctx);
        ca_memory_free(m);
    }
    char *txt = ca_context_render_text(
        "[{\"kind\":\"working\",\"text\":\"hello\",\"result\":\"\",\"score\":1}]");
    CHECK(txt && strstr(txt, "hello") != NULL);
    free(txt);
}

/* ---------- cognition: planner + evaluator ---------- */
static void test_planner(void) {
    section("planner");
    ca_llm *llm = ca_llm_create("mock", NULL, NULL, "mock");
    CHECK(llm != NULL);
    if (!llm) return;
    ca_planned_action *actions = NULL;
    int n = -1;
    char *raw = NULL;
    CHECK(ca_planner_plan(llm, "创建 test/note.txt 写入内容为 hello", &actions, &n, &raw) == 0);
    CHECK(n >= 1);
    CHECK(actions != NULL);
    if (n >= 1 && actions) CHECK_STR(actions[0].tool, "file_write");
    CHECK(raw != NULL);
    ca_planner_actions_free(actions, n);
    free(raw);
    ca_llm_destroy(llm);
}

static void test_evaluator(void) {
    section("evaluator");
    ca_evaluator *ev = ca_evaluator_new();
    CHECK(ca_evaluator_verify(ev, 1, 2) == 1);
    CHECK(ca_evaluator_verify(ev, 0, 2) == 0);
    CHECK(ca_evaluator_score(ev, 2, 2, 1, "ok") > 0.5);
    CHECK(ca_evaluator_score(ev, 2, 1, 0, "FAILED") == 0.0);
    ca_evaluator_free(ev);
}

/* ---------- plugin runtime: sandbox + capability ---------- */
static void test_sandbox(void) {
    section("sandbox");
    ca_sandbox *sb = ca_sandbox_new(5000);
    CHECK(ca_sandbox_forbidden("rm -rf /") == 1);
    CHECK(ca_sandbox_forbidden("echo hi") == 0);
    ca_sandbox_result *r = ca_sandbox_run(sb, "echo hello");
    CHECK(r != NULL);
    if (r) {
        CHECK(r->ok == 1);
        CHECK(r->output && strstr(r->output, "hello") != NULL);
        ca_sandbox_result_free(r);
    }
    CHECK(ca_sandbox_run(sb, "rm -rf /tmp/x") == NULL);
    ca_sandbox_free(sb);
}

static void test_capability(void) {
    section("capability");
    ca_capability *c = ca_capability_new();
    CHECK(c != NULL);
    if (!c) return;
    CHECK(ca_capability_grant(c, "fs.read") == 0);
    CHECK(ca_capability_grant(c, "fs.write") == 0);
    CHECK(ca_capability_grant(c, "net") == 0);
    CHECK(ca_capability_grant(c, "fs.read") == -1);
    CHECK(ca_capability_count(c) == 3);
    CHECK(ca_capability_has(c, "fs.read") == 1);
    CHECK(ca_capability_match(c, "fs.*") == 1);
    CHECK(ca_capability_match(c, "net.*") == 1);
    CHECK(ca_capability_match(c, "proc.*") == 0);
    CHECK(ca_capability_revoke(c, "fs.read") == 1);
    CHECK(ca_capability_has(c, "fs.read") == 0);
    char *j = ca_capability_json(c);
    CHECK(j && strstr(j, "fs.write") != NULL);
    free(j);
    ca_capability_free(c);
}

/* ---------- plugin intelligence ---------- */
static void test_plugin_intelligence(void) {
    section("plugin_intelligence");
    char *a = ca_analyzer_analyze(
        "{\"name\":\"p\",\"description\":\"read and write files over an http api\"}");
    CHECK(a && strstr(a, "complexity") != NULL);
    CHECK(a && strstr(a, "fs.read") != NULL);
    CHECK(a && strstr(a, "net") != NULL);
    free(a);

    char *d = ca_architect_design("build a file sync plugin");
    CHECK(d && strstr(d, "components") != NULL && strstr(d, "interfaces") != NULL);
    free(d);

    char *cg = ca_codegen_plugin("My Plugin", "does things");
    CHECK(cg && strstr(cg, "My_Plugin") != NULL);
    CHECK(cg && strstr(cg, "run") != NULL);
    free(cg);

    char *tp = ca_testing_plan("{\"name\":\"p\"}");
    CHECK(tp && strstr(tp, "cases") != NULL);
    free(tp);

    char *tr = ca_testing_run("echo ok", 5000);
    CHECK(tr && strstr(tr, "ok") != NULL);
    free(tr);

    char *sec = ca_security_audit("system(\"rm -rf /\")");
    CHECK(sec && strstr(sec, "system(") != NULL);
    CHECK(sec && strstr(sec, "rm -rf") != NULL);
    free(sec);
}

/* ---------- observability: trace ---------- */
static void test_trace(void) {
    section("trace");
    ca_trace *t = ca_trace_new(8);
    CHECK(t != NULL);
    if (!t) return;
    int64_t id = ca_trace_begin(t, "span-a");
    CHECK(id > 0);
    int64_t id2 = ca_trace_begin(t, "span-b");
    CHECK(id2 > id);
    ca_trace_end(t, id, 1);
    CHECK(ca_trace_count(t) == 2);
    char *j = ca_trace_json(t);
    CHECK(j && strstr(j, "span-a") != NULL);
    free(j);
    ca_trace_clear(t);
    CHECK(ca_trace_count(t) == 0);
    ca_trace_free(t);
}

/* ---------- llm: router + usage ---------- */
static void test_router(void) {
    section("router");
    ca_router *r = ca_router_new();
    CHECK(r != NULL);
    if (!r) return;
    CHECK(ca_router_add(r, "a", "openai", "https://a", "k", "gpt-4", 1.0) == 0);
    CHECK(ca_router_add(r, "b", "anthropic", "https://b", NULL, "claude", 2.0) == 0);
    CHECK(ca_router_count(r) == 2);
    CHECK(ca_router_pick(r) != NULL);
    CHECK(ca_router_pick(r) != NULL);
    char *j = ca_router_json(r);
    CHECK(j && strstr(j, "openai") != NULL);
    free(j);
    ca_router_free(r);
}

static void test_usage(void) {
    section("usage");
    ca_usage *u = ca_usage_new();
    CHECK(u != NULL);
    if (!u) return;
    ca_usage_add(u, "gpt-4", 100, 50);
    ca_usage_add(u, "gpt-4", 20, 10);
    ca_usage_add(u, "claude", 5, 5);
    CHECK(ca_usage_prompt_total(u) == 125);
    CHECK(ca_usage_completion_total(u) == 65);
    char *j = ca_usage_json(u);
    CHECK(j && strstr(j, "gpt-4") != NULL);
    free(j);
    ca_usage_free(u);
}

/* ---------- plugin registry ---------- */
static void test_registry(void) {
    section("plugin_registry");
    ca_plugin_registry *r = ca_plugin_registry_new();
    CHECK(r != NULL);
    if (!r) return;
    ca_plugin_meta m1;
    memset(&m1, 0, sizeof(m1));
    m1.name = "p1";
    m1.version = "1.0.0";
    char *c1[] = { "fs.read" };
    m1.caps = c1;
    m1.n_caps = 1;
    char *d1[] = { "base" };
    m1.deps = d1;
    m1.n_deps = 1;
    CHECK(ca_plugin_registry_register(r, &m1) == 0);
    CHECK(ca_plugin_registry_register(r, &m1) == -1); /* duplicate same-version */
    CHECK(ca_plugin_registry_deps_met(r, "p1") == 0);

    ca_plugin_meta base;
    memset(&base, 0, sizeof(base));
    base.name = "base";
    base.version = "1.0.0";
    CHECK(ca_plugin_registry_register(r, &base) == 0);
    CHECK(ca_plugin_registry_deps_met(r, "p1") == 1);
    CHECK(ca_plugin_registry_count(r) == 2);

    const ca_plugin_meta *f = ca_plugin_registry_find(r, "p1");
    CHECK(f != NULL && strcmp(f->version, "1.0.0") == 0);
    CHECK(ca_plugin_registry_set_enabled(r, "p1", 0) == 0);
    char *j = ca_plugin_registry_json(r);
    CHECK(j && strstr(j, "p1") != NULL);
    free(j);
    CHECK(ca_plugin_registry_unregister(r, "base") == 0);
    CHECK(ca_plugin_registry_count(r) == 1);
    ca_plugin_registry_free(r);
}

/* ---------- skills ---------- */
static void test_skills(void) {
    section("skills");
    ca_skill_registry *r = ca_skill_registry_new();
    CHECK(r != NULL);
    if (!r) return;
    ca_skill s = { "echo_hi", "print hi", "shell", "echo hi" };
    CHECK(ca_skill_register(r, &s) == 0);
    CHECK(ca_skill_register(r, &s) == -1); /* duplicate */
    CHECK(ca_skill_count(r) == 1);
    const ca_skill *f = ca_skill_find(r, "echo_hi");
    CHECK(f != NULL && strcmp(f->kind, "shell") == 0);
    ca_skill_result *res = ca_skill_execute(r, "echo_hi", NULL, NULL, 5000);
    CHECK(res != NULL);
    if (res) {
        CHECK(res->ok == 1);
        CHECK(res->output && strstr(res->output, "hi") != NULL);
        ca_skill_result_free(res);
    }
    char *j = ca_skill_list_json(r);
    CHECK(j && strstr(j, "echo_hi") != NULL);
    free(j);
    ca_skill_registry_free(r);
}

/* ---------- mcp manager ---------- */
static void test_mcp(void) {
    section("mcp");
    ca_mcp_manager *m = ca_mcp_manager_new();
    CHECK(m != NULL);
    if (!m) return;
    CHECK(ca_mcp_manager_add(m, "srv1", "http://127.0.0.1:9000/mcp", "tok") == 0);
    CHECK(ca_mcp_manager_add(m, "srv1", "http://127.0.0.1:9001/mcp", NULL) == 0); /* update */
    CHECK(ca_mcp_manager_count(m) == 1);
    const ca_mcp_conn *c = ca_mcp_manager_find(m, "srv1");
    CHECK(c != NULL && strstr(c->url, "9001") != NULL);
    char *j = ca_mcp_manager_json(m);
    CHECK(j && strstr(j, "srv1") != NULL);
    free(j);
    /* unreachable server: call returns NULL without crashing */
    CHECK(ca_mcp_manager_call(m, "srv1", "ping", "{}") == NULL);
    CHECK(ca_mcp_manager_remove(m, "srv1") == 0);
    CHECK(ca_mcp_manager_count(m) == 0);
    ca_mcp_manager_free(m);
}

/* ---------- cluster ---------- */
static void test_cluster(void) {
    section("cluster");
    ca_cluster *c = ca_cluster_new();
    CHECK(c != NULL);
    if (!c) return;
    CHECK(ca_cluster_upsert(c, "n1", "10.0.0.1", 8080, "worker") == 0);
    CHECK(ca_cluster_upsert(c, "n2", "10.0.0.2", 8080, "coordinator") == 0);
    CHECK(ca_cluster_count(c) == 2);
    CHECK(ca_cluster_up_count(c) == 2);

    ca_cluster_mark_down(c, -1); /* force every node stale */
    CHECK(ca_cluster_up_count(c) == 0);
    const ca_cluster_node *n2 = ca_cluster_find(c, "n2");
    CHECK(n2 != NULL && strcmp(n2->status, "down") == 0);

    CHECK(ca_cluster_heartbeat(c, "n1") == 0);
    CHECK(ca_cluster_up_count(c) == 1);
    const ca_cluster_node *n1 = ca_cluster_find(c, "n1");
    CHECK(n1 != NULL && strcmp(n1->status, "up") == 0);

    char *j = ca_cluster_json(c);
    CHECK(j && strstr(j, "n2") != NULL);
    free(j);
    CHECK(ca_cluster_remove(c, "n1") == 0);
    CHECK(ca_cluster_count(c) == 1);
    ca_cluster_free(c);
}

/* ---------- attention ---------- */
static void test_attention(void) {
    section("attention");
    ca_attention *a = ca_attention_new();
    CHECK(a != NULL);
    if (!a) return;
    ca_attention_candidate cands[3];
    cands[0].text = "the weather in paris is sunny"; cands[0].tags = "weather"; cands[0].boost = 0.0;
    cands[1].text = "stock market report";           cands[1].tags = "finance"; cands[1].boost = 0.0;
    cands[2].text = "paris travel guide";            cands[2].tags = "travel";  cands[2].boost = 0.0;
    CHECK(ca_attention_score(a, "paris weather", &cands[0]) >
          ca_attention_score(a, "paris weather", &cands[1]));
    ca_attention_result out[3];
    int k = ca_attention_select(a, "paris weather", cands, 3, out, 3);
    CHECK(k == 3);
    CHECK(out[0].index == 0);
    CHECK(out[0].score >= out[1].score && out[1].score >= out[2].score);
    ca_attention_free(a);
}

/* ---------- infra: lock-free ring buffer ---------- */
static void test_ringbuf(void) {
    section("ringbuf");
    ca_ringbuf *r = ca_ringbuf_new(8);
    CHECK(r != NULL);
    if (!r) return;
    /* basic FIFO */
    void *out = NULL;
    CHECK(ca_ringbuf_pop(r, &out) == 0);            /* empty */
    for (int i = 1; i <= 8; i++) CHECK(ca_ringbuf_push(r, (void *)(intptr_t)(size_t)i) == 1);
    CHECK(ca_ringbuf_push(r, (void *)(intptr_t)9) == 0);  /* full */
    for (int i = 1; i <= 8; i++) {
        CHECK(ca_ringbuf_pop(r, &out) == 1);
        CHECK((intptr_t)(size_t)out == i);
    }
    CHECK(ca_ringbuf_pop(r, &out) == 0);
    /* wrap-around */
    for (int i = 1; i <= 4; i++) CHECK(ca_ringbuf_push(r, (void *)(intptr_t)(size_t)i) == 1);
    for (int i = 1; i <= 4; i++) CHECK(ca_ringbuf_pop(r, &out) == 1);
    for (int i = 5; i <= 12; i++) CHECK(ca_ringbuf_push(r, (void *)(intptr_t)(size_t)i) == 1);
    for (int i = 5; i <= 12; i++) {
        CHECK(ca_ringbuf_pop(r, &out) == 1);
        CHECK((intptr_t)(size_t)out == i);
    }
    ca_ringbuf_free(r);
}

#define RB_NPROD 4
#define RB_PER   2000
#define RB_TOTAL (RB_NPROD * RB_PER)
static _Atomic int rb_seen[RB_TOTAL];
static _Atomic int rb_dup;
static _Atomic int rb_err;
static ca_ringbuf *rb_shared;

static void rb_producer(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int k = 0; k < RB_PER; k++) {
        int v = id * RB_PER + k + 1;
        int i = 0;
        while (ca_ringbuf_push(rb_shared, (void *)(intptr_t)(size_t)v) != 1 && i < 100000) { i++; ca_time_sleep_ms(1); }
        if (i >= 100000) atomic_fetch_add(&rb_err, 1);
    }
}
static void rb_consumer(void *arg) {
    ca_ringbuf *r = (ca_ringbuf *)arg;
    int seen = 0;
    int64_t deadline = ca_time_now_ms() + 8000;
    while (seen < RB_TOTAL && ca_time_now_ms() < deadline) {
        void *out = NULL;
        if (ca_ringbuf_pop(r, &out) == 1) {
            int v = (int)(intptr_t)(size_t)out;
            if (v < 1 || v > RB_TOTAL) { atomic_fetch_add(&rb_err, 1); continue; }
            int prev = atomic_fetch_add(&rb_seen[v - 1], 1);
            if (prev != 0) atomic_fetch_add(&rb_dup, 1);
            seen++;
        } else {
            ca_time_sleep_ms(1);
        }
    }
    atomic_fetch_add(&rb_err, RB_TOTAL - seen);
}

static void test_ringbuf_mpmc(void) {
    section("ringbuf_mpmc");
    ca_ringbuf *r = ca_ringbuf_new(64);
    CHECK(r != NULL);
    if (!r) return;
    rb_shared = r;
    ca_thread *prods[RB_NPROD];
    for (int i = 0; i < RB_NPROD; i++)
        prods[i] = ca_thread_create(rb_producer, (void *)(intptr_t)i);
    ca_thread *cons = ca_thread_create(rb_consumer, r);
    for (int i = 0; i < RB_NPROD; i++) ca_thread_join(prods[i]);
    ca_thread_join(cons);
    ca_ringbuf_free(r);
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
    ca_embedding_use_local();
    CHECK_STR(ca_embedding_provider_name(), "local");
    float a[CA_EMBED_DIM], b[CA_EMBED_DIM], c[CA_EMBED_DIM];
    ca_embed_text("hello world foo bar", a);
    ca_embed_text("hello world foo bar", b);
    ca_embed_text("completely different text here", c);
    CHECK(ca_embed_cosine(a, b, CA_EMBED_DIM) > 0.99f);
    CHECK(ca_embed_cosine(a, c, CA_EMBED_DIM) < ca_embed_cosine(a, b, CA_EMBED_DIM));
    /* rerank: relevant doc scores higher than unrelated doc */
    const char *docs[2] = {
        "how to create a file with hello content",
        "quantum entanglement of distant stars"
    };
    float scores[2];
    CHECK(ca_embed_rerank("create file hello", docs, 2, scores) == 0);
    CHECK(scores[0] > scores[1]);
}

/* ---------- im: instant messaging store ---------- */
static void test_im(void) {
    section("im");
    const char *root = "state-im-test";
    char store[600];
    snprintf(store, sizeof(store), "%s/im/sessions.json", root);
    ca_fs_remove(store);   /* remove stale store from a previous run */
    ca_fs_remove(root);    /* best-effort (fails on non-empty dir) */
    ca_im *im = ca_im_new(root);
    CHECK(im != NULL);
    if (!im) return;
    int64_t s1 = ca_im_create_session(im, "测试会话");
    CHECK(s1 > 0);
    int64_t s2 = ca_im_create_session(im, "general");
    CHECK(s2 > 0 && s2 != s1);
    CHECK(ca_im_send(im, s1, "user", "你好") > 0);
    CHECK(ca_im_send(im, s1, "assistant", "你好！") > 0);
    CHECK(ca_im_send(im, 9999, "user", "x") < 0);   /* unknown session */
    size_t n = 0;
    ca_im_message *msgs = ca_im_messages(im, s1, &n);
    CHECK(msgs != NULL && n == 2);
    if (msgs) {
        CHECK_STR(msgs[0].role, "user");
        CHECK_STR(msgs[0].content, "你好");
        CHECK_STR(msgs[1].role, "assistant");
    }
    ca_im_messages_free(msgs, n);
    CHECK(ca_im_total_messages(im) == 2);
    CHECK(ca_im_delete_session(im, s2) == 1);
    CHECK(ca_im_delete_session(im, 9999) == 0);
    ca_im_free(im);
    /* reload from disk */
    ca_im *im2 = ca_im_new(root);
    CHECK(im2 != NULL);
    if (im2) {
        size_t ns = 0;
        ca_im_session *sess = ca_im_list_sessions(im2, &ns);
        CHECK(sess != NULL && ns == 1);
        if (sess) {
            CHECK(sess[0].id == s1);
            CHECK_STR(sess[0].name, "测试会话");
        }
        ca_im_sessions_free(sess, ns);
        char *j = ca_im_sessions_json(im2);
        CHECK(j && strstr(j, "测试会话") != NULL);
        free(j);
        ca_im_free(im2);
    }
    ca_fs_remove(store);
    ca_fs_remove(root);
}

/* ---------- plugin intelligence: AI plugin generation (mock mode) ---------- */
static void test_plugin_generate(void) {
    section("plugin_generate");
    const char *root = "state-plugin-test";
    ca_fs_remove(root);
    cagent_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_root = root;
    cfg.workspace = ".";
    cfg.provider = "mock";
    cfg.http_port = 0;
    cagent_ctx ctx;
    if (cagent_init(&ctx, &cfg) != 0) { CHECK(0); return; }
    char *res = ca_plugin_generate(&ctx, "创建读取配置文件 config.json 的插件");
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
    /* registered in the plugin registry */
    CHECK(ca_plugin_registry_count(ctx.registry) >= 1);
    /* and runnable as a skill */
    CHECK(ca_skill_count(ctx.skills) >= 1);
    ca_skill_result *sr = NULL;
    for (size_t i = 0; i < (size_t)ca_skill_count(ctx.skills); i++) {
        const ca_skill *sk = ca_skill_get(ctx.skills, i);
        if (sk && (strncmp(sk->name, "cap", 3) == 0 || strstr(sk->name, "config") != NULL)) {
            sr = ca_skill_execute(ctx.skills, sk->name, NULL, ".", 10000);
            break;
        }
    }
    CHECK(sr != NULL && sr->ok);
    if (sr) ca_skill_result_free(sr);
    cagent_shutdown(&ctx);
    ca_fs_remove(root);
}

/* ---------- sandbox: wasm3 runner ---------- */
static const unsigned char WASM_ADD[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01,
    0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09,
    0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b
};

static void test_sandbox_wasm(void) {
    section("sandbox_wasm");
    /* before registering a runner the seam reports "unsupported" */
    CHECK(ca_sandbox_wasm_supported() == 0);
    char *we = ca_sandbox_run_wasm("\0asm", 4, "add", "{}");
    CHECK(we != NULL && strstr(we, "not registered") != NULL);
    free(we);

    /* register the wasm3-backed runner */
    ca_sandbox_set_wasm_runner(ca_wasm3_run);
    CHECK(ca_sandbox_wasm_supported() == 1);
    char *r = ca_sandbox_run_wasm(WASM_ADD, sizeof(WASM_ADD), "add", "[2,40]");
    CHECK(r != NULL && strstr(r, "\"result\":42") != NULL);
    free(r);
    r = ca_sandbox_run_wasm(WASM_ADD, sizeof(WASM_ADD), "add", "{\"a\":10,\"b\":32}");
    CHECK(r != NULL && strstr(r, "\"result\":42") != NULL);
    free(r);
    /* missing function -> error json */
    r = ca_sandbox_run_wasm(WASM_ADD, sizeof(WASM_ADD), "nope", "[]");
    CHECK(r != NULL && strstr(r, "ok\":false") != NULL);
    free(r);
}

/* ---------- runtime: task lifecycle ---------- */
static void test_task(void) {
    section("task");
    ca_task *t = ca_task_new(42, 3, "hello task", 1000);
    CHECK(t != NULL);
    if (!t) return;
    CHECK(t->status == CA_TS_QUEUED);
    CHECK_STR(t->input, "hello task");
    CHECK_STR(ca_task_status_name(CA_TS_FAILED), "failed");
    ca_task_transition(t, CA_TS_RUNNING, 0);
    CHECK(t->started_ms > 0);
    ca_task_transition(t, CA_TS_DONE, 0);
    CHECK(t->finished_ms > 0);
    CHECK(t->status == CA_TS_DONE);
    char *j = ca_task_to_json(t);
    CHECK(j && strstr(j, "hello task") != NULL && strstr(j, "done") != NULL);
    free(j);
    ca_task_free(t);
}

int main(void) {
    printf("c-agent unit tests\n");
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
    test_planner();
    test_evaluator();
    test_sandbox();
    test_capability();
    test_plugin_intelligence();
    test_trace();
    test_router();
    test_usage();
    test_registry();
    test_skills();
    test_mcp();
    test_cluster();
    test_attention();
    test_sandbox_wasm();
    test_im();
    test_plugin_generate();
    test_task();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
