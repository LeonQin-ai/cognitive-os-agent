/* test_scenario.c — realistic complex agent scenarios through the full
 * runtime (built-in mock provider, no network needed):
 *
 *   S1  multi-turn project workflow: file writes + read + shell across one
 *       session — session notes, code index, entity graph, and episode
 *       consolidation after 10 recorded episodes
 *   S2  missing-capability self-evolution: the mock plans a tool that is not
 *       in the registry -> reasoning auto-generates a plugin, binds it under
 *       the planned tool name, the action succeeds, and the plugin/skill/
 *       script are observable; a second run reuses the bound tool
 *   S3  multi-agent: run a task AS a named agent, result lands on the
 *       shared blackboard
 *   S4  HTTP API tour: queued task via /v1/tasks + polling, agent add +
 *       /v1/agents/<name>/run, /v1/plugins/generate, native loader error
 *       paths, generated skill run, MCP stdio add + tool discovery
 *   S5  huge files: 10G/20G/100G sparse files created by the agent must not
 *       be read into the snapshot engine (size guard); a git-managed
 *       workspace bypasses snapshots entirely
 *
 * Build:  ./build.sh scenario   (or: ./build.sh all)
 */
#include "cognitive-os-agent/cognitive-os-agent.h"
#include "cognitive-os-agent/plugin_runtime/manager.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/os/os_proc.h"
#include "cognitive-os-agent/os/os_socket.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

#ifdef COA_WINDOWS
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/statvfs.h>
#endif

static int g_fail = 0;
static int g_pass = 0;
#define CHECK(cond) do { if (cond) { g_pass++; } \
                        else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void section(const char *s) { printf("== %s ==\n", s); }

static void th_serve_ctx(void *arg) { coa_serve((coa_ctx *)arg); }

/* ---- helpers ---- */

static void rm_tree_files(const char *path) {
    coa_dir_list dl;
    if (coa_fs_list_dir(path, &dl) == 0) {
        for (size_t i = 0; i < dl.count; i++) {
            char sub[1024];
            coa_path_join(sub, sizeof sub, path, dl.items[i].name);
            if (dl.items[i].is_dir) rm_tree_files(sub);
            else coa_fs_remove(sub);
            free(dl.items[i].name);
        }
        free(dl.items);
    }
#ifdef COA_WINDOWS
    RemoveDirectoryA(path);
#else
    rmdir(path);
#endif

}

/* Minimal HTTP/1.1 client over the raw socket primitives (same pattern as
 * test_all.c; avoids the clashing coa_http_response in http.h). */
typedef struct { int status; char body[65536]; size_t body_len; } raw_http;

static int raw_http_request(uint16_t port, const char *method, const char *path,
                            const char *body, raw_http *out) {
    coa_socket *c = coa_sock_connect("127.0.0.1", port, 3000);
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
        int w = coa_sock_send(c, req + sent, (size_t)(n - sent));
        if (w <= 0) { coa_sock_close(c); return -1; }
        sent += w;
    }
    char hdr[2048]; size_t hn = 0;
    while (hn < sizeof hdr - 1) {
        int rr = coa_sock_recv(c, hdr + hn, 1);
        if (rr <= 0) break;
        hn++; hdr[hn] = '\0';
        if (hn >= 4 && memcmp(hdr + hn - 4, "\r\n\r\n", 4) == 0) break;
    }
    int status = 0;
    sscanf(hdr, "HTTP/1.1 %d", &status);
    /* honor Content-Length when present (the server may keep the connection
     * alive instead of closing it) */
    size_t want = (size_t)-1;
    {
        const char *cl = strstr(hdr, "Content-Length:");
        if (cl) want = (size_t)strtoull(cl + 15, NULL, 10);
    }
    size_t bl = 0;
    while (bl < sizeof out->body - 1) {
        size_t room = sizeof out->body - 1 - bl;
        if (want != (size_t)-1 && want - bl < room) room = want - bl;
        if (want != (size_t)-1 && bl >= want) break;
        int rr = coa_sock_recv(c, out->body + bl, room);
        if (rr <= 0) break;
        bl += (size_t)rr;
        if (want != (size_t)-1 && bl >= want) break;
    }
    out->body[bl] = '\0';
    out->body_len = bl;
    out->status = status;
    coa_sock_close(c);
    return 0;
}

