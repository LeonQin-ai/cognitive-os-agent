/* router.h — multi-provider model routing.
 * Holds named routes (provider + endpoint + model) with weights and picks the
 * next route per the configured policy: weighted round-robin (default),
 * lowest cost_rank, lowest latency_ms, or capability-tag filtered round-robin.
 * A NULL router means "single provider" (the existing llm path). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct router router;

typedef struct route {
    char *name;
    char *provider; /* "openai" | "anthropic" | "mock" */
    char *base_url;
    char *api_key;
    char *model;
    double weight;
    long long calls; /* number of times picked */
    int cost_rank;   /* 1 = cheapest; 0 = unknown (excluded from cost policy) */
    int latency_ms;  /* nominal latency; 0 = unknown (excluded from latency policy) */
    char *caps;      /* comma-separated capability tags ("tools,json,long_ctx") */
} route;

router *router_new(void);
void router_free(router *r);

int router_add(router *r, const char *name, const char *provider, const char *base_url, const char *api_key,
                   const char *model, double weight);
/* Full form: cost_rank (1=cheapest, 0 unknown), latency_ms (0 unknown),
 * caps = comma-separated capability tags (may be NULL). */
int router_add_ex(router *r, const char *name, const char *provider, const char *base_url, const char *api_key,
                      const char *model, double weight, int cost_rank, int latency_ms, const char *caps);
/* Remove a route by name (no-op if absent). Returns 1 if removed, 0 otherwise. */
int router_remove(router *r, const char *name);

/* Routing policy (persists on the router):
 *   "round_robin" (default/NULL/"")  weighted round-robin
 *   "cost"                           lowest cost_rank first, ties round-robin
 *   "latency"                        lowest latency_ms first, ties round-robin
 *   "capability:<tag>"               round-robin among routes carrying <tag>
 * Unknown values fall back to round_robin. Returns 0 ok, -1 bad args. */
int router_set_policy(router *r, const char *policy);
const char *router_policy(router *r);

/* Pick the next route per policy (borrowed). NULL if empty. */
const route *router_pick(router *r);

int router_count(router *r);
const route *router_get(router *r, size_t i);
/* JSON array of routes (caller frees). */
char *router_json(router *r);

/* Persistence: save/load the route table to a JSON file so configured models
 * survive restarts. load_file only adds routes (does not clear existing). */
int router_save_file(router *r, const char *path);
int router_load_file(router *r, const char *path);

#ifdef __cplusplus
}
#endif
