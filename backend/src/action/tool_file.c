/* tool_file.c — file_read and file_write tools. */
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

/* Resolve a (possibly relative) path against the workspace. Caller frees. */
static char *resolve_path(const tool_ctx *ctx, const char *path) {
    char full[2048];
    path_resolve(full, sizeof(full), ctx ? ctx->workspace : NULL, path ? path : "");
    return xstrdup(full);
}

static tool_result *file_read_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args;
    cJSON *path_j;
    char *rp;
    char *content;
    tool_result *r;

    (void)self;
    args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_new(0, "file_read: invalid args JSON");
    path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (!path_j || !cJSON_IsString(path_j)) {
        cJSON_Delete(args);
        return tool_result_new(0, "file_read: missing string arg 'path'");
    }

    rp = resolve_path(ctx, path_j->valuestring);
    /* Reading a directory is common (the planner often probes a path before
     * reading a file). Instead of failing, return a listing so the agent can
     * continue — a single mis-targeted file_read must not fail the whole task. */
    if (fs_is_dir(rp)) {
        dir_list dl;
        memset(&dl, 0, sizeof(dl));
        if (fs_list_dir(rp, &dl) == 0) {
            strbuf sb;
            strbuf_init(&sb);
            strbuf_appendf(&sb, "Directory listing of %s:\n", rp);
            for (size_t i = 0; i < dl.count; i++) {
                strbuf_appendf(&sb, "%s%s\n", dl.items[i].name, dl.items[i].is_dir ? "/" : "");
            }
            fs_list_free(&dl);
            char *out = strbuf_detach(&sb);
            free(rp);
            cJSON_Delete(args);
            tool_result *r = tool_result_new(1, out);
            free(out);
            return r;
        }
        fs_list_free(&dl);
    }

    content = fs_read_file(rp);
    cJSON_Delete(args);
    if (!content) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "file_read: cannot read %s", rp);
        free(rp);
        return tool_result_new(0, msg);
    }

    free(rp);
    r = tool_result_new(1, content);
    free(content);
    return r;
}

static tool_result *file_write_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args;
    cJSON *path_j;
    cJSON *content_j;
    char *rp;
    /* ensure parent dir */
    char *slash;
    size_t content_len;
    int w;
    char out[1200];

    (void)self;
    args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_new(0, "file_write: invalid args JSON");
    path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    content_j = cJSON_GetObjectItemCaseSensitive(args, "content");
    if (!path_j || !cJSON_IsString(path_j)) {
        cJSON_Delete(args);
        return tool_result_new(0, "file_write: missing string arg 'path'");
    }

    const char *content = (content_j && cJSON_IsString(content_j)) ? content_j->valuestring : "";

    rp = resolve_path(ctx, path_j->valuestring);
    /* ensure parent dir */
    slash = strrchr(rp, '/');
#if defined(_WIN32)
    char *bslash = strrchr(rp, '\\');
    if (bslash && (!slash || bslash > slash))
        slash = bslash;
#endif
    if (slash) {
        char dir[2048];
        size_t n = (size_t)(slash - rp);
        if (n > 0) {
            snprintf(dir, sizeof(dir), "%.*s", (int)n, rp);
            fs_mkdirs(dir);
        }
    }

    content_len = strlen(content);
    w = fs_write_file(rp, content, content_len);
    cJSON_Delete(args);
    if (w != 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "file_write: failed to write %s", rp);
        free(rp);
        return tool_result_new(0, msg);
    }

    snprintf(out, sizeof(out), "wrote %zu bytes to %s", content_len, rp);
    free(rp);
    return tool_result_new(1, out);
}

/* file_edit — exact string replacement (ported from Claude Code FileEditTool):
 * fails when old_string is absent, or when it occurs more than once unless
 * replace_all is set. */
static size_t count_occurrences(const char *hay, const char *needle) {
    size_t n = 0;
    size_t nl = strlen(needle);
    const char *p = hay;

    if (nl == 0)
        return 0;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += nl;
    }

    return n;
}

