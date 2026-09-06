/* mcp_conn.c — MCP (Model Context Protocol) connection manager.
 * Standard JSON-RPC 2.0 client over http (POST) or stdio (newline-delimited
 * JSON over a persistent child process). Handshake: initialize ->
 * notifications/initialized -> tools/list (cached); tools invoked via
 * tools/call. Discovered tools register into the tool registry as
 * `mcp__<server>__<tool>` with the remote inputSchema attached. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* strtok_r */
#endif
#include "cognitive-os-agent/action/mcp_conn.h"
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_proc.h"
#include "cognitive-os-agent/os/http.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define MCP_PROTO_VERSION "2024-11-05"
#define MCP_HTTP_TIMEOUT_MS 10000
/* npx may download the server on first run — measured ~30s cold on a
 * npmmirror connection, so allow 90s before declaring the handshake dead. */
#define MCP_STDIO_TIMEOUT_MS 90000
#define MCP_CLIENT_NAME "cognitive-os-agent"
#define MCP_CLIENT_VERSION "0.1"

/* Per-connection runtime state. */
typedef struct coa_mcp_session {
    coa_proc_popen *proc;   /* stdio child (NULL until first use / http) */
    int initialized;       /* handshake completed */
    cJSON *tools;          /* cached tools/list array (owned) */
} coa_mcp_session;

struct coa_mcp_manager {
    coa_mutex mtx;
    coa_mcp_conn *items;
    coa_mcp_session *sess;  /* parallel to items */
    size_t count, cap;
    /* dynamically created coa_tool structs owned here (registered into the
     * tool registry; the registry stores borrowed pointers) */
    coa_tool **owned;
    size_t n_owned, cap_owned;
};

static long g_jsonrpc_id = 1;

static void conn_free(coa_mcp_conn *c) {
    free(c->name); free(c->transport); free(c->url); free(c->token);
    free(c->command); free(c->args_csv);
}

static void sess_clear(coa_mcp_session *s) {
    if (s->proc) { coa_proc_popen_free(s->proc); s->proc = NULL; }
    if (s->tools) { cJSON_Delete(s->tools); s->tools = NULL; }
    s->initialized = 0;
}

coa_mcp_manager *coa_mcp_manager_new(void) {
    coa_mcp_manager *m = (coa_mcp_manager *)calloc(1, sizeof(coa_mcp_manager));
    if (!m) return NULL;
    coa_mutex_init(&m->mtx);
    return m;
}

void coa_mcp_manager_free(coa_mcp_manager *m) {
    if (!m) return;
    coa_mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->count; i++) { conn_free(&m->items[i]); sess_clear(&m->sess[i]); }
    free(m->items);
    free(m->sess);
    for (size_t i = 0; i < m->n_owned; i++) {
        free((void *)m->owned[i]->name);
        free((void *)m->owned[i]->description);
        free((void *)m->owned[i]->json_schema);
        free(m->owned[i]->ud);
        free(m->owned[i]);
    }
    free(m->owned);
    coa_mutex_unlock(&m->mtx);
    coa_mutex_destroy(&m->mtx);
    free(m);
}

static int find_conn(coa_mcp_manager *m, const char *name) {
    for (size_t i = 0; i < m->count; i++)
        if (strcmp(m->items[i].name, name) == 0) return (int)i;
    return -1;
}

int coa_mcp_manager_add_ex(coa_mcp_manager *m, const coa_mcp_conn *conn) {
    if (!m || !conn || !conn->name || !*conn->name) return -1;
    const char *transport = (conn->transport && *conn->transport) ? conn->transport : "http";
    int is_http = strcmp(transport, "http") == 0;
    int is_stdio = strcmp(transport, "stdio") == 0;
    if (!is_http && !is_stdio) return -1;
    if (is_http && (!conn->url || !*conn->url)) return -1;
    if (is_stdio && (!conn->command || !*conn->command)) return -1;

    coa_mutex_lock(&m->mtx);
    coa_mcp_conn *e = NULL;
    coa_mcp_session *s = NULL;
    int i = find_conn(m, conn->name);
    if (i >= 0) {
        e = &m->items[i];
        s = &m->sess[i];
        sess_clear(s);
        conn_free(e);
        memset(e, 0, sizeof(*e));
    } else {
        if (m->count == m->cap) {
            size_t ncap = m->cap ? m->cap * 2 : 8;
            coa_mcp_conn *ni = (coa_mcp_conn *)realloc(m->items, ncap * sizeof(*ni));
            coa_mcp_session *ns = (coa_mcp_session *)realloc(m->sess, ncap * sizeof(*ns));
            if (!ni || !ns) { coa_mutex_unlock(&m->mtx); return -1; }
            m->items = ni; m->sess = ns; m->cap = ncap;
        }
        e = &m->items[m->count];
        s = &m->sess[m->count];
        memset(e, 0, sizeof(*e));
        memset(s, 0, sizeof(*s));
        m->count++;
    }
    e->name = coa_strdup(conn->name);
    e->transport = coa_strdup(transport);
    e->url = conn->url ? coa_strdup(conn->url) : NULL;
    e->token = conn->token ? coa_strdup(conn->token) : NULL;
    e->command = conn->command ? coa_strdup(conn->command) : NULL;
    e->args_csv = conn->args_csv ? coa_strdup(conn->args_csv) : NULL;
    coa_mutex_unlock(&m->mtx);
    return 0;
}

