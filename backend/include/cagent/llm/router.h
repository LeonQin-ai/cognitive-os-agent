/* router.h — multi-provider model routing.
 * Holds named routes (provider + endpoint + model) with weights and picks the
 * next route in a weighted round-robin. A NULL router means "single provider"
 * (the existing ca_llm path); this adds dynamic routing on top. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_router ca_router;

typedef struct ca_route {
    char *name;
    char *provider;   /* "openai" | "anthropic" | "mock" */
    char *base_url;
    char *api_key;
    char *model;
    double weight;
    long long calls;  /* number of times picked */
} ca_route;

ca_router *ca_router_new(void);
void ca_router_free(ca_router *r);

int ca_router_add(ca_router *r, const char *name, const char *provider,
                  const char *base_url, const char *api_key,
                  const char *model, double weight);
/* Remove a route by name (no-op if absent). Returns 1 if removed, 0 otherwise. */
int ca_router_remove(ca_router *r, const char *name);
/* Weighted round-robin: returns the next route (borrowed). NULL if empty. */
const ca_route *ca_router_pick(ca_router *r);

int ca_router_count(ca_router *r);
const ca_route *ca_router_get(ca_router *r, size_t i);
/* JSON array of routes (caller frees). */
char *ca_router_json(ca_router *r);

#ifdef __cplusplus
}
#endif
