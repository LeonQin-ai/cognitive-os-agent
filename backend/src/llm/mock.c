/* mock.c — built-in offline LLM provider.
 * Produces a deterministic plan (JSON array of tool actions) for simple file
 * tasks so the full pipeline runs end-to-end without a model server. For any
 * other request it answers in plain text. Useful for demos and tests. */
#include "cagent/llm/llm.h"
#include "cagent/infra/util.h"
#include "cagent/infra/logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "cJSON.h"

typedef struct {
    char *model;
} mock_impl;

static mock_impl *impl_of(ca_llm *llm) { return (mock_impl *)llm->impl; }

static void mock_destroy(ca_llm *llm) {
    mock_impl *im = impl_of(llm);
    free(im->model);
    free(im);
    free(llm->base_url);
    free(llm->api_key);
    free(llm->model);
    free(llm);
}

static char *dup_n(const char *s, size_t n) {
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static int has_substr(const char *hay, const char *needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

static int is_path_char(int c) {
    return isalnum(c) || c == '.' || c == '/' || c == '\\' || c == '_' || c == '-' || c == ':';
}

/* Find a likely file path: the first whitespace-delimited token containing a
 * dot (extension). If the preceding token looked like a directory (ends in '/'),
 * prefix it. Returns malloc'd path or NULL. */
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
            return dup_n(s, plen);
        }
        if (e > s && (e[-1] == '/' || e[-1] == '\\')) { dir_s = s; dir_e = e; }
    }
    return NULL;
}

/* Extract content that follows a "write" marker in the request. */
static char *extract_content(const char *msg) {
    static const char *markers[] = {"写入内容为", "写入内容", "内容为", "内容:", "写入", "write: "};
    const char *best = NULL;
    for (size_t i = 0; i < sizeof(markers) / sizeof(char *); i++) {
        const char *hit = strstr(msg, markers[i]);
        if (hit && (!best || hit > best)) best = hit + strlen(markers[i]);
    }
    if (!best) return NULL;
    while (*best == ' ' || *best == '\t' || *best == '\n' || *best == '\r' ||
           *best == '"' || *best == '\'')
        best++;
    char *out = ca_strdup(best);
    /* stop at a task connector so multi-step prompts don't pollute the content */
    char *cut = strstr(out, "，");
    if (!cut) cut = strstr(out, "然后");
    if (!cut) cut = strstr(out, "接着");
    if (!cut) cut = strchr(out, ',');
    if (cut) *cut = '\0';
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\n' ||
                     out[n - 1] == '\r' || out[n - 1] == '"' || out[n - 1] == '\'' ||
                     out[n - 1] == '.'))
        out[--n] = '\0';
    return out;
}

/* Extract a shell command that follows a command marker. */
static char *extract_command(const char *msg) {
    static const char *markers[] = {"执行命令", "运行命令", "执行 ", "运行 ", "命令", "command", "shell"};
    const char *best = NULL;
    for (size_t i = 0; i < sizeof(markers) / sizeof(char *); i++) {
        const char *hit = strstr(msg, markers[i]);
        if (hit && (!best || hit > best)) best = hit + strlen(markers[i]);
    }
    if (!best) return NULL;
    while (*best == ' ' || *best == '\t') best++;
    char *out = ca_strdup(best);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t' ||
                     out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';
    return out;
}

/* Build the mock response for a user message. */
static char *mock_respond(const char *msg) {
    if (!msg) return ca_strdup("[]");

    int want_write = has_substr(msg, "文件") || has_substr(msg, "file") ||
                     has_substr(msg, "写") || has_substr(msg, "创建") ||
                     has_substr(msg, "生成");
    int want_read = has_substr(msg, "读取") || has_substr(msg, "读 ") ||
                    has_substr(msg, "cat ") || has_substr(msg, "read ") ||
                    has_substr(msg, "查看文件");
    int want_shell = has_substr(msg, "命令") || has_substr(msg, "command") ||
                     has_substr(msg, "shell");

    /* shell: run a command when no file operation is requested */
    if (want_shell && !want_write && !want_read) {
        char *cmd = extract_command(msg);
        cJSON *arr = cJSON_CreateArray();
        cJSON *a = cJSON_CreateObject();
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "tool", "shell");
        cJSON_AddStringToObject(args, "command", cmd ? cmd : "echo hi");
        cJSON_AddItemToObject(a, "args", args);
        cJSON_AddItemToArray(arr, a);
        char *out = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        free(cmd);
        return out ? out : ca_strdup("[]");
    }

    if (want_write || want_read) {
        char *path = find_path(msg);
        cJSON *arr = cJSON_CreateArray();

        if (want_write) {
            cJSON *a = cJSON_CreateObject();
            cJSON *args = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "tool", "file_write");
            cJSON_AddStringToObject(args, "path", path ? path : "test/note.txt");
            char *content = extract_content(msg);
            cJSON_AddStringToObject(args, "content", content ? content : "hello");
            cJSON_AddItemToObject(a, "args", args);
            cJSON_AddItemToArray(arr, a);
            free(content);
        }
        if (want_read) {
            cJSON *a = cJSON_CreateObject();
            cJSON *args = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "tool", "file_read");
            cJSON_AddStringToObject(args, "path", path ? path : "test/note.txt");
            cJSON_AddItemToObject(a, "args", args);
            cJSON_AddItemToArray(arr, a);
        }

        char *out = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        free(path);
        return out ? out : ca_strdup("[]");
    }

    ca_strbuf b;
    ca_strbuf_init(&b);
    ca_strbuf_appendf(&b, "已收到请求：%s（mock 离线模式，未调用工具）", msg);
    return ca_strbuf_detach(&b);
}

static int mock_chat(ca_llm *llm, const ca_llm_request *req, ca_llm_response *resp) {
    (void)llm;
    const char *last = req->num_messages ? req->messages[req->num_messages - 1].content : "";
    resp->content = mock_respond(last);
    return 0;
}

static int mock_stream(ca_llm *llm, const ca_llm_request *req, ca_llm_stream_cb cb, void *ud) {
    (void)llm;
    const char *last = req->num_messages ? req->messages[req->num_messages - 1].content : "";
    char *text = mock_respond(last);
    if (!text) return -1;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i += 16) cb(text + i, ud);
    free(text);
    return 0;
}

ca_llm *ca_mock_create(const char *model) {
    ca_llm *llm = calloc(1, sizeof(ca_llm));
    mock_impl *im = calloc(1, sizeof(mock_impl));
    if (!llm || !im) { free(llm); free(im); return NULL; }
    static const ca_llm_vtable vt = {mock_destroy, mock_chat, mock_stream};
    llm->vt = &vt;
    llm->provider = ca_strdup("mock");
    llm->model = ca_strdup(model ? model : "mock");
    llm->impl = im;
    im->model = ca_strdup(llm->model);
    ca_log_info("mock llm provider ready (model=%s)", im->model);
    return llm;
}
