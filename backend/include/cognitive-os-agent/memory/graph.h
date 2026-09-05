/* graph.h — lightweight knowledge graph (nodes + labeled directed edges). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_graph coa_graph;

coa_graph *coa_graph_new(void);
void coa_graph_free(coa_graph *g);

/* Add a node (id -> label); id must be unique. Returns 0 ok, -1 dup/arg. */
int coa_graph_add_node(coa_graph *g, const char *id, const char *label);
/* Add a directed edge (from -> to, relation). Returns 0 ok, -1 bad args. */
int coa_graph_add_edge(coa_graph *g, const char *from, const char *to, const char *relation);
int coa_graph_node_count(coa_graph *g);
int coa_graph_edge_count(coa_graph *g);

/* Outgoing neighbors of `id` as a JSON array of {to,relation} (caller frees). */
char *coa_graph_neighbors(coa_graph *g, const char *id);
/* Whole graph as {nodes:[{id,label}],edges:[{from,to,relation}]} (caller frees). */
char *coa_graph_snapshot_json(coa_graph *g);

#ifdef __cplusplus
}
#endif