int coa_mcp_manager_add(coa_mcp_manager *m, const char *name, const char *url, const char *token) {
    if (!m || !name || !*name || !url || !*url) return -1;
    coa_mcp_conn c;
    memset(&c, 0, sizeof(c));
    c.name = (char *)name;
    c.transport = (char *)"http";
    c.url = (char *)url;
    c.token = (char *)token;
    return coa_mcp_manager_add_ex(m, &c);
}

int coa_mcp_manager_remove(coa_mcp_manager *m, const char *name) {
    if (!m || !name) return -1;
    coa_mutex_lock(&m->mtx);
    int i = find_conn(m, name);
    if (i < 0) { coa_mutex_unlock(&m->mtx); return -1; }
    conn_free(&m->items[i]);
    sess_clear(&m->sess[i]);
    if (m->count - (size_t)i - 1 > 0) {
        memmove(&m->items[i], &m->items[i + 1], (m->count - (size_t)i - 1) * sizeof(coa_mcp_conn));
        memmove(&m->sess[i], &m->sess[i + 1], (m->count - (size_t)i - 1) * sizeof(coa_mcp_session));
    }
    m->count--;
    coa_mutex_unlock(&m->mtx);
    return 0;
}

const coa_mcp_conn *coa_mcp_manager_find(coa_mcp_manager *m, const char *name) {
    if (!m || !name) return NULL;
    coa_mutex_lock(&m->mtx);
    const coa_mcp_conn *c = NULL;
    int i = find_conn(m, name);
    if (i >= 0) c = &m->items[i];
    coa_mutex_unlock(&m->mtx);
    return c;
}

int coa_mcp_manager_count(coa_mcp_manager *m) {
    if (!m) return 0;
    coa_mutex_lock(&m->mtx);
    int n = (int)m->count;
    coa_mutex_unlock(&m->mtx);
    return n;
}

const coa_mcp_conn *coa_mcp_manager_get(coa_mcp_manager *m, size_t i) {
    if (!m) return NULL;
    coa_mutex_lock(&m->mtx);
    const coa_mcp_conn *c = (i < m->count) ? &m->items[i] : NULL;
    coa_mutex_unlock(&m->mtx);
    return c;
}

/* ---- stdio transport helpers ---- */

/* Split args_csv on whitespace into a NULL-terminated argv. */
static char **stdio_argv(const coa_mcp_conn *c) {
    size_t max = 4;
    if (c->args_csv) for (const char *p = c->args_csv; *p; p++) if (*p == ' ') max++;
    char **argv = (char **)calloc(max + 2, sizeof(char *));
    if (!argv) return NULL;
    int n = 0;
    argv[n++] = coa_strdup(c->command);
    if (c->args_csv) {
        char *copy = coa_strdup(c->args_csv);
        char *save = NULL;
        for (char *tok = strtok_r(copy, " \t,", &save); tok; tok = strtok_r(NULL, " \t,", &save))
            argv[n++] = coa_strdup(tok);
        free(copy);
    }
    return argv;
}

static void free_argv(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

static int stdio_ensure(coa_mcp_manager *m, size_t idx) {
    coa_mcp_conn *c = &m->items[idx];
    coa_mcp_session *s = &m->sess[idx];
    if (s->proc && coa_proc_popen_alive(s->proc)) return 0;
    if (s->proc) { coa_proc_popen_free(s->proc); s->proc = NULL; }
    s->initialized = 0;
    if (s->tools) { cJSON_Delete(s->tools); s->tools = NULL; }

    char **argv = stdio_argv(c);
    if (!argv) return -1;
    s->proc = coa_proc_popen_new(argv);
    free_argv(argv);
    return s->proc ? 0 : -1;
}

/* Scan the stdio buffer for the response with `want_id`; on success returns a
 * malloc'd JSON text of that response line and consumes it (and everything
 * before it) from the buffer. */
static char *stdio_take_response(coa_proc_popen *p, long want_id) {
    const char *buf = coa_proc_popen_buffer(p);
    const char *cur = buf;
    while (cur && *cur) {
        const char *nl = strchr(cur, '\n');
        size_t len = nl ? (size_t)(nl - cur) : strlen(cur);
        char *line = (char *)malloc(len + 1);
        if (!line) return NULL;
        memcpy(line, cur, len);
        line[len] = '\0';
        cJSON *obj = *line ? cJSON_Parse(line) : NULL;
        int is_match = 0;
        if (obj) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(obj, "id");
            if (id && cJSON_IsNumber(id) && (long)id->valuedouble == want_id) is_match = 1;
            cJSON_Delete(obj);
        }
        if (is_match) {
            /* consume everything through the matched line's newline (or to
             * end-of-buffer when the line is unterminated) */
            coa_proc_popen_trim(p, (size_t)((nl ? nl + 1 : cur + len) - buf));
            return line;
        }
        free(line);
        cur = nl ? nl + 1 : NULL;
    }
    return NULL;
}

