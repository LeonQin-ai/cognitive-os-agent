/* mcp_conn.h — MCP (Model Context Protocol) connection manager.
 * Standard MCP client over two transports:
 *   - "http":  JSON-RPC 2.0 POST to a URL (Streamable-HTTP style)
 *   - "stdio": JSON-RPC 2.0 over a spawned child's stdin/stdout (newline-
 *              delimited JSON; the child is spawned lazily on first use and
 *              kept alive for subsequent calls)
 * Implements the standard handshake: initialize -> notifications/initialized
 * -> tools/list (cached) and invokes tools via tools/call.
 * Discovered remote tools can be registered into the tool registry as
 * `mcp__<server>__<tool>` via mcp_manager_sync_tools(). */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

struct tool_registry; /* action/tools.h */

typedef struct mcp_conn {
    char *name;
    char *transport; /* "http" | "stdio" (NULL = "http") */
    char *url;       /* http: endpoint */
    char *token;     /* http: optional bearer token (NULL = none) */
    char *command;   /* stdio: executable, e.g. "npx" */
    char *args_csv;  /* stdio: space-separated args, e.g. "-y pkg" */
} mcp_conn;

typedef struct mcp_manager mcp_manager;

mcp_manager *mcp_manager_new(void);
void mcp_manager_free(mcp_manager *m);

/* Register/update a named connection (full form). Fields are copied. 0 ok. */
int mcp_manager_add_ex(mcp_manager *m, const mcp_conn *conn);

/* Legacy thin wrapper: adds an "http" connection. 0 ok, -1 invalid. */
int mcp_manager_add(mcp_manager *m, const char *name, const char *url, const char *token);
/* Remove a connection; when reg is given the server's dynamic tools
 * (mcp__<slug>__*) are unregistered and freed as well. */
int mcp_manager_remove(mcp_manager *m, const char *name,
                           struct tool_registry *reg);

/* Borrowed lookups (valid until next mutation). */
const mcp_conn *mcp_manager_find(mcp_manager *m, const char *name);
int mcp_manager_count(mcp_manager *m);

/* Invoke `tool` on the named server with JSON args (standard tools/call).
 * On success returns ok=1 with the concatenated text content; on failure
 * ok=0 with a diagnostic. Caller frees out_text/err_text with free(). */
int mcp_manager_call(mcp_manager *m, const char *name, const char *tool, const char *args_json, char **out_text,
                         char **err_text);

/* Discover remote tools (tools/list) for every connection and register them
 * into `reg` as `mcp__<server>__<tool>` (upsert). Returns the number of
 * tools registered, or -1 on fatal error. */
int mcp_manager_sync_tools(mcp_manager *m, struct tool_registry *reg);

/* Like mcp_manager_sync_tools, but probes only the named connection with
 * a bounded bootstrap timeout — used when adding one server so the request
 * doesn't block on every other server's handshake. Returns the number of
 * tools registered, or -1 if the name is unknown. */
int mcp_manager_sync_tools_one(mcp_manager *m, struct tool_registry *reg, const char *name);

/* JSON array of connections {name,transport,url,has_token,command,args,tools}. */
char *mcp_manager_json(mcp_manager *m);

/* One-shot connection test for the plaza "Test" button: performs the MCP
 * handshake (initialize -> notifications/initialized -> tools/list) against
 * `conn` WITHOUT registering it in any manager. Returns malloc'd JSON:
 *   {"ok":true,"transport":"stdio","count":N,"tools":["a","b"]}
 *   {"ok":false,"error":"..."} */
char *mcp_test_json(const mcp_conn *conn);

/* Persist / restore connections to <state_root>/mcp.json. */
int mcp_manager_persist(mcp_manager *m, const char *state_root);
int mcp_manager_load(mcp_manager *m, const char *state_root);

#ifdef __cplusplus
}
#endif