#define P(msg) printf("  .. %s\n", msg)

static int node_available(void) {
    coa_proc_result *r = coa_proc_run("node --version", 5000);
    int ok = r && r->exit_code == 0;
    coa_proc_result_free(r);
    return ok;
}

static void base_cfg(coa_config *cfg, const char *root, uint16_t port) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->state_root = root;
    cfg->workspace = "state-scen-w"; /* unused when workspace passed below */
    cfg->provider = "mock";
    cfg->http_port = port;
    cfg->workers = 2;
    cfg->use_transaction = 1;
}

/* ---- S1: multi-turn project workflow ---- */
static void s1_project_workflow(void) {
    section("S1 multi-turn project workflow");
    const char *root = "state-scen-1";
    rm_tree_files(root); /* fresh start (a killed run may leave state) */
    coa_ctx ctx;
    coa_config cfg;
    base_cfg(&cfg, root, 0);
    cfg.workspace = "state-scen-1/w";
    CHECK(coa_init(&ctx, &cfg) == 0);

    char *ans = NULL;
    CHECK(coa_run(&ctx, "创建文件 src/main.cpp 写入内容为 hello-main", &ans) == 0);
    free(ans);
    CHECK(coa_run(&ctx, "创建文件 src/util.cpp 写入内容为 util-code", &ans) == 0);
    free(ans);

    /* files really exist with the written content */
    char *d = coa_fs_read_file("state-scen-1/w/src/main.cpp");
    CHECK(d != NULL && strstr(d, "hello-main") != NULL);
    free(d);

    /* read back through the agent */
    CHECK(coa_run(&ctx, "读取 src/main.cpp", &ans) == 0);
    CHECK(ans != NULL && strstr(ans, "hello-main") != NULL);
    free(ans);

    /* shell action */
    CHECK(coa_run(&ctx, "执行命令 echo scen-shell-ok", &ans) == 0);
    CHECK(ans != NULL && strstr(ans, "scen-shell-ok") != NULL);
    free(ans);

    /* glob + grep through the full runtime (mock maps 查找文件/搜索) */
    CHECK(coa_run(&ctx, "查找文件 **/*.cpp", &ans) == 0);
    CHECK(ans != NULL && strstr(ans, "src/main.cpp") != NULL);
    free(ans);
    CHECK(coa_run(&ctx, "搜索 hello-main", &ans) == 0);
    CHECK(ans != NULL && strstr(ans, "src/main.cpp:1") != NULL);
    free(ans);

    /* agent loop: analyze -> fix -> final answer across rounds */
    coa_fs_write_file("state-scen-1/w/note.txt", "v1 OLD v1", 9);
    CHECK(coa_run(&ctx, "分析 note.txt 并修复其中的 OLD", &ans) == 0);
    CHECK(ans != NULL && strstr(ans, "[file_read]") != NULL);
    CHECK(ans != NULL && strstr(ans, "[file_edit]") != NULL);
    free(ans);
    d = coa_fs_read_file("state-scen-1/w/note.txt");
    CHECK(d != NULL && strstr(d, "NEW") != NULL && strstr(d, "OLD") == NULL);
    free(d);

    /* session notes should record the touched files */
    char *sj = coa_reasoning_session_json(ctx.reasoning);
    CHECK(sj != NULL && strstr(sj, "src/main.cpp") != NULL);
    free(sj);

    /* entity graph: task -> file_write -> touched file */
    char *rel = coa_memory_graph_related(ctx.memory, "main.cpp", 10);
    CHECK(rel != NULL && strstr(rel, "touched") != NULL);
    free(rel);

    /* consolidation: episodes with a recurring token (>= 3) distill into a
     * "topic.<token>" fact; ep 10 triggers the pass */
    for (int i = 0; i < 7; i++) {
        char p[128];
        snprintf(p, sizeof p, "parser 任务%d 聊聊话题%d", i, i);
        CHECK(coa_run(&ctx, p, &ans) == 0);
        free(ans);
    }
    const char *fact = coa_memory_recall(ctx.memory, "topic.parser");
    CHECK(fact != NULL && strstr(fact, "seen in") != NULL);

    coa_shutdown(&ctx);
    rm_tree_files(root);
}

