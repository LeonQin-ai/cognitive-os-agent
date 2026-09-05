/* node.h — cluster node registry (distributed coordination).
 * Tracks worker/observer nodes participating in the Cognitive OS cluster:
 * identity, endpoint, role, liveness state and last-seen heartbeat. Nodes are
 * marked down after a configurable staleness window by coa_cluster_mark_down. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_cluster coa_cluster;

typedef struct coa_cluster_node {
    char *id;
    char *host;
    uint16_t port;
    char *role;         /* "coordinator" | "worker" | "observer" */
    char *status;       /* "up" | "down" | "suspect" */
    char *caps;         /* comma-separated capability tags (e.g. "llm,tools") */
    int64_t last_seen_ms;
} coa_cluster_node;

coa_cluster *coa_cluster_new(void);
void coa_cluster_free(coa_cluster *c);

/* Register a node or update its endpoint/role (keeps liveness). 0 ok, -1 invalid. */
int coa_cluster_upsert(coa_cluster *c, const char *id, const char *host,
                      uint16_t port, const char *role);
/* Same, with capability tags (comma-separated; NULL/"" = none). */
int coa_cluster_upsert_ex(coa_cluster *c, const char *id, const char *host,
                         uint16_t port, const char *role, const char *caps);
int coa_cluster_remove(coa_cluster *c, const char *id);

/* Record a heartbeat: last_seen_ms = now, status = "up". 0 ok, -1 unknown id. */
int coa_cluster_heartbeat(coa_cluster *c, const char *id);
/* Mark every node whose last heartbeat is older than stale_ms as "down". */
void coa_cluster_mark_down(coa_cluster *c, int64_t stale_ms);

const coa_cluster_node *coa_cluster_find(coa_cluster *c, const char *id);
int coa_cluster_count(coa_cluster *c);
const coa_cluster_node *coa_cluster_get(coa_cluster *c, size_t i);
int coa_cluster_up_count(coa_cluster *c);

/* JSON array of nodes {id,host,port,role,status,caps,last_seen_ms} (caller frees). */
char *coa_cluster_json(coa_cluster *c);

#ifdef __cplusplus
}
#endif
