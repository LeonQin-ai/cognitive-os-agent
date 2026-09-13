/* tools.h — tool registry and dispatch.
 * A tool is a named, schema-described function taking a JSON args string and
 * returning a result. Write tools are wrapped by the transaction layer so the
 * snapshot engine can roll them back. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool tool;
typedef struct tool_registry tool_registry;
typedef struct tool_ctx tool_ctx;
typedef struct snapshot snapshot;
typedef struct tx tx;
typedef struct policy_engine policy_engine;
typedef struct event_bus event_bus;
typedef struct metrics metrics;

typedef struct tool_result {
    int ok;       /* 1 success, 0 failure/denied */
    char *output; /* text output (malloc'd) */
} tool_result;

typedef tool_result *(*tool_exec_fn)(const tool *self, const tool_ctx *ctx, const char *args_json);

typedef struct tool {
    const char *name;
    const char *description;
    const char *json_schema; /* JSON schema string (may be NULL) */
    int is_write;            /* 1 if it modifies the filesystem/system */
    tool_exec_fn execute;
    void *ud; /* tool-private closure (dynamic MCP tools) */
} tool;

struct skill_registry; /* skill.h (avoid a typedef collision across TUs) */

/* Execution context handed to every tool call. */
typedef struct tool_ctx {
    tool_registry *reg;
    policy_engine *policy;         /* permission checks (may be NULL = allow all) */
    snapshot *snapshot;            /* for write tracking (may be NULL) */
    tx *tx;                        /* active transaction (may be NULL) */
    event_bus *bus;                /* event publisher (may be NULL) */
    const char *workspace;             /* base dir for relative paths */
    metrics *metrics;              /* metrics sink (may be NULL) */
    struct skill_registry *skills; /* for the skill tool (may be NULL) */
    struct mcp_manager *mcp;       /* for MCP tools (may be NULL) */
} tool_ctx;

tool_registry *tool_registry_new(void);
void tool_registry_free(tool_registry *reg);
int tool_register(tool_registry *reg, const tool *tool);
const tool *tool_find(tool_registry *reg, const char *name);

/* Iteration (used by the API to list tools). */
int tool_registry_count(const tool_registry *reg);
const tool *tool_registry_get(const tool_registry *reg, size_t i);

/* Execute a named tool after policy check. Returns a malloc'd result (never NULL). */
tool_result *tool_execute(tool_registry *reg, const char *name, const char *args_json,
                                  const tool_ctx *ctx);

/* Lightweight JSON-Schema validation of tool args (subset: object type,
 * properties.<k>.type, required). 0 = valid, -1 = invalid (err_out receives a
 * malloc'd message). A tool with no json_schema always validates. */
int tool_validate_args(const tool *tool, const char *args_json, char **err_out);

/* Register with upsert semantics: when replace is 1 an existing tool with the
 * same name is swapped out (used for dynamic MCP tool registration). */
int tool_register_ex(tool_registry *reg, const tool *t, int replace);
/* Remove a tool by name (registry drops its pointer only; the struct itself
 * stays owned by whoever registered it). Returns 1 removed, 0 not found. */
int tool_unregister(tool_registry *reg, const char *name);
void tool_result_free(tool_result *r);

/* Convenience constructor used by tool implementations. Caller frees. */
tool_result *tool_result_new(int ok, const char *output);

/* Built-in tool factories (defined in action/). Return static tools. */
const tool *tool_file_read(void);
const tool *tool_file_write(void);
const tool *tool_file_edit(void);
const tool *tool_shell(void);
const tool *tool_git(void);
const tool *tool_mcp(void);
const tool *tool_skill(void);
const tool *tool_glob(void);
const tool *tool_grep(void);

/* Register the built-in tools (incl. the skill tool) into a registry. */
void tool_register_builtins(tool_registry *reg);

/* Bind a generated plugin (registered as `skill_name` in the skill registry)
 * as a callable tool named `tool_name` — used by the missing-capability
 * self-evolution loop. 0 ok, -1 bad args / OOM. */
int tool_register_generated(tool_registry *reg, struct skill_registry *skills, const char *tool_name,
                                const char *skill_name);

/* Persist (upsert) a generated tool -> skill binding to
 * <state_root>/generated_tools.json so it survives restarts. 0 ok. */
int tool_generated_save_mapping(const char *state_root, const char *tool, const char *skill);
/* Load the raw generated_tools.json content (malloc'd; caller frees).
 * NULL if missing / no state_root. */
char *tool_generated_load_mapping(const char *state_root);

#ifdef __cplusplus
}
#endif
