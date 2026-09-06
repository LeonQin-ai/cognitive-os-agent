/* mock.c — built-in offline LLM provider.
 * Produces a deterministic plan (JSON array of tool actions) for simple file
 * tasks so the full pipeline runs end-to-end without a model server. For any
 * other request it answers in plain text. Useful for demos and tests. */
#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/infra/logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "cJSON.h"

typedef struct {
    char *model;
} mock_impl;

static mock_impl *impl_of(coa_llm *llm) { return (mock_impl *)llm->impl; }

static void mock_destroy(coa_llm *llm) {
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
    char *out = coa_strdup(best);
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

/* Extract the argument that follows a command marker. Markers are checked in
 * priority order (longer/specific first) and the FIRST marker present wins;
 * a generic marker like "shell" must not win just because it appears later
 * in the text (e.g. inside "echo scen-shell-ok"). */
static char *extract_command(const char *msg) {
    static const char *markers[] = {"执行命令", "运行命令", "执行 ", "运行 ", "命令",
                                    "搜索", "查找文件", "command", "shell"};
    const char *best = NULL;
    for (size_t i = 0; i < sizeof(markers) / sizeof(char *); i++) {
        const char *hit = strstr(msg, markers[i]);
        if (hit) { best = hit + strlen(markers[i]); break; }
    }
    if (!best) return NULL;
    while (*best == ' ' || *best == '\t') best++;
    char *out = coa_strdup(best);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t' ||
                     out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';
    return out;
}

/* Build the mock response for a user message. */
static char *mock_respond(const char *msg) {
    if (!msg) return coa_strdup("[]");

    /* multi-agent orchestration: the decompose prompt lists the roster under
     * "可用 agent"; the merge prompt aggregates under "各 agent 结果" */
    if (has_substr(msg, "可用 agent") || has_substr(msg, "可用agent")) {
        return coa_strdup("[{\"agent\":\"alpha\","
                         "\"task\":\"创建 orch.txt 写入内容为 orch-ok\"}]");
    }
    if (has_substr(msg, "各 agent 结果"))
        return coa_strdup("综合完成：子任务已由各 agent 协作处理完毕。");

    /* When driven through the full reasoning runtime the "message" is the
     * whole augmented planner prompt (session notes, history, journal).
     * Keyword decisions must be made on the CURRENT REQUEST only — the
     * journal lines repeat earlier request texts and would otherwise
     * hijack the plan. */
    /* Agent-loop rounds: the augmented prompt carries results of previously
     * executed rounds before "## Current request". Round >= 2 defaults to a
     * plain-text answer (task complete) so single-action requests do not
     * repeat their actions; a "修复" request that has not yet been fixed gets
     * one file_edit round before finishing. */
    int in_loop = strstr(msg, "## 之前轮次的动作结果") != NULL;
    const char *full = msg; /* full augmented prompt (results live here) */
    const char *cur = strstr(msg, "## Current request\n");
    if (cur) msg = cur + strlen("## Current request\n");

    if (in_loop) {
        char *path = find_path(msg);
        /* fix marker checked only inside this run's results section — the
         * conversation history may carry old answers with the same text */
        const char *fix_hit = strstr(full, "[file_edit] ok");
        int fixed_this_run = fix_hit && cur && fix_hit < cur;
        if (has_substr(msg, "修复") && !fixed_this_run) {
            /* the previous round read/analyzed; now apply the fix */
            cJSON *arr = cJSON_CreateArray();
            cJSON *a = cJSON_CreateObject();
            cJSON *args = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "tool", "file_edit");
            cJSON_AddStringToObject(args, "path", path ? path : "a.txt");
            cJSON_AddStringToObject(args, "old_string", "OLD");
            cJSON_AddStringToObject(args, "new_string", "NEW");
            cJSON_AddItemToObject(a, "args", args);
            cJSON_AddItemToArray(arr, a);
            char *out = cJSON_PrintUnformatted(arr);
            cJSON_Delete(arr);
            free(path);
            return out ? out : coa_strdup("[]");
        }
        free(path);
        return coa_strdup("任务完成。"); /* plain text = final answer */
    }

    /* forced final synthesis after budget exhaustion (reasoning.c): the user
     * message embeds the observation log tail — reply with a synthesis, not a
     * new plan */
    if (strstr(msg, "已执行动作的观察记录"))
        return coa_strdup("综合回答：基于已收集的观察信息，任务已部分完成。");

    /* auto-evolution drill: plan a tool that is NOT in the registry so the
     * reasoning layer exercises the missing-capability generation loop */
    if (has_substr(msg, "天气")) {
        cJSON *arr = cJSON_CreateArray();
        cJSON *a = cJSON_CreateObject();
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "tool", "weather_lookup");
        cJSON_AddStringToObject(args, "city", "北京");
        cJSON_AddItemToObject(a, "args", args);
        cJSON_AddItemToArray(arr, a);
        char *out = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        return out ? out : coa_strdup("[]");
    }

    int want_write = has_substr(msg, "文件") || has_substr(msg, "file") ||
                     has_substr(msg, "写") || has_substr(msg, "创建") ||
                     has_substr(msg, "生成");
    int want_read = has_substr(msg, "读取") || has_substr(msg, "读 ") ||
                    has_substr(msg, "cat ") || has_substr(msg, "read ") ||
                    has_substr(msg, "查看文件");
    int want_shell = has_substr(msg, "命令") || has_substr(msg, "command") ||
                     has_substr(msg, "shell");
    /* an explicit "execute this command" request wins over generic keywords
     * (e.g. "fsutil file createnew" contains "file" but is not a file op) */
    int explicit_cmd = has_substr(msg, "执行命令") || has_substr(msg, "运行命令");

    /* analyze-first: an 分析 request starts the loop with a read so the fix
     * is applied on a later round with the observation in context */
    if (has_substr(msg, "分析")) {
        char *path = find_path(msg);
        cJSON *arr = cJSON_CreateArray();
        cJSON *a = cJSON_CreateObject();
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "tool", "file_read");
        cJSON_AddStringToObject(args, "path", path ? path : ".");
        cJSON_AddItemToObject(a, "args", args);
        cJSON_AddItemToArray(arr, a);
        char *out = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        free(path);
        return out ? out : coa_strdup("[]");
    }

    /* file_edit: replace OLD with NEW in a file (deterministic mock strings) */
    if (has_substr(msg, "编辑") || has_substr(msg, "替换")) {
        char *path = find_path(msg);
        cJSON *arr = cJSON_CreateArray();
        cJSON *a = cJSON_CreateObject();
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "tool", "file_edit");
        cJSON_AddStringToObject(args, "path", path ? path : "note.txt");
        cJSON_AddStringToObject(args, "old_string", "OLD");
        cJSON_AddStringToObject(args, "new_string", "NEW");
        cJSON_AddItemToObject(a, "args", args);
        cJSON_AddItemToArray(arr, a);
        char *out = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        free(path);
        return out ? out : coa_strdup("[]");
    }

    /* grep: search file contents for the text after the marker */
    if (has_substr(msg, "搜索")) {
        char *pat = extract_command(msg);
        cJSON *arr = cJSON_CreateArray();
        cJSON *a = cJSON_CreateObject();
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "tool", "grep");
        cJSON_AddStringToObject(args, "pattern", pat && *pat ? pat : "hello");
        cJSON_AddStringToObject(args, "output_mode", "content");
        cJSON_AddItemToObject(a, "args", args);
        cJSON_AddItemToArray(arr, a);
        char *out = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        free(pat);
        return out ? out : coa_strdup("[]");
    }

    /* glob: find files matching the pattern after the marker */
    if (has_substr(msg, "查找文件")) {
        char *pat = extract_command(msg);
        cJSON *arr = cJSON_CreateArray();
        cJSON *a = cJSON_CreateObject();
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "tool", "glob");
        cJSON_AddStringToObject(args, "pattern", pat && *pat ? pat : "*.txt");
        cJSON_AddItemToObject(a, "args", args);
        cJSON_AddItemToArray(arr, a);
        char *out = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        free(pat);
        return out ? out : coa_strdup("[]");
    }

    /* shell: run a command when no file operation is requested (an explicit
     * 执行命令/运行命令 request always routes here) */
    if (want_shell && (!want_write || explicit_cmd) && !want_read) {
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
        return out ? out : coa_strdup("[]");
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
        return out ? out : coa_strdup("[]");
    }

    coa_strbuf b;
    coa_strbuf_init(&b);
    coa_strbuf_appendf(&b, "已收到请求：%s（mock 离线模式，未调用工具）", msg);
    return coa_strbuf_detach(&b);
}

