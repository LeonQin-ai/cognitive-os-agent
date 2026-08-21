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

typedef ca_tool_result *(*ca_tool_exec_fn)(const ca_tool_ctx *ctx, const char *args_json);

typedef struct ca_tool {
    const char *name;
    const char *description;
    const char *json_schema;   /* JSON schema string (may be NULL) */
    int is_write;              /* 1 if it modifies the filesystem/system */
    ca_tool_exec_fn execute;
} ca_tool;

/* Execution context handed to every tool call. */
typedef struct ca_tool_ctx {
    ca_tool_registry *reg;
    ca_policy_engine *policy;  /* permission checks (may be NULL = allow all) */
    ca_snapshot *snapshot;     /* for write tracking (may be NULL) */
    ca_tx *tx;                 /* active transaction (may be NULL) */
    ca_event_bus *bus;         /* event publisher (may be NULL) */
    const char *workspace;     /* base dir for relative paths */
    ca_metrics *metrics;       /* metrics sink (may be NULL) */
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
void ca_tool_result_free(ca_tool_result *r);

/* Convenience constructor used by tool implementations. Caller frees. */
ca_tool_result *ca_tool_result_new(int ok, const char *output);

/* Built-in tool factories (defined in action/). Return static tools. */
const ca_tool *ca_tool_file_read(void);
const ca_tool *ca_tool_file_write(void);
const ca_tool *ca_tool_shell(void);
const ca_tool *ca_tool_git(void);
const ca_tool *ca_tool_mcp(void);

/* Register the five built-in tools into a registry. */
void ca_tool_register_builtins(ca_tool_registry *reg);

#ifdef __cplusplus
}
#endif