/* ---- S2: missing-capability self-evolution ---- */
static void s2_auto_evolution(void) {
    section("S2 missing-capability self-evolution");
    const char *root = "state-scen-2";
    rm_tree_files(root); /* fresh start (a killed run may leave state) */
    coa_ctx ctx;
    coa_config cfg;
    base_cfg(&cfg, root, 0);
    cfg.workspace = "state-scen-2/w";
    CHECK(coa_init(&ctx, &cfg) == 0);

    /* before: tool unknown */
    CHECK(coa_tool_find(ctx.tools, "weather_lookup") == NULL);

    char *ans = NULL;
    CHECK(coa_run(&ctx, "查询天气 北京", &ans) == 0);
    CHECK(ans != NULL && strstr(ans, "plugin:") != NULL); /* generated skill ran */
    free(ans);

    /* the planned tool is now bound and the generated pieces observable */
    CHECK(coa_tool_find(ctx.tools, "weather_lookup") != NULL);
    char *pj = ctx.registry ? coa_plugin_registry_json(ctx.registry) : NULL;
    CHECK(pj != NULL && strstr(pj, "version") != NULL);
    free(pj);
    char *sk = ctx.skills ? coa_skill_list_json(ctx.skills) : NULL;
    CHECK(sk != NULL && strstr(sk, "generated") == NULL); /* list has name/kind */
    free(sk);

    /* second run reuses the bound tool (no re-generation -> still exactly
     * one plugin in the registry) */
    CHECK(coa_run(&ctx, "查询天气 上海", &ans) == 0);
    CHECK(ans != NULL && strstr(ans, "plugin:") != NULL);
    free(ans);
    int regs = 0;
    pj = coa_plugin_registry_json(ctx.registry);
    if (pj) {
        const char *q = pj;
        while ((q = strstr(q, "\"version\"")) != NULL) { regs++; q++; }
        free(pj);
    }
    CHECK(regs == 1);

    coa_shutdown(&ctx);
    rm_tree_files(root);
}

/* ---- S3: multi-agent blackboard ---- */
static void s3_multi_agent(void) {
    section("S3 multi-agent blackboard");
    const char *root = "state-scen-3";
    rm_tree_files(root); /* fresh start (a killed run may leave state) */
    coa_ctx ctx;
    coa_config cfg;
    base_cfg(&cfg, root, 0);
    cfg.workspace = "state-scen-3/w";
    CHECK(coa_init(&ctx, &cfg) == 0);

    CHECK(coa_agent_pool_add(ctx.agents, "researcher", "summarizes topics") >= 0);

    char *ans = NULL;
    CHECK(coa_agent_run(&ctx, "researcher", "总结 parser 讨论要点", &ans) == 0);
    CHECK(ans != NULL && *ans != '\0');
    free(ans);

    /* result published on the shared blackboard */
    char *snap = coa_agent_pool_snapshot_json(ctx.agents);
    CHECK(snap != NULL && strstr(snap, "researcher") != NULL);
    CHECK(snap != NULL && strstr(snap, "已收到请求") != NULL); /* mock text reply */
    free(snap);

    /* unknown agent is rejected */
    CHECK(coa_agent_run(&ctx, "nobody", "task", &ans) == -2);

    coa_shutdown(&ctx);
    rm_tree_files(root);
}

