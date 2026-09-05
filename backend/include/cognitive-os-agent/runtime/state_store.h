/* state_store.h — Context layer: unified KV / Task / Agent state.
 * Architecture v1.0 §5: the Context MMU maintains several state slots
 * (KV State / Task State / Agent State / ...). They share one namespaced,
 * thread-safe store here so any slot can be read, written and persisted
 * through the same API and REST surface. Namespaces are free-form strings
 * ("kv", "task", "agent", ...). Persistence is a single JSON file under the
 * state root; writes auto-flush once a path has been set via save/load. */
#pragma once
#include <stddef.h>
#ifdef _WIN32
#include <basetsd.h> /* long long on MSVC */
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_state_store ca_state_store;

ca_state_store *ca_state_store_new(void);
void ca_state_store_free(ca_state_store *s);

/* Generic namespaced KV. val == NULL removes the key. Returns 0 ok,
 * -1 bad args. Borrowed values stay valid until the next mutation. */
int ca_state_store_set(ca_state_store *s, const char *ns, const char *key,
                       const char *val);
const char *ca_state_store_get(ca_state_store *s, const char *ns, const char *key);
int ca_state_store_remove(ca_state_store *s, const char *ns, const char *key);
int ca_state_store_count(ca_state_store *s);
int ca_state_store_count_ns(ca_state_store *s, const char *ns);

/* Architecture state slots:
 *  - task:  key = "<id>", value = "<status>|<input>"   (queued -> terminal)
 *  - agent: key = name,   value = "<role>|<status>"    */
int ca_state_store_task_set(ca_state_store *s, long long id, const char *status,
                            const char *input);
int ca_state_store_agent_set(ca_state_store *s, const char *name,
                             const char *role, const char *status);

/* Whole store as {"ns":{"key":"val",...},...} (malloc'd, caller frees). */
char *ca_state_store_json(ca_state_store *s);
/* Merge entries from that JSON shape. Returns entries applied, -1 bad args. */
int ca_state_store_load_json(ca_state_store *s, const char *json);

/* Persist to <path> / load (merge) from <path>; after either call the store
 * auto-flushes on every mutation. Returns 0 ok, -1 bad args/IO. */
int ca_state_store_save(ca_state_store *s, const char *path);
int ca_state_store_load(ca_state_store *s, const char *path);

#ifdef __cplusplus
}
#endif
