/* tool_mcp.c — Model Context Protocol tool adapter.
 * Invokes a tool on a REGISTERED MCP connection (http or stdio) via the
 * standard tools/call protocol. Discovered remote tools are usually invoked
 * directly as mcp__<server>__<tool>; this generic tool takes
 * {"server","tool","args"} for servers that were added after tool sync.
 * If unreachable, the tool returns an informative error rather than failing
 * the whole task. */
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/action/mcp_conn.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

static coa_tool_result *mcp_exec(const coa_tool *self, const coa_tool_ctx *ctx, const char *args_json) {
    (void)self;
    if (!ctx || !ctx->mcp)
        return coa_tool_result_new(0, "mcp: no MCP manager available");
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return coa_tool_result_new(0, "mcp: invalid args JSON");

    cJSON *srv_j = cJSON_GetObjectItemCaseSensitive(args, "server");
    cJSON *tool_j = cJSON_GetObjectItemCaseSensitive(args, "tool");
    if (!srv_j || !cJSON_IsString(srv_j) || !tool_j || !cJSON_IsString(tool_j)) {
        cJSON_Delete(args);
        return coa_tool_result_new(0,
            "mcp: required string args 'server' (connection name) and 'tool'; "
            "see the MCP connection list for available servers");
    }
    const char *server = srv_j->valuestring;
    const char *tool = tool_j->valuestring;

    char *args_out = NULL;
    cJSON *a_j = cJSON_GetObjectItemCaseSensitive(args, "args");
    if (a_j && cJSON_IsObject(a_j)) args_out = cJSON_PrintUnformatted(a_j);

    char *out = NULL, *err = NULL;
    int rc = coa_mcp_manager_call(ctx->mcp, server, tool,
                                 args_out ? args_out : "{}", &out, &err);
    free(args_out);
    cJSON_Delete(args);
    coa_tool_result *r = (rc == 0)
        ? coa_tool_result_new(1, out ? out : "")
        : coa_tool_result_new(0, err ? err : "mcp: call failed");
    free(out);
    free(err);
    return r;
}

const coa_tool *coa_tool_mcp(void) {
    static const coa_tool t = {
        "mcp",
        "Call a tool on a REGISTERED MCP server by connection name. Discovered "
        "tools are also exposed directly as mcp__<server>__<tool> entries.",
        "{\"type\":\"object\",\"properties\":{\"server\":{\"type\":\"string\"},"
        "\"tool\":{\"type\":\"string\"},"
        "\"args\":{\"type\":\"object\",\"description\":\"arguments for the remote tool\"}},"
        "\"required\":[\"server\",\"tool\"]}",
        1,
        mcp_exec,
        NULL,
    };
    return &t;
}