/* ---- S4: HTTP API tour ---- */
static void s4_http_api(void) {
    section("S4 http api tour");
    const char *root = "state-scen-4";
    rm_tree_files(root); /* fresh start (a killed run may leave state) */
    coa_ctx ctx;
    coa_config cfg;
    base_cfg(&cfg, root, 18251);
    cfg.workspace = "state-scen-4/w";
    CHECK(coa_init(&ctx, &cfg) == 0);
    coa_thread *srv = coa_thread_create(th_serve_ctx, &ctx);
    CHECK(srv != NULL);
    coa_time_sleep_ms(300);

    /* queued task -> poll until DONE -> file created */
    raw_http r;
    P("POST /v1/tasks");
    CHECK(raw_http_request(18251, "POST", "/v1/tasks",
                           "{\"prompt\":\"创建文件 api_done.txt 写入内容为 api-task\"}", &r) == 0);
    CHECK(r.status == 200);
    long long id = -1;
    sscanf(r.body, "{\"id\":%lld", &id);
    CHECK(id >= 0);
    int done = 0;
    for (int i = 0; i < 60 && !done; i++) {
        char path[64];
        snprintf(path, sizeof path, "/v1/tasks/%lld", id);
        CHECK(raw_http_request(18251, "GET", path, NULL, &r) == 0);
        if (strstr(r.body, "DONE") || strstr(r.body, "FAILED")) done = 1;
        else coa_time_sleep_ms(100);
    }
    CHECK(done && strstr(r.body, "DONE") != NULL);
    char *d = coa_fs_read_file("state-scen-4/w/api_done.txt");
    CHECK(d != NULL && strstr(d, "api-task") != NULL);
    free(d);

    /* agent registration + run over HTTP */
    P("POST /v1/agents");
    CHECK(raw_http_request(18251, "POST", "/v1/agents",
                           "{\"name\":\"planner-agent\",\"role\":\"planning\"}", &r) == 0);
    CHECK(r.status == 200);
    P("POST /v1/agents/<n>/run");
    CHECK(raw_http_request(18251, "POST", "/v1/agents/planner-agent/run",
                           "{\"task\":\"聊聊架构设计\"}", &r) == 0);
    printf("  [run] %d %.160s\n", r.status, r.body);
    P("GET /v1/blackboard");
    CHECK(raw_http_request(18251, "GET", "/v1/blackboard", NULL, &r) == 0);
    /* agent results are posted under "result:<agent>" (snapshot is {key:val}) */
    CHECK(strstr(r.body, "\"result:planner-agent\"") != NULL);

    /* plugin generation over HTTP -> persisted script exists */
    P("POST /v1/plugins/generate");
    CHECK(raw_http_request(18251, "POST", "/v1/plugins/generate",
                           "{\"description\":\"读取配置文件 plugin gen\"}", &r) == 0);
    CHECK(r.status == 200 && strstr(r.body, "\"ok\":true") != NULL);
    char gen_name[128] = "", gen_path[512] = "";
    {
        cJSON *g = cJSON_Parse(r.body);
        cJSON *p = g ? cJSON_GetObjectItemCaseSensitive(g, "plugin") : NULL;
        cJSON *n = p ? cJSON_GetObjectItemCaseSensitive(p, "name") : NULL;
        cJSON *pth = g ? cJSON_GetObjectItemCaseSensitive(g, "path") : NULL;
        if (n && cJSON_IsString(n)) snprintf(gen_name, sizeof gen_name, "%s", n->valuestring);
        if (pth && cJSON_IsString(pth)) snprintf(gen_path, sizeof gen_path, "%s", pth->valuestring);
        if (g) cJSON_Delete(g);
    }
    CHECK(gen_name[0] != '\0');
    CHECK(gen_path[0] != '\0' && coa_fs_read_file(gen_path) != NULL);

    /* generated skill runs over HTTP with args */
    {
        char body[256];
        snprintf(body, sizeof body, "{\"name\":\"%s\",\"args\":\"{}\"}", gen_name);
        P("POST /v1/skills/run");
        CHECK(raw_http_request(18251, "POST", "/v1/skills/run", body, &r) == 0);
        CHECK(r.status == 200 && strstr(r.body, "\"ok\":true") != NULL);
    }

    /* native loader error paths */
    P("POST /v1/plugins/native/load (empty)");
    CHECK(raw_http_request(18251, "POST", "/v1/plugins/native/load", "{}", &r) == 0);
    CHECK(r.status == 400 && strstr(r.body, "need 'path'") != NULL);
    P("POST /v1/plugins/native/load (missing dll)");
    CHECK(raw_http_request(18251, "POST", "/v1/plugins/native/load",
                           "{\"path\":\"build/definitely_missing.dll\"}", &r) == 0);
    CHECK(r.status == 400 && strstr(r.body, "\"ok\":false") != NULL);

    /* MCP stdio server: add -> discover tools -> found in /v1/tools */
    if (node_available()) {
        P("POST /v1/mcp (stdio)");
        CHECK(raw_http_request(18251, "POST", "/v1/mcp",
                               "{\"name\":\"mocksapi\",\"transport\":\"stdio\","
                               "\"command\":\"node\",\"args\":\"tools/mock_mcp_server.js --stdio\"}",
                               &r) == 0);
        CHECK(r.status == 200);
        int found = 0;
        for (int i = 0; i < 40 && !found; i++) {
            CHECK(raw_http_request(18251, "GET", "/v1/tools", NULL, &r) == 0);
            printf("  [tools] status=%d len=%zu hit=%d\n", r.status, r.body_len,
                   strstr(r.body, "mcp__mocksapi__echo") ? 1 : 0);
            if (strstr(r.body, "mcp__mocksapi__echo")) found = 1;
            else coa_time_sleep_ms(150);
        }
        CHECK(found);
    } else {
        printf("  (node not available, mcp stdio part skipped)\n");
    }

    coa_stop(&ctx);
    coa_thread_join(srv);
    coa_shutdown(&ctx);
    rm_tree_files(root);
}