/* Send one JSON-RPC request over the connection and return the parsed
 * "result" object (owned) or NULL with *err set. */
static cJSON *rpc_http(const coa_mcp_conn *c, const char *method, cJSON *params,
                       long id, char **err) {
    cJSON *rpc = cJSON_CreateObject();
    cJSON_AddStringToObject(rpc, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(rpc, "id", (double)id);
    cJSON_AddStringToObject(rpc, "method", method);
    if (params) cJSON_AddItemToObject(rpc, "params", params);
    char *body = cJSON_PrintUnformatted(rpc);
    cJSON_Delete(rpc);
    if (!body) { if (err) *err = coa_strdup("rpc: build request failed"); return NULL; }

    coa_strmap hdrs;
    memset(&hdrs, 0, sizeof(hdrs));
    if (c->token && *c->token) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Bearer %s", c->token);
        coa_strmap_set(&hdrs, "Authorization", auth);
    }

    char base[512], path[512];
    const char *slash = strstr(c->url, "://");
    const char *pathstart = slash ? strchr(slash + 3, '/') : strchr(c->url, '/');
    if (pathstart) {
        size_t blen = (size_t)(pathstart - c->url);
        if (blen >= sizeof(base)) blen = sizeof(base) - 1;
        memcpy(base, c->url, blen);
        base[blen] = '\0';
        snprintf(path, sizeof(path), "%s", pathstart);
    } else {
        snprintf(base, sizeof(base), "%s", c->url);
        snprintf(path, sizeof(path), "/");
    }

    coa_http_response *r = coa_http_post(base, path, body, "application/json",
                                       (c->token && *c->token) ? &hdrs : NULL,
                                       MCP_HTTP_TIMEOUT_MS);
    free(body);
    if (c->token && *c->token) coa_strmap_free(&hdrs);
    if (!r) { if (err) *err = coa_strdup("http request failed"); return NULL; }
    cJSON *resp = r->body ? cJSON_Parse(r->body) : NULL;
    int status = r->status;
    coa_http_response_free(r);
    if (!resp) {
        if (err) {
            char msg[128];
            snprintf(msg, sizeof(msg), "http %d: invalid JSON response", status);
            *err = coa_strdup(msg);
        }
        return NULL;
    }
    cJSON *jerr = cJSON_GetObjectItemCaseSensitive(resp, "error");
    if (jerr) {
        if (err) {
            char *es = cJSON_PrintUnformatted(jerr);
            *err = coa_strdup(es ? es : "json-rpc error");
            free(es);
        }
        cJSON_Delete(resp);
        return NULL;
    }
    cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(resp, "result");
    cJSON_Delete(resp);
    if (!result) { if (err) *err = coa_strdup("response has no result"); return NULL; }
    return result;
}

