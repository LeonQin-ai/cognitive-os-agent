#include "cognitive-os-agent/tx/tx.h"
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/snapshot/snapshot.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct tx_manager {
    int unused;
};

struct tx {
    snapshot *snap;
    tool_registry *tools;
    tool_ctx ctx;
    int all_ok;
    int n_actions;
    int rolled_back;
    int git_managed; /* workspace lives inside a git repo -> no snapshots */
    char *output;    /* accumulated "[tool] output\n" lines */
    size_t output_len;
    size_t output_cap;
};

/* 1 if `dir` or any of its ancestors contains .git (a directory, or a file
 * for git worktrees/submodules). Git already provides version control, so
 * the snapshot engine stays out of the way for git-managed workspaces. */
static int is_git_managed(const char *dir) {
    if (!dir || !*dir)
        return 0;
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", dir);
    for (int depth = 0; depth < 32; depth++) {
        char gitp[2200];
        path_join(gitp, sizeof(gitp), buf, ".git");
        if (fs_exists(gitp))
            return 1;
        /* strip the last path component */
        char *slash = strrchr(buf, '/');
#if defined(_WIN32)
        char *bslash = strrchr(buf, '\\');
        if (bslash && (!slash || bslash > slash))
            slash = bslash;
#endif
        if (!slash || slash == buf)
            break;
        *slash = '\0';
        /* drive root reached ("D:") — check the root itself, then stop */
        size_t n = strlen(buf);
        if (n == 2 && buf[1] == ':') {
            snprintf(gitp, sizeof(gitp), "%s\\.git", buf);
            return fs_exists(gitp);
        }
    }
    return 0;
}

tx_manager *tx_manager_new(void) {
    return calloc(1, sizeof(tx_manager));
}

void tx_manager_free(tx_manager *m) {
    free(m);
}

tx *tx_begin(tx_manager *m, snapshot *snap, tool_registry *tools, const tool_ctx *ctx) {
    (void)m;
    /* sizeof(*tx): a local named `tx` shadows the typedef, so a bare
     * sizeof(tx) here would yield pointer size (8) — classic heap overflow */
    tx *tx = calloc(1, sizeof(*tx));
    if (!tx)
        return NULL;
    tx->snap = snap;
    tx->tools = tools;
    tx->all_ok = 1;
    if (ctx)
        tx->ctx = *ctx;
    tx->ctx.tx = tx;
    tx->git_managed = is_git_managed(tx->ctx.workspace);
    return tx;
}

/* Extract candidate file paths from a tool's args JSON (for pre-capture).
 * Paths are resolved against the transaction workspace so rollback restores
 * the same files the tools actually write to. */
static void extract_paths(const char *args_json, const char *workspace, char *paths[16], int *npaths) {
    *npaths = 0;
    if (!args_json)
        return;
    cJSON *args = cJSON_Parse(args_json);
    if (!args)
        return;
    cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (p && cJSON_IsString(p) && *npaths < 16) {
        char full[2048];
        path_resolve(full, sizeof(full), workspace, p->valuestring);
        paths[(*npaths)++] = xstrdup(full);
    }
    cJSON *files = cJSON_GetObjectItemCaseSensitive(args, "files");
    if (files && cJSON_IsArray(files)) {
        cJSON *it;
        cJSON_ArrayForEach(it, files) {
            if (cJSON_IsString(it) && *npaths < 16) {
                char full[2048];
                path_resolve(full, sizeof(full), workspace, it->valuestring);
                paths[(*npaths)++] = xstrdup(full);
            }
        }
    }
    cJSON_Delete(args);
}

/* Append one action's result line "[tool] output\n" to tx->output. */
static void tx_append_output(tx *tx, const char *tool, const char *output) {
    const char *out = output ? output : "";
    size_t need = strlen(tool) + strlen(out) + 4; /* "[", "] ", "\n", NUL */
    if (tx->output_len + need > tx->output_cap) {
        size_t ncap = tx->output_cap ? tx->output_cap * 2 : 256;
        while (ncap < tx->output_len + need)
            ncap *= 2;
        char *nb = (char *)realloc(tx->output, ncap);
        if (!nb)
            return;
        tx->output = nb;
        tx->output_cap = ncap;
    }
    tx->output_len +=
        (size_t)snprintf(tx->output + tx->output_len, tx->output_cap - tx->output_len, "[%s] %s\n", tool, out);
}

int tx_run(tx *tx, const char *tool_name, const char *args_json) {
    if (!tx || !tx->tools)
        return -1;
    const tool *tool = tool_find(tx->tools, tool_name);
    if (!tool)
        return -1;

    /* pre-capture write targets so rollback can restore them. Skipped for
     * git-managed workspaces: git is the version control, duplicating file
     * content in the snapshot block store is wasted work (and cannot handle
     * huge files anyway). */
    if (tx->snap && tool->is_write && !tx->git_managed) {
        char *paths[16];
        int npaths = 0;
        extract_paths(args_json, tx->ctx.workspace, paths, &npaths);
        for (int i = 0; i < npaths; i++) {
            snapshot_capture(tx->snap, paths[i]);
            free(paths[i]);
        }
    }

    tool_result *r = tool_execute(tx->tools, tool_name, args_json, &tx->ctx);
    if (!r)
        return -1;
    tx->n_actions++;
    tx_append_output(tx, tool_name, r->output);
    int ok = r->ok;
    if (!ok)
        tx->all_ok = 0;
    tool_result_free(r);
    return ok ? 0 : -1;
}

int tx_validate(tx *tx) {
    return tx ? tx->all_ok : 0;
}

const char *tx_output(tx *tx) {
    return tx ? tx->output : NULL;
}

int tx_commit(tx *tx) {
    if (!tx)
        return -1;
    if (tx->snap)
        snapshot_commit(tx->snap);
    return 0;
}

int tx_rollback(tx *tx) {
    if (!tx)
        return -1;
    if (tx->snap)
        snapshot_restore_pending(tx->snap);
    tx->rolled_back = 1;
    return 0;
}

void tx_free(tx *tx) {
    if (!tx)
        return;
    free(tx->output);
    free(tx);
}
