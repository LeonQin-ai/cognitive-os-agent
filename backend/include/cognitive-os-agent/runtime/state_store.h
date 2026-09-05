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

typedef struct coa_state_store coa_state_store;

coa_state_store *coa_state_store_new(void);
void coa_state_store_free(coa_state_store *s);

/* Generic namespaced KV. val == NULL removes the key. Returns 0 ok,
 * -1 bad args. Borrowed values stay valid until the next mutation. */
int coa_state_store_set(coa_state_store *s, const char *ns, const char *key,
                       const char *val);
const char *coa_state_store_get(coa_state_store *s, const char *ns, const char *key);
int coa_state_store_remove(coa_state_store *s, const char *ns, const char *key);
int coa_state_store_count(coa_state_store *s);
int coa_state_store_count_ns(coa_state_store *s, const char *ns);

/* Architecture state slots:
 *  - task:  key = "<id>", value = "<status>|<input>"   (queued -> terminal)
 *  - agent: key = name,   value = "<role>|<status>"    */
int coa_state_store_task_set(coa_state_store *s, long long id, const char *status,
                            const char *input);
int coa_state_store_agent_set(coa_state_store *s, const char *name,
                             const char *role, const char *status);

/* Whole store as {"ns":{"key":"val",...},...} (malloc'd, caller frees). */
char *coa_state_store_json(coa_state_store *s);
/* Merge entries from that JSON shape. Returns entries applied, -1 bad args. */
int coa_state_store_load_json(coa_state_store *s, const char *json);

/* Persist to <path> / load (merge) from <path>; after either call the store
 * auto-flushes on every mutation. Returns 0 ok, -1 bad args/IO. */
int coa_state_store_save(coa_state_store *s, const char *path);
int coa_state_store_load(coa_state_store *s, const char *path);

#ifdef __cplusplus
}
#endif
