/* api_rest.h — REST API layer on top of the HTTP server.
 * Routes:
 *   POST /v1/tasks               {"prompt":"..."} -> submit to scheduler
 *   GET  /v1/tasks/{id}          task status + output
 *   GET  /v1/tools               registered tool list
 *   GET  /v1/memory              working + long-term memory
 *   GET  /v1/snapshots           committed snapshots
 *   POST /v1/snapshots/rollback  restore latest snapshot
 *   GET  /v1/trace               span traces (observability)
 *   GET  /v1/routes              model route table
 *   GET  /v1/usage               per-model token accounting
 *   GET  /v1/plugins             versioned plugin registry
 *   GET  /v1/skills              static skills
 *   POST /v1/skills/run          execute a skill {"name":...}
 *   GET  /v1/mcp                 MCP connections
 *   GET  /v1/cluster             cluster node registry
 *   GET  /metrics                Prometheus text
 *   GET  /                       embedded web UI
 */
#pragma once
#include "cagent/cagent.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the HTTP server (if ctx->http_port > 0) and register routes.
 * Returns 0 ok, -1 if the server could not be started. */
int cagent_api_attach(cagent_ctx *ctx);

#ifdef __cplusplus
}
#endif
