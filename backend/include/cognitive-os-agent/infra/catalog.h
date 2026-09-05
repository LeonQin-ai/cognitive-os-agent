/* catalog.h — curated catalogs: free models + MCP server plaza.
 * Server-driven presets so the console can offer "free model" and "MCP
 * marketplace" one-click wiring without hardcoding data in the UI. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* JSON array of free/cheap model presets:
 *   [{id,name,provider,base_url,model,key_hint,note}] */
char *ca_catalog_models_json(void);

/* JSON array of MCP server plaza entries:
 *   [{id,name,url,description,category,needs_local,key_hint,repo}] */
char *ca_catalog_mcp_json(void);

#ifdef __cplusplus
}
#endif
