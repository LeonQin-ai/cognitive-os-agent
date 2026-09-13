/* node.h — cluster node registry (distributed coordination).
 * Tracks worker/observer nodes participating in the Cognitive OS cluster:
 * identity, endpoint, role, liveness state and last-seen heartbeat. Nodes are
 * marked down after a configurable staleness window by cluster_mark_down. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cluster cluster;

typedef struct cluster_node {
    char *id;
    char *host;
    uint16_t port;
    char *role;   /* "coordinator" | "worker" | "observer" */
    char *status; /* "up" | "down" | "suspect" */
    char *caps;   /* comma-separated capability tags (e.g. "llm,tools") */
    int64_t last_seen_ms;
} cluster_node;

cluster *cluster_new(void);
void cluster_free(cluster *c);

/* Register a node or update its endpoint/role (keeps liveness). 0 ok, -1 invalid. */
int cluster_upsert(cluster *c, const char *id, const char *host, uint16_t port, const char *role);
/* Same, with capability tags (comma-separated; NULL/"" = none). */
int cluster_upsert_ex(cluster *c, const char *id, const char *host, uint16_t port, const char *role,
                          const char *caps);
int cluster_remove(cluster *c, const char *id);

/* Record a heartbeat: last_seen_ms = now, status = "up". 0 ok, -1 unknown id. */
int cluster_heartbeat(cluster *c, const char *id);
/* Mark every node whose last heartbeat is older than stale_ms as "down". */
void cluster_mark_down(cluster *c, int64_t stale_ms);

const cluster_node *cluster_find(cluster *c, const char *id);
int cluster_count(cluster *c);
int cluster_up_count(cluster *c);

/* JSON array of nodes {id,host,port,role,status,caps,last_seen_ms} (caller frees). */
char *cluster_json(cluster *c);

#ifdef __cplusplus
}
#endif
