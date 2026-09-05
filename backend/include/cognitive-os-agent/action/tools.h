/* tools.h — tool registry and dispatch.
 * A tool is a named, schema-described function taking a JSON args string and
 * returning a result. Write tools are wrapped by the transaction layer so the
 * snapshot engine can roll them back. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_tool coa_tool;
typedef struct coa_tool_registry coa_tool_registry;
typedef struct coa_tool_ctx coa_tool_ctx;
typedef struct coa_snapshot coa_snapshot;
typedef struct coa_tx coa_tx;
typedef struct coa_policy_engine coa_policy_engine;
typedef struct coa_event_bus coa_event_bus;
typedef struct coa_metrics coa_metrics;

typedef struct coa_tool_result {
    int ok;          /* 1 success, 0 failure/denied */
    char *output;    /* text output (malloc'd) */
} coa_tool_result;

typedef coa_tool_result *(*coa_tool_exec_fn)(const coa_tool *self, const coa_tool_ctx *ctx,
                                           const char *args_json);

typedef struct coa_tool {
    const char *name;
    const char *description;
    const char *json_schema;   /* JSON schema string (may be NULL) */
    int is_write;              /* 1 if it modifies the filesystem/system */
    coa_tool_exec_fn execute;
    void *ud;                  /* tool-private closure (dynamic MCP tools) */
} coa_tool;

struct coa_skill_registry;   /* skill.h (avoid a typedef collision across TUs) */

/* Execution context handed to every tool call. */
typedef struct coa_tool_ctx {
    coa_tool_registry *reg;
    coa_policy_engine *policy;  /* permission checks (may be NULL = allow all) */
    coa_snapshot *snapshot;     /* for write tracking (may be NULL) */
    coa_tx *tx;                 /* active transaction (may be NULL) */
    coa_event_bus *bus;         /* event publisher (may be NULL) */
    const char *workspace;     /* base dir for relative paths */
    coa_metrics *metrics;       /* metrics sink (may be NULL) */
    struct coa_skill_registry *skills;  /* for the skill tool (may be NULL) */
    struct coa_mcp_manager *mcp;        /* for MCP tools (may be NULL) */
} coa_tool_ctx;

coa_tool_registry *coa_tool_registry_new(void);
void coa_tool_registry_free(coa_tool_registry *reg);
int coa_tool_register(coa_tool_registry *reg, const coa_tool *tool);
const coa_tool *coa_tool_find(coa_tool_registry *reg, const char *name);

/* Iteration (used by the API to list tools). */
int coa_tool_registry_count(const coa_tool_registry *reg);
const coa_tool *coa_tool_registry_get(const coa_tool_registry *reg, size_t i);

/* Execute a named tool after policy check. Returns a malloc'd result (never NULL). */
coa_tool_result *coa_tool_execute(coa_tool_registry *reg, const char *name,
                                const char *args_json, const coa_tool_ctx *ctx);

/* Lightweight JSON-Schema validation of tool args (subset: object type,
 * properties.<k>.type, required). 0 = valid, -1 = invalid (err_out receives a
 * malloc'd message). A tool with no json_schema always validates. */
int coa_tool_validate_args(const coa_tool *tool, const char *args_json, char **err_out);

/* Register with upsert semantics: when replace is 1 an existing tool with the
 * same name is swapped out (used for dynamic MCP tool registration). */
int coa_tool_register_ex(coa_tool_registry *reg, const coa_tool *tool, int replace);
void coa_tool_result_free(coa_tool_result *r);

/* Convenience constructor used by tool implementations. Caller frees. */
coa_tool_result *coa_tool_result_new(int ok, const char *output);

/* Built-in tool factories (defined in action/). Return static tools. */
const coa_tool *coa_tool_file_read(void);
const coa_tool *coa_tool_file_write(void);
const coa_tool *coa_tool_file_edit(void);
const coa_tool *coa_tool_shell(void);
const coa_tool *coa_tool_git(void);
const coa_tool *coa_tool_mcp(void);
const coa_tool *coa_tool_skill(void);
const coa_tool *coa_tool_glob(void);
const coa_tool *coa_tool_grep(void);

/* Register the built-in tools (incl. the skill tool) into a registry. */
void coa_tool_register_builtins(coa_tool_registry *reg);

/* Bind a generated plugin (registered as `skill_name` in the skill registry)
 * as a callable tool named `tool_name` — used by the missing-capability
 * self-evolution loop. 0 ok, -1 bad args / OOM. */
int coa_tool_register_generated(coa_tool_registry *reg, struct coa_skill_registry *skills,
                               const char *tool_name, const char *skill_name);

/* Persist (upsert) a generated tool -> skill binding to
 * <state_root>/generated_tools.json so it survives restarts. 0 ok. */
int coa_tool_generated_save_mapping(const char *state_root, const char *tool,
                                   const char *skill);
/* Load the raw generated_tools.json content (malloc'd; caller frees).
 * NULL if missing / no state_root. */
char *coa_tool_generated_load_mapping(const char *state_root);

#ifdef __cplusplus
}
#endif
