/* tool_file.c — file_read and file_write tools. */
#include "cagent/action/tools.h"
#include "cagent/os/os_fs.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

/* Resolve a (possibly relative) path against the workspace. Caller frees. */
static char *resolve_path(const ca_tool_ctx *ctx, const char *path) {
    char full[2048];
    ca_path_resolve(full, sizeof(full), ctx ? ctx->workspace : NULL, path ? path : "");
    return ca_strdup(full);
}

static ca_tool_result *file_read_exec(const ca_tool *self, const ca_tool_ctx *ctx, const char *args_json) {
    (void)self;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return ca_tool_result_new(0, "file_read: invalid args JSON");
    cJSON *path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (!path_j || !cJSON_IsString(path_j)) {
        cJSON_Delete(args);
        return ca_tool_result_new(0, "file_read: missing string arg 'path'");
    }
    char *rp = resolve_path(ctx, path_j->valuestring);
    /* Reading a directory is common (the planner often probes a path before
     * reading a file). Instead of failing, return a listing so the agent can
     * continue — a single mis-targeted file_read must not fail the whole task. */
    if (ca_fs_is_dir(rp)) {
        ca_dir_list dl;
        memset(&dl, 0, sizeof(dl));
        if (ca_fs_list_dir(rp, &dl) == 0) {
            ca_strbuf sb;
            ca_strbuf_init(&sb);
            ca_strbuf_appendf(&sb, "Directory listing of %s:\n", rp);
            for (size_t i = 0; i < dl.count; i++) {
                ca_strbuf_appendf(&sb, "%s%s\n", dl.items[i].name,
                                  dl.items[i].is_dir ? "/" : "");
            }
            ca_fs_list_free(&dl);
            char *out = ca_strbuf_detach(&sb);
            free(rp);
            cJSON_Delete(args);
            ca_tool_result *r = ca_tool_result_new(1, out);
            free(out);
            return r;
        }
        ca_fs_list_free(&dl);
    }
    char *content = ca_fs_read_file(rp);
    cJSON_Delete(args);
    if (!content) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "file_read: cannot read %s", rp);
        free(rp);
        return ca_tool_result_new(0, msg);
    }
    free(rp);
    ca_tool_result *r = ca_tool_result_new(1, content);
    free(content);
    return r;
}

static ca_tool_result *file_write_exec(const ca_tool *self, const ca_tool_ctx *ctx, const char *args_json) {
    (void)self;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return ca_tool_result_new(0, "file_write: invalid args JSON");
    cJSON *path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    cJSON *content_j = cJSON_GetObjectItemCaseSensitive(args, "content");
    if (!path_j || !cJSON_IsString(path_j)) {
        cJSON_Delete(args);
        return ca_tool_result_new(0, "file_write: missing string arg 'path'");
    }
    const char *content = (content_j && cJSON_IsString(content_j)) ? content_j->valuestring : "";

    char *rp = resolve_path(ctx, path_j->valuestring);
    /* ensure parent dir */
    char *slash = strrchr(rp, '/');
#if defined(_WIN32)
    char *bslash = strrchr(rp, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash) {
        char dir[2048];
        size_t n = (size_t)(slash - rp);
        if (n > 0) {
            snprintf(dir, sizeof(dir), "%.*s", (int)n, rp);
            ca_fs_mkdirs(dir);
        }
    }

    size_t content_len = strlen(content);
    int w = ca_fs_write_file(rp, content, content_len);
    cJSON_Delete(args);
    if (w != 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "file_write: failed to write %s", rp);
        free(rp);
        return ca_tool_result_new(0, msg);
    }
    char out[1200];
    snprintf(out, sizeof(out), "wrote %zu bytes to %s", content_len, rp);
    free(rp);
    return ca_tool_result_new(1, out);
}

/* file_edit — exact string replacement (ported from Claude Code FileEditTool):
 * fails when old_string is absent, or when it occurs more than once unless
 * replace_all is set. */
static size_t count_occurrences(const char *hay, const char *needle) {
    size_t n = 0;
    size_t nl = strlen(needle);
    if (nl == 0) return 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { n++; p += nl; }
    return n;
}

