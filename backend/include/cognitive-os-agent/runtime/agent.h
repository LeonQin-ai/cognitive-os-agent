/* agent.h — multi-agent coordinator sharing a blackboard.
 * A agent_pool registers named agents (with roles) that publish partial
 * results onto a shared blackboard and read each other's contributions. */
#pragma once
#include "cognitive-os-agent/cognition/blackboard.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_pool agent_pool;

agent_pool *agent_pool_new(void);
void agent_pool_free(agent_pool *p);

/* Register a named agent with an optional role description. Returns its index
 * (>= 0) or -1 on bad args. */
int agent_pool_add(agent_pool *p, const char *name, const char *role);

/* Like agent_pool_add, but also records the model/provider this agent
 * should use (stored for display and future per-agent execution). Either may
 * be NULL (falls back to the global active model). */
int agent_pool_add_model(agent_pool *p, const char *name, const char *role, const char *provider,
                             const char *model);
int agent_pool_count(agent_pool *p);

/* Remove a registered agent by name. Returns 0 ok, -1 if unknown/bad args. */
int agent_pool_remove(agent_pool *p, const char *name);

/* Swap in an externally owned blackboard (caller keeps ownership and frees
 * it; the pool borrows it). Used to share one blackboard between the runtime
 * context and the agent pool. */
void agent_pool_adopt_blackboard(agent_pool *p, blackboard *b);

/* Borrow the shared blackboard (owned by the pool; do not free). */
blackboard *agent_pool_blackboard(agent_pool *p);

/* Index of the named agent, or -1 if unknown. */
int agent_pool_find(agent_pool *p, const char *name);

/* Publish a key/value fact tagged with an agent. Returns 0 ok, -1 if the agent
 * is unknown or args are NULL/empty. */
int agent_post(agent_pool *p, const char *agent, const char *key, const char *val);

/* All agents (name + role) and the shared facts as one JSON object. Caller frees. */
char *agent_pool_snapshot_json(agent_pool *p);

/* Persist the agent roster (name/role/provider/model, not facts) as
 * <dir>/agents.json. Returns 0 ok, -1 on write failure. */
int agent_pool_save(agent_pool *p, const char *dir);
/* Load a roster previously written by agent_pool_save (duplicate names are
 * skipped). Returns the number of agents loaded, or -1 on read failure. */
int agent_pool_load(agent_pool *p, const char *dir);

#ifdef __cplusplus
}
#endif
