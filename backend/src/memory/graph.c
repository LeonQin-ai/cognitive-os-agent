/* graph.c — lightweight knowledge graph store. */
#include "cognitive-os-agent/memory/graph.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

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

struct coa_graph {
    coa_mutex mtx;
    gnode *nodes;
    size_t n_nodes, cap_nodes;
    gedge *edges;
    size_t n_edges, cap_edges;
};

coa_graph *coa_graph_new(void) {
    coa_graph *g = (coa_graph *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    coa_mutex_init(&g->mtx);
    return g;
}

void coa_graph_free(coa_graph *g) {
    if (!g) return;
    coa_mutex_lock(&g->mtx);
    for (size_t i = 0; i < g->n_nodes; i++) { free(g->nodes[i].id); free(g->nodes[i].label); }
    for (size_t i = 0; i < g->n_edges; i++) { free(g->edges[i].from); free(g->edges[i].to); free(g->edges[i].relation); }
    free(g->nodes);
    free(g->edges);
    g->nodes = NULL;
    g->edges = NULL;
    g->n_nodes = g->cap_nodes = g->n_edges = g->cap_edges = 0;
    coa_mutex_unlock(&g->mtx);
    coa_mutex_destroy(&g->mtx);
    free(g);
}

static int find_node(coa_graph *g, const char *id) {
    for (size_t i = 0; i < g->n_nodes; i++)
        if (strcmp(g->nodes[i].id, id) == 0) return (int)i;
    return -1;
}

int coa_graph_add_node(coa_graph *g, const char *id, const char *label) {
    if (!g || !id || !*id) return -1;
    coa_mutex_lock(&g->mtx);
    if (find_node(g, id) >= 0) { coa_mutex_unlock(&g->mtx); return -1; }
    if (g->n_nodes == g->cap_nodes) {
        size_t cap = g->cap_nodes ? g->cap_nodes * 2 : 8;
        gnode *nn = (gnode *)realloc(g->nodes, cap * sizeof(gnode));
        if (!nn) { coa_mutex_unlock(&g->mtx); return -1; }
        g->nodes = nn;
        g->cap_nodes = cap;
    }
    g->nodes[g->n_nodes].id = coa_strdup(id);
    g->nodes[g->n_nodes].label = coa_strdup(label ? label : "");
    g->n_nodes++;
    coa_mutex_unlock(&g->mtx);
    return 0;
}

int coa_graph_add_edge(coa_graph *g, const char *from, const char *to, const char *relation) {
    if (!g || !from || !to) return -1;
    coa_mutex_lock(&g->mtx);
    /* dedup: identical labeled edges are folded (idempotent recording) */
    for (size_t i = 0; i < g->n_edges; i++) {
        if (strcmp(g->edges[i].from, from) == 0 &&
            strcmp(g->edges[i].to, to) == 0 &&
            strcmp(g->edges[i].relation, relation ? relation : "") == 0) {
            coa_mutex_unlock(&g->mtx);
            return 0;
        }
    }
    if (g->n_edges == g->cap_edges) {
        size_t cap = g->cap_edges ? g->cap_edges * 2 : 8;
        gedge *ne = (gedge *)realloc(g->edges, cap * sizeof(gedge));
        if (!ne) { coa_mutex_unlock(&g->mtx); return -1; }
        g->edges = ne;
        g->cap_edges = cap;
    }
    g->edges[g->n_edges].from = coa_strdup(from);
    g->edges[g->n_edges].to = coa_strdup(to);
    g->edges[g->n_edges].relation = coa_strdup(relation ? relation : "");
    g->n_edges++;
    coa_mutex_unlock(&g->mtx);
    return 0;
}

int coa_graph_node_count(coa_graph *g) {
    if (!g) return 0;
    coa_mutex_lock(&g->mtx);
    int n = (int)g->n_nodes;
    coa_mutex_unlock(&g->mtx);
    return n;
}

int coa_graph_edge_count(coa_graph *g) {
    if (!g) return 0;
    coa_mutex_lock(&g->mtx);
    int n = (int)g->n_edges;
    coa_mutex_unlock(&g->mtx);
    return n;
}

char *coa_graph_neighbors(coa_graph *g, const char *id) {
    if (!g || !id) return coa_strdup("[]");
    coa_mutex_lock(&g->mtx);
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
    coa_mutex_unlock(&g->mtx);
    return s ? s : coa_strdup("[]");
}

char *coa_graph_snapshot_json(coa_graph *g) {
    if (!g) return coa_strdup("{}");
    coa_mutex_lock(&g->mtx);
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
    coa_mutex_unlock(&g->mtx);
    return s ? s : coa_strdup("{}");
}
