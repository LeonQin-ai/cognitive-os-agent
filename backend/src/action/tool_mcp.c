/* tool_mcp.c — Model Context Protocol tool adapter.
 * Calls a JSON-RPC 2.0 tools/call endpoint over HTTP. The server endpoint is
 * taken from args {"server_url","server","tool","args"}. If unreachable, the
 * tool returns an informative error rather than failing the whole task. */
#include "cagent/action/tools.h"
#include "cagent/os/http.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

static long g_jsonrpc_id = 1;

static ca_tool_result *mcp_exec(const ca_tool_ctx *ctx, const char *args_json) {
    (void)ctx;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return ca_tool_result_new(0, "mcp: invalid args JSON");

    cJSON *url_j = cJSON_GetObjectItemCaseSensitive(args, "server_url");
    const char *url = (url_j && cJSON_IsString(url_j)) ? url_j->valuestring : NULL;
    if (!url) {
        cJSON_Delete(args);
        return ca_tool_result_new(0,
            "mcp: no server_url configured. MCP servers are registered per-deployment; "
            "pass {\"server_url\":\"http://host:port/mcp\",\"server\":\"...\",\"tool\":\"...\",\"args\":{...}}");
    }

    cJSON *tool_j = cJSON_GetObjectItemCaseSensitive(args, "tool");
    const char *tool = (tool_j && cJSON_IsString(tool_j)) ? tool_j->valuestring : "";
    cJSON *tool_args = cJSON_GetObjectItemCaseSensitive(args, "args");
    if (!tool_args) tool_args = cJSON_CreateObject();
    else tool_args = cJSON_Duplicate(tool_args, 1);

    /* build JSON-RPC request */
    cJSON *rpc = cJSON_CreateObject();
    cJSON_AddStringToObject(rpc, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(rpc, "id", g_jsonrpc_id++);
    cJSON *method = cJSON_AddObjectToObject(rpc, "method");
    cJSON_AddStringToObject(method, "name", tool);
    cJSON_AddItemToObject(method, "arguments", tool_args);

    char *body = cJSON_PrintUnformatted(rpc);
    cJSON_Delete(rpc);
    cJSON_Delete(args);

    /* server_url may be "http://host:port/mcp" or "http://host:port" */
    char base[512], path[512];
    const char *slash = strstr(url, "://");
    const char *pathstart = slash ? strchr(slash + 3, '/') : strchr(url, '/');
    if (pathstart) {
        size_t blen = (size_t)(pathstart - url);
        if (blen >= sizeof(base)) blen = sizeof(base) - 1;
        memcpy(base, url, blen);
        base[blen] = '\0';
        snprintf(path, sizeof(path), "%s", pathstart);
    } else {
        snprintf(base, sizeof(base), "%s", url);
        snprintf(path, sizeof(path), "/");
    }

    ca_http_response *r = ca_http_post(base, path, body, "application/json", NULL, 10000);
    free(body);
    if (!r) {
        char msg[512];
        snprintf(msg, sizeof(msg), "mcp: cannot reach server %s", url);
        return ca_tool_result_new(0, msg);
    }
    if (r->status != 200) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "mcp: http %d: %s", r->status, r->body ? r->body : "");
        ca_http_response_free(r);
        return ca_tool_result_new(0, msg);
    }
    ca_tool_result *res = ca_tool_result_new(1, r->body ? r->body : "");
    ca_http_response_free(r);
    return res;
}

const ca_tool *ca_tool_mcp(void) {
    static const ca_tool t = {
        "mcp",
        "Call a Model Context Protocol tool on a remote JSON-RPC server.",
        "{\"type\":\"object\",\"properties\":{\"server_url\":{\"type\":\"string\"},\"server\":{\"type\":\"string\"},\"tool\":{\"type\":\"string\"},\"args\":{\"type\":\"object\"}}}",
        1,
        mcp_exec,
    };
    return &t;
}
