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

struct graph {
    mutex_t mtx;
    gnode *nodes;
    size_t n_nodes, cap_nodes;
    gedge *edges;
    size_t n_edges, cap_edges;
};

graph *graph_new(void) {
    graph *g = (graph *)calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    mutex_init(&g->mtx);
    return g;
}

void graph_free(graph *g) {
    if (!g)
        return;
    mutex_lock(&g->mtx);
    for (size_t i = 0; i < g->n_nodes; i++) {
        free(g->nodes[i].id);
        free(g->nodes[i].label);
    }

    for (size_t i = 0; i < g->n_edges; i++) {
        free(g->edges[i].from);
        free(g->edges[i].to);
        free(g->edges[i].relation);
    }

    free(g->nodes);
    free(g->edges);
    g->nodes = NULL;
    g->edges = NULL;
    g->n_nodes = g->cap_nodes = g->n_edges = g->cap_edges = 0;
    mutex_unlock(&g->mtx);
    mutex_destroy(&g->mtx);
    free(g);
}

static int find_node(graph *g, const char *id) {
    for (size_t i = 0; i < g->n_nodes; i++)
        if (strcmp(g->nodes[i].id, id) == 0)
            return (int)i;
    return -1;
}

int graph_add_node(graph *g, const char *id, const char *label) {
    if (!g || !id || !*id)
        return -1;
    mutex_lock(&g->mtx);
    if (find_node(g, id) >= 0) {
        mutex_unlock(&g->mtx);
        return -1;
    }

    if (g->n_nodes == g->cap_nodes) {
        size_t cap = g->cap_nodes ? g->cap_nodes * 2 : 8;
        gnode *nn = (gnode *)realloc(g->nodes, cap * sizeof(gnode));
        if (!nn) {
            mutex_unlock(&g->mtx);
            return -1;
        }
        g->nodes = nn;
        g->cap_nodes = cap;
    }

    g->nodes[g->n_nodes].id = xstrdup(id);
    g->nodes[g->n_nodes].label = xstrdup(label ? label : "");
    g->n_nodes++;
    mutex_unlock(&g->mtx);
    return 0;
}

int graph_add_edge(graph *g, const char *from, const char *to, const char *relation) {
    if (!g || !from || !to)
        return -1;
    mutex_lock(&g->mtx);
    /* dedup: identical labeled edges are folded (idempotent recording) */
    for (size_t i = 0; i < g->n_edges; i++) {
        if (strcmp(g->edges[i].from, from) == 0 && strcmp(g->edges[i].to, to) == 0 &&
            strcmp(g->edges[i].relation, relation ? relation : "") == 0) {
            mutex_unlock(&g->mtx);
            return 0;
        }
    }

    if (g->n_edges == g->cap_edges) {
        size_t cap = g->cap_edges ? g->cap_edges * 2 : 8;
        gedge *ne = (gedge *)realloc(g->edges, cap * sizeof(gedge));
        if (!ne) {
            mutex_unlock(&g->mtx);
            return -1;
        }
        g->edges = ne;
        g->cap_edges = cap;
    }

    g->edges[g->n_edges].from = xstrdup(from);
    g->edges[g->n_edges].to = xstrdup(to);
    g->edges[g->n_edges].relation = xstrdup(relation ? relation : "");
    g->n_edges++;
    mutex_unlock(&g->mtx);
    return 0;
}

int graph_node_count(graph *g) {
    int n;

    if (!g)
        return 0;
    mutex_lock(&g->mtx);
    n = (int)g->n_nodes;
    mutex_unlock(&g->mtx);
    return n;
}

int graph_edge_count(graph *g) {
    int n;

    if (!g)
        return 0;
    mutex_lock(&g->mtx);
    n = (int)g->n_edges;
    mutex_unlock(&g->mtx);
    return n;
}

char *graph_neighbors(graph *g, const char *id) {
    cJSON *arr;
    char *s;

    if (!g || !id)
        return xstrdup("[]");
    mutex_lock(&g->mtx);
    arr = cJSON_CreateArray();
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

    s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr)
        cJSON_Delete(arr);
    mutex_unlock(&g->mtx);
    return s ? s : xstrdup("[]");
}

char *graph_snapshot_json(graph *g) {
    cJSON *root;
    cJSON *nodes;
    cJSON *edges;
    char *s;

    if (!g)
        return xstrdup("{}");
    mutex_lock(&g->mtx);
    root = cJSON_CreateObject();
    nodes = cJSON_CreateArray();
    edges = cJSON_CreateArray();
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

    s = root ? cJSON_PrintUnformatted(root) : NULL;
    if (root)
        cJSON_Delete(root);
    mutex_unlock(&g->mtx);
    return s ? s : xstrdup("{}");
}
