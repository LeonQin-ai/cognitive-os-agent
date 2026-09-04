/* agent.h — multi-agent coordinator sharing a blackboard.
 * A ca_agent_pool registers named agents (with roles) that publish partial
 * results onto a shared ca_blackboard and read each other's contributions. */
#pragma once
#include "cagent/cognition/blackboard.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_agent_pool ca_agent_pool;

ca_agent_pool *ca_agent_pool_new(void);
void ca_agent_pool_free(ca_agent_pool *p);

/* Register a named agent with an optional role description. Returns its index
 * (>= 0) or -1 on bad args. */
int ca_agent_pool_add(ca_agent_pool *p, const char *name, const char *role);

/* Like ca_agent_pool_add, but also records the model/provider this agent
 * should use (stored for display and future per-agent execution). Either may
 * be NULL (falls back to the global active model). */
int ca_agent_pool_add_model(ca_agent_pool *p, const char *name, const char *role,
                            const char *provider, const char *model);
int ca_agent_pool_count(ca_agent_pool *p);

/* Swap in an externally owned blackboard (caller keeps ownership and frees
 * it; the pool borrows it). Used to share one blackboard between the runtime
 * context and the agent pool. */
void ca_agent_pool_adopt_blackboard(ca_agent_pool *p, ca_blackboard *b);

/* Borrow the shared blackboard (owned by the pool; do not free). */
ca_blackboard *ca_agent_pool_blackboard(ca_agent_pool *p);

/* Index of the named agent, or -1 if unknown. */
int ca_agent_pool_find(ca_agent_pool *p, const char *name);

/* Publish a key/value fact tagged with an agent. Returns 0 ok, -1 if the agent
 * is unknown or args are NULL/empty. */
int ca_agent_post(ca_agent_pool *p, const char *agent, const char *key, const char *val);

/* All agents (name + role) and the shared facts as one JSON object. Caller frees. */
char *ca_agent_pool_snapshot_json(ca_agent_pool *p);

/* Persist the agent roster (name/role/provider/model, not facts) as
 * <dir>/agents.json. Returns 0 ok, -1 on write failure. */
int ca_agent_pool_save(ca_agent_pool *p, const char *dir);
/* Load a roster previously written by ca_agent_pool_save (duplicate names are
 * skipped). Returns the number of agents loaded, or -1 on read failure. */
int ca_agent_pool_load(ca_agent_pool *p, const char *dir);

#ifdef __cplusplus
}
#endif
