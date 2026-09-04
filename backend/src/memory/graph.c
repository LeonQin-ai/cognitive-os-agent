/* graph.c — lightweight knowledge graph store. */
#include "cagent/memory/graph.h"
#include "cagent/os/os_thread.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

typedef struct gnode {
    char *id;
    char *label;
} gnode;

typedef struct gedge {
    char *from;
    char *to;
    char *relation;
} gedge;

struct ca_graph {
    ca_mutex mtx;
    gnode *nodes;
    size_t n_nodes, cap_nodes;
    gedge *edges;
    size_t n_edges, cap_edges;
};

ca_graph *ca_graph_new(void) {
    ca_graph *g = (ca_graph *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    ca_mutex_init(&g->mtx);
    return g;
}

void ca_graph_free(ca_graph *g) {
    if (!g) return;
    ca_mutex_lock(&g->mtx);
    for (size_t i = 0; i < g->n_nodes; i++) { free(g->nodes[i].id); free(g->nodes[i].label); }
    for (size_t i = 0; i < g->n_edges; i++) { free(g->edges[i].from); free(g->edges[i].to); free(g->edges[i].relation); }
    free(g->nodes);
    free(g->edges);
    g->nodes = NULL;
    g->edges = NULL;
    g->n_nodes = g->cap_nodes = g->n_edges = g->cap_edges = 0;
    ca_mutex_unlock(&g->mtx);
    ca_mutex_destroy(&g->mtx);
    free(g);
}

static int find_node(ca_graph *g, const char *id) {
    for (size_t i = 0; i < g->n_nodes; i++)
        if (strcmp(g->nodes[i].id, id) == 0) return (int)i;
    return -1;
}

int ca_graph_add_node(ca_graph *g, const char *id, const char *label) {
    if (!g || !id || !*id) return -1;
    ca_mutex_lock(&g->mtx);
    if (find_node(g, id) >= 0) { ca_mutex_unlock(&g->mtx); return -1; }
    if (g->n_nodes == g->cap_nodes) {
        size_t cap = g->cap_nodes ? g->cap_nodes * 2 : 8;
        gnode *nn = (gnode *)realloc(g->nodes, cap * sizeof(gnode));
        if (!nn) { ca_mutex_unlock(&g->mtx); return -1; }
        g->nodes = nn;
        g->cap_nodes = cap;
    }
    g->nodes[g->n_nodes].id = ca_strdup(id);
    g->nodes[g->n_nodes].label = ca_strdup(label ? label : "");
    g->n_nodes++;
    ca_mutex_unlock(&g->mtx);
    return 0;
}

int ca_graph_add_edge(ca_graph *g, const char *from, const char *to, const char *relation) {
    if (!g || !from || !to) return -1;
    ca_mutex_lock(&g->mtx);
    /* dedup: identical labeled edges are folded (idempotent recording) */
    for (size_t i = 0; i < g->n_edges; i++) {
        if (strcmp(g->edges[i].from, from) == 0 &&
            strcmp(g->edges[i].to, to) == 0 &&
            strcmp(g->edges[i].relation, relation ? relation : "") == 0) {
            ca_mutex_unlock(&g->mtx);
            return 0;
        }
    }
    if (g->n_edges == g->cap_edges) {
        size_t cap = g->cap_edges ? g->cap_edges * 2 : 8;
        gedge *ne = (gedge *)realloc(g->edges, cap * sizeof(gedge));
        if (!ne) { ca_mutex_unlock(&g->mtx); return -1; }
        g->edges = ne;
        g->cap_edges = cap;
    }
    g->edges[g->n_edges].from = ca_strdup(from);
    g->edges[g->n_edges].to = ca_strdup(to);
    g->edges[g->n_edges].relation = ca_strdup(relation ? relation : "");
    g->n_edges++;
    ca_mutex_unlock(&g->mtx);
    return 0;
}

int ca_graph_node_count(ca_graph *g) {
    if (!g) return 0;
    ca_mutex_lock(&g->mtx);
    int n = (int)g->n_nodes;
    ca_mutex_unlock(&g->mtx);
    return n;
}

int ca_graph_edge_count(ca_graph *g) {
    if (!g) return 0;
    ca_mutex_lock(&g->mtx);
    int n = (int)g->n_edges;
    ca_mutex_unlock(&g->mtx);
    return n;
}

char *ca_graph_neighbors(ca_graph *g, const char *id) {
    if (!g || !id) return ca_strdup("[]");
    ca_mutex_lock(&g->mtx);
    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (size_t i = 0; i < g->n_edges; i++) {
            if (strcmp(g->edges[i].from, id) == 0) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "to", g->edges[i].to);
                cJSON_AddStringToObject(o, "relation", g->edges[i].relation);
                cJSON_AddItemToArray(arr, o);
            }
        }
    }
    char *s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr) cJSON_Delete(arr);
    ca_mutex_unlock(&g->mtx);
    return s ? s : ca_strdup("[]");
}

char *ca_graph_snapshot_json(ca_graph *g) {
    if (!g) return ca_strdup("{}");
    ca_mutex_lock(&g->mtx);
    cJSON *root = cJSON_CreateObject();
    cJSON *nodes = cJSON_CreateArray();
    cJSON *edges = cJSON_CreateArray();
    if (root && nodes && edges) {
        cJSON_AddItemToObject(root, "nodes", nodes);
        cJSON_AddItemToObject(root, "edges", edges);
        for (size_t i = 0; i < g->n_nodes; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", g->nodes[i].id);
            cJSON_AddStringToObject(o, "label", g->nodes[i].label);
            cJSON_AddItemToArray(nodes, o);
        }
        for (size_t i = 0; i < g->n_edges; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "from", g->edges[i].from);
            cJSON_AddStringToObject(o, "to", g->edges[i].to);
            cJSON_AddStringToObject(o, "relation", g->edges[i].relation);
            cJSON_AddItemToArray(edges, o);
        }
    }
    char *s = root ? cJSON_PrintUnformatted(root) : NULL;
    if (root) cJSON_Delete(root);
    ca_mutex_unlock(&g->mtx);
    return s ? s : ca_strdup("{}");
}
