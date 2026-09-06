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

/* GitHub remote skill: a real file in a live upstream repo, fetched over
 * HTTPS at install time and registered as a prompt-kind skill. */
typedef struct catalog_remote_skill {
    const char *repo;         /* grouping key shown in the UI ("fabric"…) */
    const char *id;           /* runtime skill name (ascii) */
    const char *name;         /* display name */
    const char *description;
    const char *raw_url;      /* direct https URL to the skill file */
    const char *fallback_url; /* mirror (ghproxy) tried when direct fails */
} catalog_remote_skill;

int coa_catalog_remote_skill_count(void);
const catalog_remote_skill *coa_catalog_remote_skill_at(int i);
/* Find by repo+id (either may be NULL = wildcard). NULL when not found. */
const catalog_remote_skill *coa_catalog_remote_skill_find(const char *repo,
                                                          const char *id);

/* JSON array of remote skill entries:
 *   [{repo,id,name,description,raw_url,fallback_url}] */
char *coa_catalog_remote_skills_json(void);

/* Download the skill file for `e` (direct first, then fallback mirror).
 * Returns the malloc'd text (truncated to 64 KB) or NULL on failure.
 * Blocking network I/O — call from a worker, not the HTTP thread. */
char *coa_catalog_remote_skill_fetch(const catalog_remote_skill *e);

/* skillhub.cn (技能市场) live catalog. Both do blocking network I/O against
 * api.skillhub.cn — call from a worker, not the HTTP thread. */

/* Fetch the skillhub skill-package listing (first page, 40 entries) and
 * return a normalized JSON array:
 *   [{id,name,description,skill_count}]
 * Returns NULL on network/parse failure. */
char *coa_catalog_skillhub_list_json(void);

/* Download SKILL.md content for a skillhub slug. Slug is validated against
 * [A-Za-z0-9._-]. Returns the malloc'd markdown (truncated to 64 KB) or
 * NULL on failure / bad slug. */
char *coa_catalog_skillhub_fetch_skill(const char *slug);

#ifdef __cplusplus
}
#endif