/* ---- S5: huge files + git-managed workspace ---- */

/* total bytes of all block files under <root>/snapshots/blocks (dup storage) */
static long long blocks_bytes(const char *root) {
    char bdir[600];
    coa_path_join(bdir, sizeof bdir, root, "snapshots/blocks");
    coa_dir_list dl;
    memset(&dl, 0, sizeof dl);
    long long total = 0;
    if (coa_fs_list_dir(bdir, &dl) == 0) {
        for (size_t i = 0; i < dl.count; i++) {
            if (dl.items[i].is_dir) continue;
            char p[800];
            coa_path_join(p, sizeof p, bdir, dl.items[i].name);
            total += coa_fs_file_size(p);
            free(dl.items[i].name);
        }
        coa_fs_list_free(&dl);
    }
    return total;
}

/* free disk bytes for the volume containing `path` (gate for the 100G case) */
static long long s5_disk_free(const char *path) {
#ifdef _WIN32
    ULARGE_INTEGER avail;
    if (GetDiskFreeSpaceExA(path, &avail, NULL, NULL)) {
        return (long long)avail.QuadPart;
    }
    return 0;
#else
    struct statvfs vfs;
    if (statvfs(path, &vfs) == 0) {
        return (long long)vfs.f_bavail * (long long)vfs.f_frsize;
    }
    return 0;
#endif
}

