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

static ca_tool_result *file_read_exec(const ca_tool_ctx *ctx, const char *args_json) {
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return ca_tool_result_new(0, "file_read: invalid args JSON");
    cJSON *path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (!path_j || !cJSON_IsString(path_j)) {
        cJSON_Delete(args);
        return ca_tool_result_new(0, "file_read: missing string arg 'path'");
    }
    char *rp = resolve_path(ctx, path_j->valuestring);
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

static ca_tool_result *file_write_exec(const ca_tool_ctx *ctx, const char *args_json) {
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

    int w = ca_fs_write_file(rp, content, strlen(content));
    cJSON_Delete(args);
    if (w != 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "file_write: failed to write %s", rp);
        free(rp);
        return ca_tool_result_new(0, msg);
    }
    char out[1200];
    snprintf(out, sizeof(out), "wrote %zu bytes to %s", strlen(content), rp);
    free(rp);
    return ca_tool_result_new(1, out);
}

const ca_tool *ca_tool_file_read(void) {
    static const ca_tool t = {
        "file_read",
        "Read the content of a file.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
        0,
        file_read_exec,
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
    };
    return &t;
}