static cJSON *rpc_stdio(coa_mcp_manager *m, size_t idx, const char *method,
                        cJSON *params, long id, char **err, int tmo) {
    coa_mcp_conn *c = &m->items[idx];
    coa_mcp_session *s = &m->sess[idx];
    (void)c;
    if (stdio_ensure(m, idx) != 0) {
        if (params) cJSON_Delete(params);
        if (err) *err = coa_strdup("stdio: failed to spawn server process");
        return NULL;
    }
    s = &m->sess[idx];

    cJSON *rpc = cJSON_CreateObject();
    cJSON_AddStringToObject(rpc, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(rpc, "id", (double)id);
    cJSON_AddStringToObject(rpc, "method", method);
    if (params) cJSON_AddItemToObject(rpc, "params", params);
    char *body = cJSON_PrintUnformatted(rpc);
    cJSON_Delete(rpc);
    if (!body) { if (err) *err = coa_strdup("rpc: build request failed"); return NULL; }

    coa_strbuf wire;
    coa_strbuf_init(&wire);
    coa_strbuf_append(&wire, body);
    coa_strbuf_append(&wire, "\n");
    free(body);
    int wrc = coa_proc_popen_write(s->proc, wire.buf ? wire.buf : "", wire.len);
    coa_strbuf_free(&wire);
    if (wrc != 0) {
        if (err) *err = coa_strdup("stdio: write failed (server died?)");
        return NULL;
    }

    int64_t deadline = coa_time_now_ms() + (tmo > 0 ? tmo : MCP_STDIO_TIMEOUT_MS);
    while (coa_time_now_ms() < deadline && coa_proc_popen_alive(s->proc)) {
        coa_proc_popen_read(s->proc, 100);
        char *line = stdio_take_response(s->proc, id);
        if (line) {
            cJSON *resp = cJSON_Parse(line);
            free(line);
            if (!resp) { if (err) *err = coa_strdup("stdio: invalid JSON response"); return NULL; }
            cJSON *jerr = cJSON_GetObjectItemCaseSensitive(resp, "error");
            if (jerr) {
                if (err) {
                    char *es = cJSON_PrintUnformatted(jerr);
                    *err = coa_strdup(es ? es : "json-rpc error");
                    free(es);
                }
                cJSON_Delete(resp);
                return NULL;
            }
            cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(resp, "result");
            cJSON_Delete(resp);
            if (!result) { if (err) *err = coa_strdup("response has no result"); return NULL; }
            return result;
        }
    }
    if (err) *err = coa_strdup("stdio: timeout waiting for response");
    return NULL;
}

static cJSON *rpc(coa_mcp_manager *m, size_t idx, const char *method,
                  cJSON *params, long id, char **err, int tmo) {
    if (strcmp(m->items[idx].transport ? m->items[idx].transport : "http", "stdio") == 0)
        return rpc_stdio(m, idx, method, params, id, err, tmo);
    return rpc_http(&m->items[idx], method, params, id, err);
}

static void send_notification_stdio(coa_mcp_manager *m, size_t idx, const char *method) {
    coa_mcp_session *s = &m->sess[idx];
    if (!s->proc) return;
    cJSON *rpc = cJSON_CreateObject();
    cJSON_AddStringToObject(rpc, "jsonrpc", "2.0");
    cJSON_AddStringToObject(rpc, "method", method);
    char *body = cJSON_PrintUnformatted(rpc);
    cJSON_Delete(rpc);
    if (body) {
        coa_strbuf wire;
        coa_strbuf_init(&wire);
        coa_strbuf_append(&wire, body);
        coa_strbuf_append(&wire, "\n");
        coa_proc_popen_write(s->proc, wire.buf ? wire.buf : "", wire.len);
        coa_strbuf_free(&wire);
        free(body);
    }
}

/* Ensure the session completed the MCP handshake. 0 ok. */
static int ensure_initialized(coa_mcp_manager *m, size_t idx, int tmo) {
    coa_mcp_session *s = &m->sess[idx];
    if (strcmp(m->items[idx].transport ? m->items[idx].transport : "http", "stdio") == 0
        && stdio_ensure(m, idx) != 0) return -1;
    if (s->initialized) return 0;

    long id = g_jsonrpc_id++;
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "protocolVersion", MCP_PROTO_VERSION);
    cJSON *caps = cJSON_AddObjectToObject(params, "capabilities");
    (void)caps;
    cJSON *ci = cJSON_AddObjectToObject(params, "clientInfo");
    cJSON_AddStringToObject(ci, "name", MCP_CLIENT_NAME);
    cJSON_AddStringToObject(ci, "version", MCP_CLIENT_VERSION);
    char *err = NULL;
    cJSON *result = rpc(m, idx, "initialize", params, id, &err, tmo);
    if (!result) {
        fprintf(stderr, "mcp: initialize %s failed: %s\n", m->items[idx].name,
                err ? err : "?");
        free(err);
        return -1;
    }
    cJSON_Delete(result);
    if (strcmp(m->items[idx].transport ? m->items[idx].transport : "http", "stdio") == 0)
        send_notification_stdio(m, idx, "notifications/initialized");
    s->initialized = 1;
    return 0;
}

/* Fetch (and cache) the tools/list array. Borrowed pointer, NULL on error. */
static const cJSON *fetch_tools(coa_mcp_manager *m, size_t idx, int tmo) {
    coa_mcp_session *s = &m->sess[idx];
    if (s->tools) return s->tools;
    long id = g_jsonrpc_id++;
    char *err = NULL;
    cJSON *params = cJSON_CreateObject();
    cJSON *result = rpc(m, idx, "tools/list", params, id, &err, tmo);
    if (!result) { free(err); return NULL; }
    cJSON *tools = cJSON_DetachItemFromObjectCaseSensitive(result, "tools");
    cJSON_Delete(result);
    if (!tools || !cJSON_IsArray(tools)) { cJSON_Delete(tools); return NULL; }
    s->tools = tools;
    return s->tools;
}