static tool_result *file_edit_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args;
    cJSON *path_j;
    cJSON *old_j;
    cJSON *new_j;
    int replace_all = 0;
    cJSON *ra_j;
    /* copy strings out before cJSON_Delete(args) frees the tree */
    char *old_s;
    char *new_s;
    char *rp;
    char *content;
    size_t old_len;
    size_t occ;
    strbuf sb;
    int w;
    char out[1240];

    (void)self;
    args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_new(0, "file_edit: invalid args JSON");
    path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    old_j = cJSON_GetObjectItemCaseSensitive(args, "old_string");
    new_j = cJSON_GetObjectItemCaseSensitive(args, "new_string");
    if (!path_j || !cJSON_IsString(path_j) || !old_j || !cJSON_IsString(old_j) || !new_j || !cJSON_IsString(new_j)) {
        cJSON_Delete(args);
        return tool_result_new(0, "file_edit: requires string args 'path', 'old_string', 'new_string'");
    }

    ra_j = cJSON_GetObjectItemCaseSensitive(args, "replace_all");
    if (ra_j && cJSON_IsTrue(ra_j))
        replace_all = 1;

    /* copy strings out before cJSON_Delete(args) frees the tree */
    old_s = xstrdup(old_j->valuestring);
    new_s = xstrdup(new_j->valuestring);
    rp = resolve_path(ctx, path_j->valuestring);
    content = fs_read_file(rp);
    cJSON_Delete(args);
    if (!content) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "file_edit: file does not exist: %s (use file_write to create it)", rp);
        free(rp);
        free(old_s);
        free(new_s);
        return tool_result_new(0, msg);
    }

    old_len = strlen(old_s);
    occ = count_occurrences(content, old_s);
    if (occ == 0) {
        char msg[1200];
        snprintf(msg, sizeof(msg),
                 "file_edit: old_string not found in %s (the edit will fail if old_string does not match exactly, "
                 "including whitespace)",
                 rp);
        free(content);
        free(rp);
        free(old_s);
        free(new_s);
        return tool_result_new(0, msg);
    }

    if (occ > 1 && !replace_all) {
        char msg[1200];
        snprintf(msg, sizeof(msg),
                 "file_edit: old_string is not unique in %s (%zu occurrences). Provide a larger string with more "
                 "surrounding context to make it unique, or use replace_all to change every instance",
                 rp, occ);
        free(content);
        free(rp);
        free(old_s);
        free(new_s);
        return tool_result_new(0, msg);
    }

    strbuf_init(&sb);
    const char *p = content;
    while (*p) {
        const char *hit = strstr(p, old_s);
        if (!hit) {
            strbuf_append(&sb, p);
            break;
        }
        strbuf_append_n(&sb, p, (size_t)(hit - p));
        strbuf_append(&sb, new_s);
        p = hit + old_len;
    }

    w = fs_write_file(rp, sb.buf ? sb.buf : "", sb.len);
    free(sb.buf);
    free(content);
    free(old_s);
    free(new_s);
    if (w != 0) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "file_edit: failed to write %s", rp);
        free(rp);
        return tool_result_new(0, msg);
    }

    snprintf(out, sizeof(out), "edited %s (%zu replacement%s)", rp, occ, occ == 1 ? "" : "s");
    free(rp);
    return tool_result_new(1, out);
}

const tool *tool_file_read(void) {
    static const tool t = {
        "file_read",
        "Read a file's content. If the path is a DIRECTORY, returns its listing instead of failing.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
        0,
        file_read_exec,
        NULL,
    };
    return &t;
}

const tool *tool_file_write(void) {
    static const tool t = {
        "file_write",
        "Write content to a file (creates parent directories).",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}}}",
        1,
        file_write_exec,
        NULL,
    };
    return &t;
}

const tool *tool_file_edit(void) {
    static const tool t = {
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
