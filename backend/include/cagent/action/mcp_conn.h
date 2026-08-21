/* mcp_conn.h — MCP (Model Context Protocol) connection manager.
 * Maintains named connections to JSON-RPC 2.0 MCP servers. The MCP tool
 * adapter (tool_mcp.c) can resolve a server by name and reuse the stored
 * endpoint + optional bearer token instead of passing server_url inline. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_mcp_conn {
    char *name;
    char *url;       /* e.g. "http://host:port/mcp" */
    char *token;     /* optional bearer token (NULL = none) */
} ca_mcp_conn;

typedef struct ca_mcp_manager ca_mcp_manager;

ca_mcp_manager *ca_mcp_manager_new(void);
void ca_mcp_manager_free(ca_mcp_manager *m);

/* Register/update a named connection. 0 ok, -1 invalid. */
int ca_mcp_manager_add(ca_mcp_manager *m, const char *name, const char *url, const char *token);
int ca_mcp_manager_remove(ca_mcp_manager *m, const char *name);

/* Borrowed lookups (valid until next mutation). */
const ca_mcp_conn *ca_mcp_manager_find(ca_mcp_manager *m, const char *name);
int ca_mcp_manager_count(ca_mcp_manager *m);
const ca_mcp_conn *ca_mcp_manager_get(ca_mcp_manager *m, size_t i);

/* Invoke `tool` on the named server with JSON args. Returns the raw JSON
 * response body (malloc'd; caller frees), or NULL on connection/HTTP error. */
char *ca_mcp_manager_call(ca_mcp_manager *m, const char *name,
                          const char *tool, const char *args_json);

/* JSON array of connections {name,url,has_token} (malloc'd; caller frees). */
char *ca_mcp_manager_json(ca_mcp_manager *m);

#ifdef __cplusplus
}
#endif
