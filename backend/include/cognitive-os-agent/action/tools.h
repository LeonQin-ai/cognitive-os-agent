/* tools.h — tool registry and dispatch.
 * A tool is a named, schema-described function taking a JSON args string and
 * returning a result. Write tools are wrapped by the transaction layer so the
 * snapshot engine can roll them back. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_tool ca_tool;
typedef struct ca_tool_registry ca_tool_registry;
typedef struct ca_tool_ctx ca_tool_ctx;
typedef struct ca_snapshot ca_snapshot;
typedef struct ca_tx ca_tx;
typedef struct ca_policy_engine ca_policy_engine;
typedef struct ca_event_bus ca_event_bus;
typedef struct ca_metrics ca_metrics;

typedef struct ca_tool_result {
    int ok;          /* 1 success, 0 failure/denied */
    char *output;    /* text output (malloc'd) */
} ca_tool_result;

typedef ca_tool_result *(*ca_tool_exec_fn)(const ca_tool *self, const ca_tool_ctx *ctx,
                                           const char *args_json);

typedef struct ca_tool {
    const char *name;
    const char *description;
    const char *json_schema;   /* JSON schema string (may be NULL) */
    int is_write;              /* 1 if it modifies the filesystem/system */
    ca_tool_exec_fn execute;
    void *ud;                  /* tool-private closure (dynamic MCP tools) */
} ca_tool;

struct ca_skill_registry;   /* skill.h (avoid a typedef collision across TUs) */

/* Execution context handed to every tool call. */
typedef struct ca_tool_ctx {
    ca_tool_registry *reg;
    ca_policy_engine *policy;  /* permission checks (may be NULL = allow all) */
    ca_snapshot *snapshot;     /* for write tracking (may be NULL) */
    ca_tx *tx;                 /* active transaction (may be NULL) */
    ca_event_bus *bus;         /* event publisher (may be NULL) */
    const char *workspace;     /* base dir for relative paths */
    ca_metrics *metrics;       /* metrics sink (may be NULL) */
    struct ca_skill_registry *skills;  /* for the skill tool (may be NULL) */
    struct ca_mcp_manager *mcp;        /* for MCP tools (may be NULL) */
} ca_tool_ctx;

ca_tool_registry *ca_tool_registry_new(void);
void ca_tool_registry_free(ca_tool_registry *reg);
int ca_tool_register(ca_tool_registry *reg, const ca_tool *tool);
const ca_tool *ca_tool_find(ca_tool_registry *reg, const char *name);

/* Iteration (used by the API to list tools). */
int ca_tool_registry_count(const ca_tool_registry *reg);
const ca_tool *ca_tool_registry_get(const ca_tool_registry *reg, size_t i);

/* Execute a named tool after policy check. Returns a malloc'd result (never NULL). */
ca_tool_result *ca_tool_execute(ca_tool_registry *reg, const char *name,
                                const char *args_json, const ca_tool_ctx *ctx);

/* Lightweight JSON-Schema validation of tool args (subset: object type,
 * properties.<k>.type, required). 0 = valid, -1 = invalid (err_out receives a
 * malloc'd message). A tool with no json_schema always validates. */
int ca_tool_validate_args(const ca_tool *tool, const char *args_json, char **err_out);

/* Register with upsert semantics: when replace is 1 an existing tool with the
 * same name is swapped out (used for dynamic MCP tool registration). */
int ca_tool_register_ex(ca_tool_registry *reg, const ca_tool *tool, int replace);
void ca_tool_result_free(ca_tool_result *r);

/* Convenience constructor used by tool implementations. Caller frees. */
ca_tool_result *ca_tool_result_new(int ok, const char *output);

/* Built-in tool factories (defined in action/). Return static tools. */
const ca_tool *ca_tool_file_read(void);
const ca_tool *ca_tool_file_write(void);
const ca_tool *ca_tool_file_edit(void);
const ca_tool *ca_tool_shell(void);
const ca_tool *ca_tool_git(void);
const ca_tool *ca_tool_mcp(void);
const ca_tool *ca_tool_skill(void);
const ca_tool *ca_tool_glob(void);
const ca_tool *ca_tool_grep(void);

/* Register the built-in tools (incl. the skill tool) into a registry. */
void ca_tool_register_builtins(ca_tool_registry *reg);

/* Bind a generated plugin (registered as `skill_name` in the skill registry)
 * as a callable tool named `tool_name` — used by the missing-capability
 * self-evolution loop. 0 ok, -1 bad args / OOM. */
int ca_tool_register_generated(ca_tool_registry *reg, struct ca_skill_registry *skills,
                               const char *tool_name, const char *skill_name);

/* Persist (upsert) a generated tool -> skill binding to
 * <state_root>/generated_tools.json so it survives restarts. 0 ok. */
int ca_tool_generated_save_mapping(const char *state_root, const char *tool,
                                   const char *skill);
/* Load the raw generated_tools.json content (malloc'd; caller frees).
 * NULL if missing / no state_root. */
char *ca_tool_generated_load_mapping(const char *state_root);

#ifdef __cplusplus
}
#endif