int coa_mcp_manager_call(coa_mcp_manager *m, const char *name,
                        const char *tool, const char *args_json,
                        char **out_text, char **err_text) {
    if (out_text) *out_text = NULL;
    if (err_text) *err_text = NULL;
    if (!m || !name || !tool) {
        if (err_text) *err_text = coa_strdup("mcp: invalid call");
        return -1;
    }
    coa_mutex_lock(&m->mtx);
    int idx = find_conn(m, name);
    if (idx < 0) {
        coa_mutex_unlock(&m->mtx);
        if (err_text) *err_text = coa_strdup("mcp: unknown server");
        return -1;
    }
    if (ensure_initialized(m, (size_t)idx, MCP_STDIO_TIMEOUT_MS) != 0) {
        coa_mutex_unlock(&m->mtx);
        if (err_text) *err_text = coa_strdup("mcp: handshake failed");
        return -1;
    }

    cJSON *arguments = cJSON_Parse(args_json && *args_json ? args_json : "{}");
    if (!arguments) arguments = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", tool);
    cJSON_AddItemToObject(params, "arguments", arguments);

    long id = g_jsonrpc_id++;
    char *err = NULL;
    cJSON *result = rpc(m, (size_t)idx, "tools/call", params, id, &err,
                        MCP_STDIO_TIMEOUT_MS);
    if (!result) {
        coa_mutex_unlock(&m->mtx);
        if (err_text) *err_text = err ? err : coa_strdup("mcp: call failed");
        return -1;
    }
    /* Standard result: {content:[{type:"text",text:...}], isError?} */
    cJSON *is_err = cJSON_GetObjectItemCaseSensitive(result, "isError");
    int tool_is_error = (is_err && cJSON_IsTrue(is_err)) ? 1 : 0;
    cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    coa_strbuf sb;
    coa_strbuf_init(&sb);
    if (cJSON_IsArray(content)) {
        cJSON *it;
        cJSON_ArrayForEach(it, content) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(it, "text");
            if (text && cJSON_IsString(text) && text->valuestring) {
                coa_strbuf_append(&sb, text->valuestring);
                coa_strbuf_append(&sb, "\n");
            }
        }
    } else if (!cJSON_IsArray(content)) {
        char *rs = cJSON_PrintUnformatted(result);
        coa_strbuf_append(&sb, rs ? rs : "");
        free(rs);
    }
    cJSON_Delete(result);
    coa_mutex_unlock(&m->mtx);

    char *text = coa_strbuf_detach(&sb);
    if (tool_is_error) {
        if (err_text) *err_text = text ? text : coa_strdup("tool reported error");
        else free(text);
        return -1;
    }
    if (out_text) *out_text = text ? text : coa_strdup("");
    else free(text);
    return 0;
}

/* ---- dynamic tool registration ---- */

typedef struct mcp_tool_ud {
    coa_mcp_manager *mgr;
    char server[128];
    char tool[256];
} mcp_tool_ud;

static coa_tool_result *mcp_remote_exec(const coa_tool *self, const coa_tool_ctx *ctx,
                                       const char *args_json) {
    (void)ctx;
    mcp_tool_ud *ud = self ? (mcp_tool_ud *)self->ud : NULL;
    if (!ud || !ud->mgr)
        return coa_tool_result_new(0, "mcp: broken dynamic tool binding");
    char *out = NULL, *err = NULL;
    int rc = coa_mcp_manager_call(ud->mgr, ud->server, ud->tool, args_json, &out, &err);
    coa_tool_result *r;
    if (rc == 0) r = coa_tool_result_new(1, out ? out : "");
    else r = coa_tool_result_new(0, err ? err : "mcp: call failed");
    free(out);
    free(err);
    return r;
}

static void mcp_server_slug(const char *server, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = server; *p && o + 1 < cap; p++)
        out[o++] = ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') ? *p : '_';
    out[o] = '\0';
}

/* Eager discovery budget: bounds boot and the POST /v1/mcp add request even
 * when npx must cold-download. Servers that miss it register their tools on
 * a later sync / agent call and can be re-checked from the UI test button. */
#define MCP_BOOTSTRAP_TIMEOUT_MS 15000

/* Discover and register tools for ONE connection. Caller holds m->mtx.
 * Returns the number of tools registered. */