static void s5_huge_files(void) {
    section("S5 huge files + git-managed workspace");
    const char *root = "state-scen-5";
    rm_tree_files(root);
    coa_ctx ctx;
    coa_config cfg;
    base_cfg(&cfg, root, 0);
    cfg.workspace = "state-scen-5/w";
    CHECK(coa_init(&ctx, &cfg) == 0);

    /* create 10G/20G/100G sparse files through the agent's shell tool */
    char *ans = NULL;
#ifdef _WIN32
    int rc10 = coa_run(&ctx, "执行命令 fsutil file createnew big10.bin 10737418240", &ans);
    if (rc10 != 0) printf("  [dbg big10 rc=%d] %s\n", rc10, ans ? ans : "(null)");
    CHECK(rc10 == 0);
#else
    CHECK(coa_run(&ctx, "执行命令 dd if=/dev/zero of=big10.bin bs=1 count=0 seek=10737418240", &ans) == 0);
#endif
    free(ans);
    CHECK(coa_fs_file_size("state-scen-5/w/big10.bin") == 10737418240LL);
#ifdef _WIN32
    CHECK(coa_run(&ctx, "执行命令 fsutil file createnew big20.bin 21474836480", &ans) == 0);
#else
    CHECK(coa_run(&ctx, "执行命令 dd if=/dev/zero of=big20.bin bs=1 count=0 seek=21474836480", &ans) == 0);
#endif
    free(ans);
    CHECK(coa_fs_file_size("state-scen-5/w/big20.bin") == 21474836480LL);

    /* 100G only when the disk can actually hold it (createnew allocates real
     * clusters — 10G+20G+100G needs ~130 GB free); otherwise skip gracefully */
    long long freeb = s5_disk_free(".");
    int do100 = freeb > 135LL * 1024 * 1024 * 1024;
    if (do100) {
#ifdef _WIN32
        CHECK(coa_run(&ctx, "执行命令 fsutil file createnew big100.bin 107374182400", &ans) == 0);
#else
        CHECK(coa_run(&ctx, "执行命令 dd if=/dev/zero of=big100.bin bs=1 count=0 seek=107374182400", &ans) == 0);
#endif
        free(ans);
        CHECK(coa_fs_file_size("state-scen-5/w/big100.bin") == 107374182400LL);
    } else {
        printf("  [skip] 100G case needs 130G+ free disk, only %.1f GB — skipping\n",
               (double)freeb / (1024.0 * 1024 * 1024));
    }

    /* agent overwrites each huge file: the tx pre-capture must SKIP (size
     * guard) instead of reading 10-100G into memory / duplicating to blocks */
    CHECK(coa_run(&ctx, "创建文件 big10.bin 写入内容为 tiny10", &ans) == 0);
    free(ans);
    CHECK(coa_run(&ctx, "创建文件 big20.bin 写入内容为 tiny20", &ans) == 0);
    free(ans);
    CHECK(coa_fs_file_size("state-scen-5/w/big10.bin") == 6);
    CHECK(coa_fs_file_size("state-scen-5/w/big20.bin") == 6);
    if (do100) {
        CHECK(coa_run(&ctx, "创建文件 big100.bin 写入内容为 tiny100", &ans) == 0);
        free(ans);
        CHECK(coa_fs_file_size("state-scen-5/w/big100.bin") == 7);
    }

    /* the block store never duplicated a huge file (limit: 1 MB total) */
    long long bb = blocks_bytes(root);
    printf("  [blocks] %lld bytes\n", bb);
    CHECK(bb < 1024 * 1024);

    coa_shutdown(&ctx);
    rm_tree_files(root);

    /* git-managed workspace: snapshots bypassed entirely (no blocks) */
    const char *rootg = "state-scen-5g";
    rm_tree_files(rootg);
    coa_fs_mkdirs("state-scen-5g/wg/.git"); /* workspace inside a git repo */
    coa_ctx gctx;
    base_cfg(&cfg, rootg, 0);
    cfg.workspace = "state-scen-5g/wg";
    CHECK(coa_init(&gctx, &cfg) == 0);
    CHECK(coa_run(&gctx, "创建文件 code.txt 写入内容为 git-version", &ans) == 0);
    free(ans);
    char *d = coa_fs_read_file("state-scen-5g/wg/code.txt");
    CHECK(d != NULL && strstr(d, "git-version") != NULL);
    free(d);
    CHECK(blocks_bytes(rootg) == 0); /* git is the VCS: nothing captured */
    coa_shutdown(&gctx);
    rm_tree_files(rootg);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    s1_project_workflow();
    s2_auto_evolution();
    s3_multi_agent();
    s4_http_api();
    s5_huge_files();
    printf("\nscenario: %d passed, %d failed\n", g_pass, g_fail);
    printf(g_fail == 0 ? "SCENARIO PASS\n" : "SCENARIO FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
