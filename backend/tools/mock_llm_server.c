/* mock_llm_server.c — standalone OpenAI-compatible mock LLM HTTP server.
 * Serves POST /v1/chat/completions (chat + SSE streaming) with a deterministic
 * plan derived from the last user message, so the full c-agent pipeline can be
 * exercised end-to-end against a *real* HTTP LLM endpoint.
 *
 *   ./build/mock-llm-server 9000          (or tools/mock_llm_server on Linux)
 *
 * Then run:  cagent run "创建 test/a.txt 写入内容为 hello"  with provider=openai
 *            base_url=http://localhost:9000
 */
#include "cagent/api/http_server.h"
#include "cagent/os/os_socket.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "cJSON.h"

/* ---------- prompt analysis (mirrors src/llm/mock.c) ---------- */
static int is_path_char(int c) {
    return isalnum(c) || c == '.' || c == '/' || c == '\\' || c == '_' || c == '-' || c == ':';
}

static char *find_path(const char *msg) {
    const char *p = msg;
    const char *dir_s = NULL, *dir_e = NULL;
    while (*p) {
        while (*p && !is_path_char(*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && is_path_char(*p)) p++;
        const char *e = p;
        int has_dot = 0;
        for (const char *q = s; q < e; q++) if (*q == '.') { has_dot = 1; break; }
        if (has_dot) {
            size_t plen = (size_t)(e - s);
            if (dir_s && dir_e == s) {
                size_t dlen = (size_t)(dir_e - dir_s);
                char *out = malloc(dlen + plen + 1);
                memcpy(out, dir_s, dlen);
                memcpy(out + dlen, s, plen);
                out[dlen + plen] = '\0';
                return out;
            }
            char *out = malloc(plen + 1);
            memcpy(out, s, plen);
            out[plen] = '\0';
            return out;
        }
        if (e > s && (e[-1] == '/' || e[-1] == '\\')) { dir_s = s; dir_e = e; }
    }
    return NULL;
}

static char *extract_content(const char *msg) {
    static const char *markers[] = {"写入内容为", "写入内容", "内容为", "内容:", "写入", "write: "};
    const char *best = NULL;
    for (size_t i = 0; i < sizeof(markers) / sizeof(char *); i++) {
        const char *hit = strstr(msg, markers[i]);
        if (hit && (!best || hit > best)) best = hit + strlen(markers[i]);
    }
    if (!best) return NULL;
    while (*best == ' ' || *best == '\t' || *best == '\n' || *best == '\r')
        best++;
    char *out = ca_strdup(best);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\n' ||
                     out[n - 1] == '\r'))
        out[--n] = '\0';
    return out;
}

/* Produce the JSON text that a real planner LLM would emit: a JSON array of
 * {"tool": "...", "args": {...}} for file tasks, else a short prose answer.
 * Multi-round aware: the reasoning loop feeds previous-round action results
 * back under "## 之前轮次的动作结果"; round >= 2 finishes with a prose answer
 * unless a 修复 request has not yet seen its file_edit applied. */
static char *plan_for(const char *msg) {
    if (!msg) return ca_strdup("[]");
    const char *cur = strstr(msg, "## Current request\n");
    const char *req = cur ? cur + strlen("## Current request\n") : msg;
    const char *loop_sec = strstr(msg, "## 之前轮次的动作结果");
    if (loop_sec) {
        /* check the fix marker only inside THIS run's results section — the
         * conversation history may contain old answers with the same text */
        const char *fix_hit = strstr(loop_sec, "[file_edit] ok");
        if (strstr(req, "修复") && !(fix_hit && cur && fix_hit < cur)) {
            char *fpath = find_path(req);
            cJSON *arr = cJSON_CreateArray();
            cJSON *a = cJSON_CreateObject();
            cJSON *args = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "tool", "file_edit");
            cJSON_AddStringToObject(args, "path", fpath ? fpath : "a.txt");
            cJSON_AddStringToObject(args, "old_string", "OLD");
            cJSON_AddStringToObject(args, "new_string", "NEW");
            cJSON_AddItemToObject(a, "args", args);
            cJSON_AddItemToArray(arr, a);
            char *out = cJSON_PrintUnformatted(arr);
            cJSON_Delete(arr);
            free(fpath);
            return out ? out : ca_strdup("[]");
        }
        /* plain-text answer; contains "ok" so the e2e assertion keeps passing */
        return ca_strdup("ok: task complete / 任务完成。");
    }
    int want_write = strstr(msg, "文件") || strstr(msg, "file") ||
                     strstr(msg, "写") || strstr(msg, "创建") || strstr(msg, "生成");
    int want_read  = strstr(msg, "读取") || strstr(msg, "cat ") || strstr(msg, "read ");
    char *path = find_path(msg);
    if (strstr(req, "分析")) {
        cJSON *arr = cJSON_CreateArray();
        cJSON *a = cJSON_CreateObject();
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "tool", "file_read");
        cJSON_AddStringToObject(args, "path", path ? path : "a.txt");
        cJSON_AddItemToObject(a, "args", args);
        cJSON_AddItemToArray(arr, a);
        char *out = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        free(path);
        return out ? out : ca_strdup("[]");
    }
    if (want_write || want_read) {
        cJSON *arr = cJSON_CreateArray();
        cJSON *a = cJSON_CreateObject();
        cJSON *args = cJSON_CreateObject();
        if (want_write) {
            cJSON_AddStringToObject(a, "tool", "file_write");
            cJSON_AddStringToObject(args, "path", path ? path : "test/note.txt");
            char *content = extract_content(msg);
            cJSON_AddStringToObject(args, "content", content ? content : "hello");
            free(content);
        } else {
            cJSON_AddStringToObject(a, "tool", "file_read");
            cJSON_AddStringToObject(args, "path", path ? path : "test/note.txt");
        }
        cJSON_AddItemToObject(a, "args", args);
        cJSON_AddItemToArray(arr, a);
        char *out = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        free(path);
        return out ? out : ca_strdup("[]");
    }
    free(path);
    return ca_strdup("{\"answer\":\"ok\"}");
}

