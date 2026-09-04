/* hook.h — horizontal hook system (v1.0 architecture §17).
 *
 * A registry of named-event callbacks that lets builtin components and third
 * parties (plugins, external REST clients) observe — and for before_* events
 * intercept — runtime behavior WITHOUT touching core code.
 *
 * Events fired by the runtime:
 *   agent.before_run     {prompt}              (blocking: nonzero = skip run)
 *   agent.after_run      {prompt,status,answer}
 *   agent.on_error       {prompt,status}
 *   agent.on_state_change{state}                (each cognitive stage entry)
 *   exec.before_execute  {tool,args}           (blocking: nonzero = skip tool)
 *   exec.after_execute   {tool,ok}
 *   exec.on_failure      {tool,args}
 *
 * Dispatch matches the exact event name and the wildcard "*". A hook returning
 * nonzero blocks before_* actions (dispatch reports 1); for other events the
 * return value is recorded but ignored.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_hook_registry ca_hook_registry;

/* Return 0 = allow/ok, nonzero = block (only honored for before_* events). */
typedef int (*ca_hook_fn)(const char *event, const char *payload_json, void *ud);

ca_hook_registry *ca_hook_registry_new(void);
void ca_hook_registry_free(ca_hook_registry *h);

/* Register a callback for `event` (or "*" for all events). Returns a hook id
 * (>0), or -1 on bad args / OOM. Thread-safe. */
int ca_hook_register(ca_hook_registry *h, const char *event, ca_hook_fn fn, void *ud);

/* Remove a previously registered hook. Returns 0 ok, -1 not found. */
int ca_hook_unregister(ca_hook_registry *h, int id);

/* Dispatch `event` with `payload_json` (may be NULL / arbitrary text) to all
 * matching hooks in registration order. Returns 0 = all allowed, 1 = a hook
 * blocked (nonzero return), -1 = bad args. Thread-safe. */
int ca_hook_dispatch(ca_hook_registry *h, const char *event, const char *payload_json);

/* Registered hooks as a JSON array [{"id":N,"event":"..."}]. Caller frees. */
char *ca_hook_registry_json(ca_hook_registry *h);

/* Builtin audit hook: appends {"ts_ms","event","payload"} as one JSON line to
 * the file whose path is passed as `ud` (opens/appends/closes per call).
 * Register with event "*" to audit everything. */
int ca_hook_audit_file(const char *event, const char *payload_json, void *ud);

#ifdef __cplusplus
}
#endif