static int mock_chat(coa_llm *llm, const coa_llm_request *req, coa_llm_response *resp) {
    (void)llm;
    const char *last = req->num_messages ? req->messages[req->num_messages - 1].content : "";
    resp->content = mock_respond(last);
    return 0;
}

static int mock_stream(coa_llm *llm, const coa_llm_request *req, coa_llm_stream_cb cb, void *ud) {
    const char *last = req->num_messages ? req->messages[req->num_messages - 1].content : "";
    char *text = mock_respond(last);
    if (!text) return -1;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i += 16) {
        if (llm->cancel) { free(text); return -1; }
        cb(text + i, ud);
    }
    free(text);
    return 0;
}

coa_llm *coa_mock_create(const char *model) {
    coa_llm *llm = calloc(1, sizeof(coa_llm));
    mock_impl *im = calloc(1, sizeof(mock_impl));
    if (!llm || !im) { free(llm); free(im); return NULL; }
    static const coa_llm_vtable vt = {mock_destroy, mock_chat, mock_stream};
    llm->vt = &vt;
    llm->provider = coa_strdup("mock");
    llm->model = coa_strdup(model ? model : "mock");
    llm->impl = im;
    im->model = coa_strdup(llm->model);
    coa_log_info("mock llm provider ready (model=%s)", im->model);
    return llm;
}
