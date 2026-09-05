/* executor.c — Execution Runtime: executor vtable plumbing + LocalExecutor.
 * LocalExecutor delegates action execution to the tool registry (a pure move
 * of the previous inline path — semantics unchanged). Sandbox/VM executors
 * plug in behind the same vtable later. */
#include "cognitive-os-agent/execution/executor.h"
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/snapshot/snapshot.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

void coa_executor_result_free(coa_executor_result *r) {
    if (!r) return;
    free(r->output);
    free(r);
}

coa_executor *coa_executor_new(const coa_executor_ops *ops, void *impl) {
    if (!ops || !ops->execute || !impl) return NULL;
    coa_executor *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->ops = ops;
    e->impl = impl;
    return e;
}

void coa_executor_free(coa_executor *e) {
    if (!e) return;
    if (e->ops->destroy) e->ops->destroy(e->impl);
    free(e);
}

const char *coa_executor_name(const coa_executor *e) {
    return (e && e->ops && e->ops->name) ? e->ops->name : "?";
}

int coa_executor_start(coa_executor *e) {
    if (!e || !e->ops->start) return 0;
    return e->ops->start(e->impl);
}

int coa_executor_stop(coa_executor *e) {
    if (!e || !e->ops->stop) return 0;
    return e->ops->stop(e->impl);
}

int coa_executor_execute(coa_executor *e, const char *tool, const char *args_json,
                        coa_executor_result **result) {
    if (!e || !e->ops->execute || !result) return -1;
    return e->ops->execute(e->impl, tool, args_json, result);
}

int coa_executor_snapshot(coa_executor *e, char **snapshot_id) {
    if (!e || !e->ops->snapshot || !snapshot_id) return -1;
    return e->ops->snapshot(e->impl, snapshot_id);
}

int coa_executor_restore(coa_executor *e, const char *snapshot_id) {
    if (!e || !e->ops->restore) return -1;
    return e->ops->restore(e->impl, snapshot_id);
}

/* ---------- LocalExecutor ---------- */

typedef struct local_impl {
    coa_tool_registry *reg;
    coa_tool_ctx *tctx;    /* borrowed; built by the caller (h_act) */
    coa_snapshot *snap;    /* may be NULL */
} local_impl;

static int local_start(void *impl) { (void)impl; return 0; }
static int local_stop(void *impl)  { (void)impl; return 0; }

static int local_execute(void *impl, const char *tool, const char *args_json,
                         coa_executor_result **result) {
    local_impl *li = impl;
    if (!result) return -1;
    *result = NULL;
    coa_tool_result *tr = coa_tool_execute(li->reg, tool, args_json, li->tctx);
    if (!tr) return -1; /* unknown tool / registry failure */
    coa_executor_result *er = calloc(1, sizeof(*er));
    if (!er) { coa_tool_result_free(tr); return -1; }
    er->ok = tr->ok;
    char *safe = coa_str_utf8_sanitize(tr->output ? tr->output : "");
    er->output = safe ? safe : coa_strdup(tr->output ? tr->output : "");
    coa_tool_result_free(tr);
    *result = er;
    return 0;
}

static void local_destroy(void *impl) {
    free(impl);
    /* tctx is owned by the caller (stack); reg/snap are borrowed */
}

static int local_snapshot(void *impl, char **snapshot_id) {
    local_impl *li = impl;
    if (!li->snap || !snapshot_id) return -1;
    /* capture the workspace baseline; the id is the commit label */
    coa_snapshot_capture(li->snap, li->tctx->workspace ? li->tctx->workspace : ".");
    const char *id = coa_snapshot_commit(li->snap);
    if (!id) return -1;
    *snapshot_id = coa_strdup(id);
    return *snapshot_id ? 0 : -1;
}

static int local_restore(void *impl, const char *snapshot_id) {
    local_impl *li = impl;
    if (!li->snap || !snapshot_id) return -1;
    return coa_snapshot_restore(li->snap, snapshot_id);
}

static const coa_executor_ops local_ops = {
    "local", local_start, local_execute, local_stop,
    local_destroy, local_snapshot, local_restore,
};

coa_executor *coa_executor_new_local(coa_tool_registry *reg, coa_tool_ctx *tctx,
                                   void *snapshot) {
    if (!reg || !tctx) return NULL;
    local_impl *li = calloc(1, sizeof(*li));
    if (!li) return NULL;
    li->reg = reg;
    li->tctx = tctx;
    li->snap = (coa_snapshot *)snapshot;
    coa_executor *e = coa_executor_new(&local_ops, li);
    if (!e) free(li);
    return e;
}

/* ---------- Routing executors: WSL / Remote ---------- */