/* ---------- HTTP handler ---------- */
static char *body_str(const ca_http_request *req) {
    if (!req->body || req->body_len == 0) return ca_strdup("");
    char *s = malloc(req->body_len + 1);
    if (!s) return NULL;
    memcpy(s, req->body, req->body_len);
    s[req->body_len] = '\0';
    return s;
}

static int h_chat(const ca_http_request *req, ca_http_response *resp, void *ud) {
    (void)ud;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);

    const char *last = "hello";
    int stream = 0;
    if (root && cJSON_IsObject(root)) {
        cJSON *msgs = cJSON_GetObjectItemCaseSensitive(root, "messages");
        cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "stream");
        if (st) stream = cJSON_IsTrue(st);
        if (msgs && cJSON_IsArray(msgs)) {
            cJSON *it = msgs->child;
            const char *content = NULL;
            while (it) {
                cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "content");
                if (c && cJSON_IsString(c)) content = c->valuestring;
                it = it->next;
            }
            if (content) last = content;
        }
    }
    char *plan = plan_for(last);
    cJSON_Delete(root);

    char *escaped = NULL;
    {
        /* JSON-escape the plan so it embeds as a string value */
        cJSON *tmp = cJSON_CreateString(plan);
        escaped = cJSON_PrintUnformatted(tmp);
        cJSON_Delete(tmp);
    }

    if (stream) {
        /* SSE stream: one complete delta frame + done terminator */
        ca_strbuf body;
        ca_strbuf_init(&body);
        char buf[8192];
        snprintf(buf, sizeof(buf),
                 "data: {\"choices\":[{\"delta\":{\"content\":%s}}]}\n\n"
                 "data: [DONE]\n\n",
                 escaped ? escaped : "\"\"");
        ca_strbuf_append(&body, buf);
        snprintf(resp->content_type, sizeof(resp->content_type), "text/event-stream");
        /* Content-Length + Connection: close still works with ca_sse's line reader */
        ca_http_resp_append(resp, body.buf);
        ca_strbuf_free(&body);
    } else {
        ca_http_resp_appendf(resp,
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":%s}}]}",
            escaped ? escaped : "\"[]\"");
    }
    free(escaped);
    free(plan);
    return 0;
}

static volatile int g_stop = 0;

/* Anthropic-style response: {"content":[{"type":"text","text":<plan>}]} */
static int h_messages(const ca_http_request *req, ca_http_response *resp, void *ud) {
    (void)ud;
    char *b = body_str(req);
    cJSON *root = b ? cJSON_Parse(b) : NULL;
    free(b);

    const char *last = "hello";
    int stream = 0;
    if (root && cJSON_IsObject(root)) {
        cJSON *msgs = cJSON_GetObjectItemCaseSensitive(root, "messages");
        cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "stream");
        if (st) stream = cJSON_IsTrue(st);
        if (msgs && cJSON_IsArray(msgs)) {
            cJSON *it = msgs->child;
            const char *content = NULL;
            while (it) {
                cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "content");
                if (c && cJSON_IsString(c)) content = c->valuestring;
                it = it->next;
            }
            if (content) last = content;
        }
    }
    char *plan = plan_for(last);
    cJSON_Delete(root);

    char *escaped = NULL;
    {
        cJSON *tmp = cJSON_CreateString(plan);
        escaped = cJSON_PrintUnformatted(tmp);
        cJSON_Delete(tmp);
    }

    if (stream) {
        /* SSE with content_block_delta frames (what anthropic_stream expects) */
        ca_strbuf body;
        ca_strbuf_init(&body);
        char buf[8192];
        snprintf(buf, sizeof(buf),
                 "event: content_block_delta\n"
                 "data: {\"delta\":{\"type\":\"text_delta\",\"text\":%s}}\n\n"
                 "data: [DONE]\n\n",
                 escaped ? escaped : "\"\"");
        ca_strbuf_append(&body, buf);
        snprintf(resp->content_type, sizeof(resp->content_type), "text/event-stream");
        ca_http_resp_append(resp, body.buf);
        ca_strbuf_free(&body);
    } else {
        ca_http_resp_appendf(resp,
            "{\"content\":[{\"type\":\"text\",\"text\":%s}]}", escaped ? escaped : "\"[]\"");
    }
    free(escaped);
    free(plan);
    return 0;
}

int main(int argc, char **argv) {
    uint16_t port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 9000);
    if (ca_sock_init() != 0) { fprintf(stderr, "sock init failed\n"); return 1; }
    ca_http_server *s = ca_http_server_new(port);
    if (!s) { fprintf(stderr, "listen failed on %u\n", (unsigned)port); return 1; }
    ca_http_server_route(s, "POST", "/v1/chat/completions", h_chat, NULL);
    ca_http_server_route(s, "POST", "/v1/messages", h_messages, NULL);
    /* Volcengine Ark Coding Plan style paths (base_url already ends in /vN,
     * so the adapter appends only /chat/completions). */
    ca_http_server_route(s, "POST", "/api/coding/v3/chat/completions", h_chat, NULL);
    ca_http_server_route(s, "POST", "/api/coding/v1/messages", h_messages, NULL);
    printf("mock LLM server on http://localhost:%u/v1/chat/completions\n", (unsigned)port);
    fflush(stdout);
    (void)g_stop;
    ca_http_server_serve(s);
    ca_http_server_free(s);
    ca_sock_cleanup();
    return 0;
}
