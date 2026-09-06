/* catalog.h — curated catalogs: free models + MCP server plaza.
 * Server-driven presets so the console can offer "free model" and "MCP
 * marketplace" one-click wiring without hardcoding data in the UI. */
#pragma once

#include "cognitive-os-agent/action/skill.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JSON array of free/cheap model presets:
 *   [{id,name,provider,base_url,model,key_hint,note}] */
char *coa_catalog_models_json(void);

/* JSON array of MCP server plaza entries:
 *   [{id,name,url,description,category,needs_local,key_hint,repo}] */
char *coa_catalog_mcp_json(void);

/* Curated skills plaza entry. kind=="reference" entries point at upstream
 * skill/prompt repos (fabric, skillhub, …) and cannot be installed/executed;
 * the rest are runnable shell/python skills installable via /v1/skills/install. */
typedef struct catalog_skill {
    const char *id;
    const char *name;
    const char *description;
    const char *kind;     /* shell | python | reference */
    const char *body;     /* skill body ("" for reference entries) */
    const char *test_args;/* JSON args binding used by tests/UI ("" = none) */
    const char *source;   /* inspiration repo ("" = built-in) */
} catalog_skill;

int coa_catalog_skill_count(void);
const catalog_skill *coa_catalog_skill_at(int i);

/* JSON array of curated skills plaza entries:
 *   [{id,name,description,kind,body,test_args,source,type}] */
char *coa_catalog_skills_json(void);

#ifdef __cplusplus
}
#endif