static int sync_server(coa_mcp_manager *m, struct coa_tool_registry *reg,
                       size_t i, int tmo) {
    int registered = 0;
    if (ensure_initialized(m, i, tmo) != 0) return 0;
    const cJSON *tools = fetch_tools(m, i, tmo);
    if (!tools) return 0;
        const char *srv = m->items[i].name;
        char slug[128];
        mcp_server_slug(srv, slug, sizeof(slug));
        cJSON *it;
        cJSON_ArrayForEach(it, tools) {
            cJSON *tname = cJSON_GetObjectItemCaseSensitive(it, "name");
            if (!tname || !cJSON_IsString(tname) || !tname->valuestring) continue;
            cJSON *tdesc = cJSON_GetObjectItemCaseSensitive(it, "description");
            cJSON *tschema = cJSON_GetObjectItemCaseSensitive(it, "inputSchema");

            char full[512];
            snprintf(full, sizeof(full), "mcp__%s__%s", slug, tname->valuestring);
            char desc[1024];
            snprintf(desc, sizeof(desc), "[mcp:%s] %s", srv,
                     (tdesc && cJSON_IsString(tdesc) && tdesc->valuestring)
                         ? tdesc->valuestring : "MCP tool");

            coa_tool *t = (coa_tool *)calloc(1, sizeof(*t));
            mcp_tool_ud *ud = (mcp_tool_ud *)calloc(1, sizeof(*ud));
            char *schema_str = tschema ? cJSON_PrintUnformatted(tschema) : NULL;
            if (!t || !ud) { free(t); free(ud); free(schema_str); continue; }
            ud->mgr = m;
            snprintf(ud->server, sizeof(ud->server), "%s", srv);
            snprintf(ud->tool, sizeof(ud->tool), "%s", tname->valuestring);
            t->name = coa_strdup(full);
            t->description = coa_strdup(desc);
            t->json_schema = schema_str;      /* NULL ok (validation skipped) */
            t->is_write = 1;                  /* remote side effects unknown */
            t->execute = mcp_remote_exec;
            t->ud = ud;
            if (coa_tool_register_ex(reg, t, 1) == 0) {
                if (m->n_owned == m->cap_owned) {
                    size_t nc = m->cap_owned ? m->cap_owned * 2 : 16;
                    coa_tool **no = (coa_tool **)realloc(m->owned, nc * sizeof(*no));
                    if (!no) { free(schema_str); continue; }
                    m->owned = no;
                    m->cap_owned = nc;
                }
                m->owned[m->n_owned++] = t;
                registered++;
            } else {
                /* already registered and unchanged: replace in place */
                coa_tool *prev = (coa_tool *)coa_tool_find(reg, full);
                if (prev && prev->ud && prev->execute == mcp_remote_exec) {
                    mcp_tool_ud *pud = (mcp_tool_ud *)prev->ud;
                    if (strcmp(pud->server, srv) == 0 && strcmp(pud->tool, tname->valuestring) == 0) {
                        /* replace contents of the previously owned struct */
                        for (size_t k = 0; k < m->n_owned; k++) {
                            if (m->owned[k] == prev) {
                                free((void *)prev->name);
                                free((void *)prev->description);
                                free((void *)prev->json_schema);
                                free(prev->ud);
                                prev->name = coa_strdup(full);
                                prev->description = coa_strdup(desc);
                                prev->json_schema = schema_str;
                                prev->ud = ud;
                                registered++;
                                break;
                            }
                        }
                        continue;
                    }
                }
                free((void *)t->name);
                free((void *)t->description);
                free(schema_str);
                free(t->ud);
                free(t);
            }
        }
    return registered;
}

int coa_mcp_manager_sync_tools(coa_mcp_manager *m, struct coa_tool_registry *reg) {
    if (!m || !reg) return -1;
    int registered = 0;
    coa_mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->count; i++)
        registered += sync_server(m, reg, i, MCP_BOOTSTRAP_TIMEOUT_MS);
    coa_mutex_unlock(&m->mtx);
    return registered;
}

/* Discover tools for a single named connection only (used by POST /v1/mcp so
 * adding one server doesn't pay the handshake cost of every other server).
 * Returns registered tool count, or -1 when the name is unknown. */
int coa_mcp_manager_sync_tools_one(coa_mcp_manager *m, struct coa_tool_registry *reg,
                                   const char *name) {
    if (!m || !reg || !name) return -1;
    coa_mutex_lock(&m->mtx);
    int idx = find_conn(m, name);
    if (idx < 0) {
        coa_mutex_unlock(&m->mtx);
        return -1;
    }
    int registered = sync_server(m, reg, (size_t)idx, MCP_BOOTSTRAP_TIMEOUT_MS);
    coa_mutex_unlock(&m->mtx);
    return registered;
}

int coa_mcp_manager_tool_count(coa_mcp_manager *m, const char *name) {
    if (!m || !name) return -1;
    coa_mutex_lock(&m->mtx);
    int idx = find_conn(m, name);
    int n = -1;
    if (idx >= 0) {
        if (ensure_initialized(m, (size_t)idx, MCP_BOOTSTRAP_TIMEOUT_MS) == 0) {
            const cJSON *tools = fetch_tools(m, (size_t)idx, MCP_BOOTSTRAP_TIMEOUT_MS);
            n = tools ? cJSON_GetArraySize(tools) : -1;
        }
    }
    coa_mutex_unlock(&m->mtx);
    return n;
}

char *coa_mcp_manager_json(coa_mcp_manager *m) {
    cJSON *arr = cJSON_CreateArray();
    if (!m) return cJSON_PrintUnformatted(arr);
    coa_mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->count; i++) {
        coa_mcp_conn *e = &m->items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "transport", e->transport ? e->transport : "http");
        if (e->url) cJSON_AddStringToObject(o, "url", e->url);
        cJSON_AddBoolToObject(o, "has_token", (e->token && *e->token) ? 1 : 0);
        if (e->command) cJSON_AddStringToObject(o, "command", e->command);
        if (e->args_csv) cJSON_AddStringToObject(o, "args", e->args_csv);
        coa_mcp_session *s = &m->sess[i];
        cJSON_AddNumberToObject(o, "tools", s->tools ? cJSON_GetArraySize(s->tools) : 0);
        cJSON_AddBoolToObject(o, "connected", s->initialized ? 1 : 0);
        cJSON_AddItemToArray(arr, o);
    }
    coa_mutex_unlock(&m->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}

