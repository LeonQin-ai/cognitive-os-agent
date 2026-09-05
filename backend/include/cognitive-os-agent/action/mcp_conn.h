/* mcp_conn.h — MCP (Model Context Protocol) connection manager.
 * Standard MCP client over two transports:
 *   - "http":  JSON-RPC 2.0 POST to a URL (Streamable-HTTP style)
 *   - "stdio": JSON-RPC 2.0 over a spawned child's stdin/stdout (newline-
 *              delimited JSON; the child is spawned lazily on first use and
 *              kept alive for subsequent calls)
 * Implements the standard handshake: initialize -> notifications/initialized
 * -> tools/list (cached) and invokes tools via tools/call.
 * Discovered remote tools can be registered into the tool registry as
 * `mcp__<server>__<tool>` via ca_mcp_manager_sync_tools(). */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

struct ca_tool_registry;   /* action/tools.h */

typedef struct ca_mcp_conn {
    char *name;
    char *transport;   /* "http" | "stdio" (NULL = "http") */
    char *url;         /* http: endpoint */
    char *token;       /* http: optional bearer token (NULL = none) */
    char *command;     /* stdio: executable, e.g. "npx" */
    char *args_csv;    /* stdio: space-separated args, e.g. "-y pkg" */
} ca_mcp_conn;

typedef struct ca_mcp_manager ca_mcp_manager;

ca_mcp_manager *ca_mcp_manager_new(void);
void ca_mcp_manager_free(ca_mcp_manager *m);

/* Register/update a named connection (full form). Fields are copied. 0 ok. */
int ca_mcp_manager_add_ex(ca_mcp_manager *m, const ca_mcp_conn *conn);

/* Legacy thin wrapper: adds an "http" connection. 0 ok, -1 invalid. */
int ca_mcp_manager_add(ca_mcp_manager *m, const char *name, const char *url, const char *token);
int ca_mcp_manager_remove(ca_mcp_manager *m, const char *name);

/* Borrowed lookups (valid until next mutation). */
const ca_mcp_conn *ca_mcp_manager_find(ca_mcp_manager *m, const char *name);
int ca_mcp_manager_count(ca_mcp_manager *m);
const ca_mcp_conn *ca_mcp_manager_get(ca_mcp_manager *m, size_t i);

/* Invoke `tool` on the named server with JSON args (standard tools/call).
 * On success returns ok=1 with the concatenated text content; on failure
 * ok=0 with a diagnostic. Caller frees out_text/err_text with free(). */
int ca_mcp_manager_call(ca_mcp_manager *m, const char *name,
                        const char *tool, const char *args_json,
                        char **out_text, char **err_text);

/* Discover remote tools (tools/list) for every connection and register them
 * into `reg` as `mcp__<server>__<tool>` (upsert). Returns the number of
 * tools registered, or -1 on fatal error. */
int ca_mcp_manager_sync_tools(ca_mcp_manager *m, struct ca_tool_registry *reg);

/* Number of discovered tools for a connection (-1 if unknown). */
int ca_mcp_manager_tool_count(ca_mcp_manager *m, const char *name);

/* JSON array of connections {name,transport,url,has_token,command,args,tools}. */
char *ca_mcp_manager_json(ca_mcp_manager *m);

/* Persist / restore connections to <state_root>/mcp.json. */
int ca_mcp_manager_persist(ca_mcp_manager *m, const char *state_root);
int ca_mcp_manager_load(ca_mcp_manager *m, const char *state_root);

#ifdef __cplusplus
}
#endif