/* POSIX single-quote a string for `bash -c '...'`. Malloc'd, caller frees. */
static char *sh_quote(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s), cap = n * 4 + 3;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    char *w = out;
    *w++ = '\'';
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\'') { memcpy(w, "'\\''", 4); w += 4; }
        else *w++ = s[i];
    }
    *w++ = '\'';
    *w = '\0';
    return out;
}

typedef struct route_impl {
    coa_executor *inner; /* owned */
    char *target;       /* distro or user@host; NULL = WSL default */
    int is_wsl;
} route_impl;

/* Forward one action to inner; for shell, wrap cmd into the target env. */
static int route_execute(void *impl, const char *tool, const char *args_json,
                         coa_executor_result **result) {
    route_impl *ri = impl;
    if (!result) return -1;
    *result = NULL;
    if (!tool || strcmp(tool, "shell") != 0)
        return coa_executor_execute(ri->inner, tool, args_json, result);

    /* extract cmd */
    cJSON *root = args_json ? cJSON_Parse(args_json) : NULL;
    cJSON *cmdj = root ? cJSON_GetObjectItemCaseSensitive(root, "cmd") : NULL;
    if (!cmdj || !cJSON_IsString(cmdj)) {
        if (root) cJSON_Delete(root);
        return coa_executor_execute(ri->inner, tool, args_json, result);
    }
    char *q = sh_quote(cmdj->valuestring);
    char wrapped[4096];
    if (ri->is_wsl) {
        if (ri->target && *ri->target)
            snprintf(wrapped, sizeof(wrapped), "wsl.exe -d %s -e bash -c %s",
                     ri->target, q);
        else
            snprintf(wrapped, sizeof(wrapped), "wsl.exe -e bash -c %s", q);
    } else {
        snprintf(wrapped, sizeof(wrapped), "ssh -o ConnectTimeout=5 %s bash -c %s",
                 ri->target ? ri->target : "", q);
    }
    free(q);
    cJSON_Delete(root);

    cJSON *n = cJSON_CreateObject();
    if (!n) return -1;
    cJSON_AddStringToObject(n, "cmd", wrapped);
    /* carry timeout through if the caller set one */
    if (args_json) {
        cJSON *t = cJSON_Parse(args_json);
        cJSON *tj = t ? cJSON_GetObjectItemCaseSensitive(t, "timeout_ms") : NULL;
        if (tj && cJSON_IsNumber(tj))
            cJSON_AddNumberToObject(n, "timeout_ms", tj->valuedouble);
        if (t) cJSON_Delete(t);
    }
    char *nargs = cJSON_PrintUnformatted(n);
    cJSON_Delete(n);
    if (!nargs) return -1;
    int rc = coa_executor_execute(ri->inner, tool, nargs, result);
    free(nargs);
    return rc;
}

static int route_start(void *impl) {
    route_impl *ri = impl;
    return coa_executor_start(ri->inner);
}
static int route_stop(void *impl) {
    route_impl *ri = impl;
    return coa_executor_stop(ri->inner);
}
static int route_snapshot(void *impl, char **snapshot_id) {
    route_impl *ri = impl;
    return coa_executor_snapshot(ri->inner, snapshot_id);
}
static int route_restore(void *impl, const char *snapshot_id) {
    route_impl *ri = impl;
    return coa_executor_restore(ri->inner, snapshot_id);
}
static void route_destroy(void *impl) {
    route_impl *ri = impl;
    if (ri->inner) coa_executor_free(ri->inner);
    free(ri->target);
    free(ri);
}

static const coa_executor_ops wsl_ops = {
    "wsl", route_start, route_execute, route_stop,
    route_destroy, route_snapshot, route_restore,
};
static const coa_executor_ops remote_ops = {
    "remote", route_start, route_execute, route_stop,
    route_destroy, route_snapshot, route_restore,
};

static coa_executor *coa_executor_new_route(coa_executor *inner, const char *target,
                                          int is_wsl) {
    if (!inner) return NULL;
    route_impl *ri = calloc(1, sizeof(*ri));
    if (!ri) return NULL;
    ri->inner = inner;
    ri->target = target ? coa_strdup(target) : NULL;
    if (target && *target && !ri->target) { free(ri); return NULL; }
    ri->is_wsl = is_wsl;
    coa_executor *e = coa_executor_new(is_wsl ? &wsl_ops : &remote_ops, ri);
    if (!e) { free(ri->target); free(ri); }
    return e;
}

coa_executor *coa_executor_new_wsl(coa_executor *inner, const char *distro) {
    return coa_executor_new_route(inner, distro, 1);
}

coa_executor *coa_executor_new_remote(coa_executor *inner, const char *host) {
    if (!host || !*host) return NULL;
    return coa_executor_new_route(inner, host, 0);
}