/* ---- persistence ---- */

int coa_mcp_manager_persist(coa_mcp_manager *m, const char *state_root) {
    if (!m || !state_root || !*state_root) return -1;
    char path[1024];
    coa_path_join(path, sizeof(path), state_root, "mcp.json");
    char *s = coa_mcp_manager_json(m);
    if (!s) return -1;
    int rc = coa_fs_write_file(path, s, strlen(s));
    free(s);
    return rc;
}

int coa_mcp_manager_load(coa_mcp_manager *m, const char *state_root) {
    if (!m || !state_root || !*state_root) return -1;
    char path[1024];
    coa_path_join(path, sizeof(path), state_root, "mcp.json");
    char *body = coa_fs_read_file(path);
    if (!body) return -1;
    cJSON *arr = cJSON_Parse(body);
    free(body);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return -1; }
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsObject(it)) continue;
        cJSON *jn = cJSON_GetObjectItemCaseSensitive(it, "name");
        if (!jn || !cJSON_IsString(jn)) continue;
        coa_mcp_conn c;
        memset(&c, 0, sizeof(c));
        c.name = jn->valuestring;
        cJSON *jt = cJSON_GetObjectItemCaseSensitive(it, "transport");
        cJSON *ju = cJSON_GetObjectItemCaseSensitive(it, "url");
        cJSON *jk = cJSON_GetObjectItemCaseSensitive(it, "token");
        cJSON *jc = cJSON_GetObjectItemCaseSensitive(it, "command");
        cJSON *ja = cJSON_GetObjectItemCaseSensitive(it, "args");
        if (jt && cJSON_IsString(jt)) c.transport = jt->valuestring;
        if (ju && cJSON_IsString(ju)) c.url = ju->valuestring;
        if (jk && cJSON_IsString(jk)) c.token = jk->valuestring;
        if (jc && cJSON_IsString(jc)) c.command = jc->valuestring;
        if (ja && cJSON_IsString(ja)) c.args_csv = ja->valuestring;
        coa_mcp_manager_add_ex(m, &c);
    }
    cJSON_Delete(arr);
    return 0;
}

/* ---- one-shot connection test (plaza "Test" button) ---- */

/* Wait for the JSON-RPC response with `want_id` on a fresh stdio child.
 * Returns the parsed "result" object (owned) or NULL with *err set. */