static ca_tool_result *file_edit_exec(const ca_tool *self, const ca_tool_ctx *ctx, const char *args_json) {
    (void)self;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return ca_tool_result_new(0, "file_edit: invalid args JSON");
    cJSON *path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    cJSON *old_j = cJSON_GetObjectItemCaseSensitive(args, "old_string");
    cJSON *new_j = cJSON_GetObjectItemCaseSensitive(args, "new_string");
    if (!path_j || !cJSON_IsString(path_j) || !old_j || !cJSON_IsString(old_j) ||
        !new_j || !cJSON_IsString(new_j)) {
        cJSON_Delete(args);
        return ca_tool_result_new(0, "file_edit: requires string args 'path', 'old_string', 'new_string'");
    }
    int replace_all = 0;
    cJSON *ra_j = cJSON_GetObjectItemCaseSensitive(args, "replace_all");
    if (ra_j && cJSON_IsTrue(ra_j)) replace_all = 1;

    /* copy strings out before cJSON_Delete(args) frees the tree */
    char *old_s = ca_strdup(old_j->valuestring);
    char *new_s = ca_strdup(new_j->valuestring);
    char *rp = resolve_path(ctx, path_j->valuestring);
    char *content = ca_fs_read_file(rp);
    cJSON_Delete(args);
    if (!content) {
        char msg[1200];
        snprintf(msg, sizeof(msg),
                 "file_edit: file does not exist: %s (use file_write to create it)", rp);
        free(rp); free(old_s); free(new_s);
        return ca_tool_result_new(0, msg);
    }
    size_t old_len = strlen(old_s);
    size_t occ = count_occurrences(content, old_s);
    if (occ == 0) {
        char msg[1200];
        snprintf(msg, sizeof(msg),
                 "file_edit: old_string not found in %s (the edit will fail if old_string does not match exactly, including whitespace)", rp);
        free(content); free(rp); free(old_s); free(new_s);
        return ca_tool_result_new(0, msg);
    }
    if (occ > 1 && !replace_all) {
        char msg[1200];
        snprintf(msg, sizeof(msg),
                 "file_edit: old_string is not unique in %s (%zu occurrences). Provide a larger string with more surrounding context to make it unique, or use replace_all to change every instance",
                 rp, occ);
        free(content); free(rp); free(old_s); free(new_s);
        return ca_tool_result_new(0, msg);
    }
    ca_strbuf sb;
    ca_strbuf_init(&sb);
    const char *p = content;
    while (*p) {
        const char *hit = strstr(p, old_s);
        if (!hit) { ca_strbuf_append(&sb, p); break; }
        ca_strbuf_append_n(&sb, p, (size_t)(hit - p));
        ca_strbuf_append(&sb, new_s);
        p = hit + old_len;
    }
    int w = ca_fs_write_file(rp, sb.buf ? sb.buf : "", sb.len);
    free(sb.buf);
    free(content);
    free(old_s);
    free(new_s);
    if (w != 0) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "file_edit: failed to write %s", rp);
        free(rp);
        return ca_tool_result_new(0, msg);
    }
    char out[1240];
    snprintf(out, sizeof(out), "edited %s (%zu replacement%s)", rp, occ, occ == 1 ? "" : "s");
    free(rp);
    return ca_tool_result_new(1, out);
}

const ca_tool *ca_tool_file_read(void) {
    static const ca_tool t = {
        "file_read",
        "Read a file's content. If the path is a DIRECTORY, returns its listing instead of failing.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
        0,
        file_read_exec,
        NULL,
    };
    return &t;
}

const ca_tool *ca_tool_file_write(void) {
    static const ca_tool t = {
        "file_write",
        "Write content to a file (creates parent directories).",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}}}",
        1,
        file_write_exec,
        NULL,
    };
    return &t;
}

const ca_tool *ca_tool_file_edit(void) {
    static const ca_tool t = {
        "file_edit",
        "Performs exact string replacements in files. ALWAYS prefer editing existing files; "
        "NEVER write new files unless required. The edit FAILS if old_string is not unique in the "
        "file - either provide a larger string with more surrounding context to make it unique, or "
        "use replace_all to change every instance of old_string.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"},"
        "\"replace_all\":{\"type\":\"boolean\"}},"
        "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        1,
        file_edit_exec,
        NULL,
    };
    return &t;
}
