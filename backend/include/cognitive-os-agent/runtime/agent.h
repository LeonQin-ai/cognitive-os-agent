/* agent.h — multi-agent coordinator sharing a blackboard.
 * A coa_agent_pool registers named agents (with roles) that publish partial
 * results onto a shared coa_blackboard and read each other's contributions. */
#pragma once
#include "cognitive-os-agent/cognition/blackboard.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_agent_pool coa_agent_pool;

coa_agent_pool *coa_agent_pool_new(void);
void coa_agent_pool_free(coa_agent_pool *p);

/* Register a named agent with an optional role description. Returns its index
 * (>= 0) or -1 on bad args. */
int coa_agent_pool_add(coa_agent_pool *p, const char *name, const char *role);

/* Like coa_agent_pool_add, but also records the model/provider this agent
 * should use (stored for display and future per-agent execution). Either may
 * be NULL (falls back to the global active model). */
int coa_agent_pool_add_model(coa_agent_pool *p, const char *name, const char *role,
                            const char *provider, const char *model);
int coa_agent_pool_count(coa_agent_pool *p);

/* Remove a registered agent by name. Returns 0 ok, -1 if unknown/bad args. */
int coa_agent_pool_remove(coa_agent_pool *p, const char *name);

/* Swap in an externally owned blackboard (caller keeps ownership and frees
 * it; the pool borrows it). Used to share one blackboard between the runtime
 * context and the agent pool. */
void coa_agent_pool_adopt_blackboard(coa_agent_pool *p, coa_blackboard *b);

/* Borrow the shared blackboard (owned by the pool; do not free). */
coa_blackboard *coa_agent_pool_blackboard(coa_agent_pool *p);

/* Index of the named agent, or -1 if unknown. */
int coa_agent_pool_find(coa_agent_pool *p, const char *name);

/* Publish a key/value fact tagged with an agent. Returns 0 ok, -1 if the agent
 * is unknown or args are NULL/empty. */
int coa_agent_post(coa_agent_pool *p, const char *agent, const char *key, const char *val);

/* All agents (name + role) and the shared facts as one JSON object. Caller frees. */
char *coa_agent_pool_snapshot_json(coa_agent_pool *p);

/* Persist the agent roster (name/role/provider/model, not facts) as
 * <dir>/agents.json. Returns 0 ok, -1 on write failure. */
int coa_agent_pool_save(coa_agent_pool *p, const char *dir);
/* Load a roster previously written by coa_agent_pool_save (duplicate names are
 * skipped). Returns the number of agents loaded, or -1 on read failure. */
int coa_agent_pool_load(coa_agent_pool *p, const char *dir);

#ifdef __cplusplus
}
#endif