static cJSON *test_stdio_roundtrip(coa_proc_popen *proc, const char *method,
                                   cJSON *params, long want_id, char **err) {
    cJSON *rpc = cJSON_CreateObject();
    cJSON_AddStringToObject(rpc, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(rpc, "id", (double)want_id);
    cJSON_AddStringToObject(rpc, "method", method);
    if (params) cJSON_AddItemToObject(rpc, "params", params);
    char *body = cJSON_PrintUnformatted(rpc);
    cJSON_Delete(rpc);
    if (!body) { if (err) *err = coa_strdup("rpc: build request failed"); return NULL; }
    coa_strbuf wire;
    coa_strbuf_init(&wire);
    coa_strbuf_append(&wire, body);
    coa_strbuf_append(&wire, "\n");
    free(body);
    int wrc = coa_proc_popen_write(proc, wire.buf ? wire.buf : "", wire.len);
    coa_strbuf_free(&wire);
    if (wrc != 0) { if (err) *err = coa_strdup("stdio: write failed (server died?)"); return NULL; }

    int64_t deadline = coa_time_now_ms() + MCP_STDIO_TIMEOUT_MS;
    while (coa_time_now_ms() < deadline && coa_proc_popen_alive(proc)) {
        coa_proc_popen_read(proc, 100);
        char *line = stdio_take_response(proc, want_id);
        if (line) {
            cJSON *resp = cJSON_Parse(line);
            free(line);
            if (!resp) { if (err) *err = coa_strdup("stdio: invalid JSON response"); return NULL; }
            cJSON *jerr = cJSON_GetObjectItemCaseSensitive(resp, "error");
            if (jerr) {
                if (err) {
                    char *es = cJSON_PrintUnformatted(jerr);
                    *err = coa_strdup(es ? es : "json-rpc error");
                    free(es);
                }
                cJSON_Delete(resp);
                return NULL;
            }
            cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(resp, "result");
            cJSON_Delete(resp);
            if (!result) { if (err) *err = coa_strdup("response has no result"); return NULL; }
            return result;
        }
    }
    if (err) *err = coa_strdup("stdio: timeout waiting for response");
    return NULL;
}

char *coa_mcp_test_json(const coa_mcp_conn *conn) {
    const char *transport = (conn && conn->transport && *conn->transport) ? conn->transport : "http";
    cJSON *o = cJSON_CreateObject();
    int is_stdio = strcmp(transport, "stdio") == 0;
    if (!conn || (!is_stdio && (!conn->url || !*conn->url)) ||
        (is_stdio && (!conn->command || !*conn->command))) {
        cJSON_AddBoolToObject(o, "ok", 0);
        cJSON_AddStringToObject(o, "error", "need 'command' (stdio) or 'url' (http)");
        char *s = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        return s;
    }

    char *err = NULL;
    int count = -1;
    cJSON *tools = NULL; /* owned array of tool-name strings */

    /* handshake params shared by both transports */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "protocolVersion", MCP_PROTO_VERSION);
    cJSON *caps = cJSON_AddObjectToObject(params, "capabilities");
    (void)caps;
    cJSON *ci = cJSON_AddObjectToObject(params, "clientInfo");
    cJSON_AddStringToObject(ci, "name", MCP_CLIENT_NAME);
    cJSON_AddStringToObject(ci, "version", MCP_CLIENT_VERSION);

    if (is_stdio) {
        char **argv = stdio_argv(conn);
        coa_proc_popen *proc = argv ? coa_proc_popen_new_ex(argv, 1) : NULL;
        free_argv(argv);
        if (!proc) {
            cJSON_AddBoolToObject(o, "ok", 0);
            cJSON_AddStringToObject(o, "error", "stdio: failed to spawn server process");
            char *s = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
            cJSON_Delete(params);
            return s;
        }
        cJSON *result = test_stdio_roundtrip(proc, "initialize", params, g_jsonrpc_id++, &err);
        if (result) {
            cJSON_Delete(result);
            /* notifications/initialized (no id, no response) */
            cJSON *note = cJSON_CreateObject();
            cJSON_AddStringToObject(note, "jsonrpc", "2.0");
            cJSON_AddStringToObject(note, "method", "notifications/initialized");
            char *nb = cJSON_PrintUnformatted(note);
            cJSON_Delete(note);
            if (nb) {
                coa_strbuf w2;
                coa_strbuf_init(&w2);
                coa_strbuf_append(&w2, nb);
                coa_strbuf_append(&w2, "\n");
                coa_proc_popen_write(proc, w2.buf ? w2.buf : "", w2.len);
                coa_strbuf_free(&w2);
                free(nb);
            }
            result = test_stdio_roundtrip(proc, "tools/list", NULL, g_jsonrpc_id++, &err);
            if (result) {
                cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "tools");
                tools = (arr && cJSON_IsArray(arr)) ? cJSON_Duplicate(arr, 1) : cJSON_CreateArray();
                count = cJSON_GetArraySize(tools);
                cJSON_Delete(result);
            }
        }
        /* surface the server's own stdout/stderr tail so first-run failures
         * (npx downloads, missing runtimes) are diagnosable from the UI */
        if (count < 0) {
            const char *out = coa_proc_popen_buffer(proc);
            if (out && *out) {
                char tail[260];
                size_t len = strlen(out);
                size_t start = len >= sizeof(tail) - 1 ? len - (sizeof(tail) - 1) : 0;
                snprintf(tail, sizeof(tail), "%s", out + start);
                for (char *p = tail; *p; p++)
                    if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
                char merged[640];
                snprintf(merged, sizeof(merged), "%s | server output: %s",
                         err ? err : "connection test failed", tail);
                free(err);
                err = coa_strdup(merged);
            }
        }
        coa_proc_popen_free(proc);
    } else {
        cJSON *result = rpc_http(conn, "initialize", params, g_jsonrpc_id++, &err);
        if (result) {
            cJSON_Delete(result);
            result = rpc_http(conn, "tools/list", NULL, g_jsonrpc_id++, &err);
            if (result) {
                cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "tools");
                tools = (arr && cJSON_IsArray(arr)) ? cJSON_Duplicate(arr, 1) : cJSON_CreateArray();
                count = cJSON_GetArraySize(tools);
                cJSON_Delete(result);
            }
        }
    }

    if (count < 0) {
        cJSON_AddBoolToObject(o, "ok", 0);
        cJSON_AddStringToObject(o, "error", err ? err : "connection test failed");
    } else {
        cJSON_AddBoolToObject(o, "ok", 1);
        cJSON_AddStringToObject(o, "transport", transport);
        cJSON_AddNumberToObject(o, "count", (double)count);
        cJSON *names = cJSON_AddArrayToObject(o, "tools");
        cJSON *t;
        cJSON_ArrayForEach(t, tools) {
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(t, "name");
            if (nm && cJSON_IsString(nm)) cJSON_AddItemToArray(names, cJSON_CreateString(nm->valuestring));
        }
    }
    free(err);
    if (tools) cJSON_Delete(tools);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}
