/* node.h — cluster node registry (distributed coordination).
 * Tracks worker/observer nodes participating in the Cognitive OS cluster:
 * identity, endpoint, role, liveness state and last-seen heartbeat. Nodes are
 * marked down after a configurable staleness window by ca_cluster_mark_down. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_cluster ca_cluster;

typedef struct ca_cluster_node {
    char *id;
    char *host;
    uint16_t port;
    char *role;         /* "coordinator" | "worker" | "observer" */
    char *status;       /* "up" | "down" | "suspect" */
    int64_t last_seen_ms;
} ca_cluster_node;

ca_cluster *ca_cluster_new(void);
void ca_cluster_free(ca_cluster *c);

/* Register a node or update its endpoint/role (keeps liveness). 0 ok, -1 invalid. */
int ca_cluster_upsert(ca_cluster *c, const char *id, const char *host,
                      uint16_t port, const char *role);
int ca_cluster_remove(ca_cluster *c, const char *id);

/* Record a heartbeat: last_seen_ms = now, status = "up". 0 ok, -1 unknown id. */
int ca_cluster_heartbeat(ca_cluster *c, const char *id);
/* Mark every node whose last heartbeat is older than stale_ms as "down". */
void ca_cluster_mark_down(ca_cluster *c, int64_t stale_ms);

const ca_cluster_node *ca_cluster_find(ca_cluster *c, const char *id);
int ca_cluster_count(ca_cluster *c);
const ca_cluster_node *ca_cluster_get(ca_cluster *c, size_t i);
int ca_cluster_up_count(ca_cluster *c);

/* JSON array of nodes {id,host,port,role,status,last_seen_ms} (caller frees). */
char *ca_cluster_json(ca_cluster *c);

#ifdef __cplusplus
}
#endif
