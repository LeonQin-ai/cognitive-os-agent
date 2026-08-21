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
int ca_agent_pool_count(ca_agent_pool *p);

/* Borrow the shared blackboard (owned by the pool; do not free). */
ca_blackboard *ca_agent_pool_blackboard(ca_agent_pool *p);

/* Publish a key/value fact tagged with an agent. Returns 0 ok, -1 if the agent
 * is unknown or args are NULL/empty. */
int ca_agent_post(ca_agent_pool *p, const char *agent, const char *key, const char *val);

/* All agents (name + role) and the shared facts as one JSON object. Caller frees. */
char *ca_agent_pool_snapshot_json(ca_agent_pool *p);

#ifdef __cplusplus
}
#endif
