/* graph.h — lightweight knowledge graph (nodes + labeled directed edges). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct graph graph;

graph *graph_new(void);
void graph_free(graph *g);

/* Add a node (id -> label); id must be unique. Returns 0 ok, -1 dup/arg. */
int graph_add_node(graph *g, const char *id, const char *label);
/* Add a directed edge (from -> to, relation). Returns 0 ok, -1 bad args. */
int graph_add_edge(graph *g, const char *from, const char *to, const char *relation);
int graph_node_count(graph *g);
int graph_edge_count(graph *g);

/* Outgoing neighbors of `id` as a JSON array of {to,relation} (caller frees). */
char *graph_neighbors(graph *g, const char *id);
/* Whole graph as {nodes:[{id,label}],edges:[{from,to,relation}]} (caller frees). */
char *graph_snapshot_json(graph *g);

#ifdef __cplusplus
}
#endif
