/* graph.h — lightweight knowledge graph (nodes + labeled directed edges). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_graph ca_graph;

ca_graph *ca_graph_new(void);
void ca_graph_free(ca_graph *g);

/* Add a node (id -> label); id must be unique. Returns 0 ok, -1 dup/arg. */
int ca_graph_add_node(ca_graph *g, const char *id, const char *label);
/* Add a directed edge (from -> to, relation). Returns 0 ok, -1 bad args. */
int ca_graph_add_edge(ca_graph *g, const char *from, const char *to, const char *relation);
int ca_graph_node_count(ca_graph *g);
int ca_graph_edge_count(ca_graph *g);

/* Outgoing neighbors of `id` as a JSON array of {to,relation} (caller frees). */
char *ca_graph_neighbors(ca_graph *g, const char *id);
/* Whole graph as {nodes:[{id,label}],edges:[{from,to,relation}]} (caller frees). */
char *ca_graph_snapshot_json(ca_graph *g);

#ifdef __cplusplus
}
#endif
