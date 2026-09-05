/* router.c — multi-provider model routing with pluggable selection policy
 * (round_robin / cost / latency / capability:<tag>). */
#include "cognitive-os-agent/llm/router.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct coa_router {
    coa_mutex mtx;
    coa_route *routes;
    size_t count, cap;
    size_t cursor;      /* round-robin cursor */
    size_t tie_cursor;  /* cursor within a tied best-class */
    char *policy;       /* "round_robin" | "cost" | "latency" | "capability:<tag>" */
};

coa_router *coa_router_new(void) {
    coa_router *r = (coa_router *)calloc(1, sizeof(coa_router));
    if (!r) return NULL;
    coa_mutex_init(&r->mtx);
    r->policy = coa_strdup("round_robin");
    return r;
}

void route_clear(coa_route *e) {
    free(e->name); free(e->provider); free(e->base_url);
    free(e->api_key); free(e->model); free(e->caps);
    memset(e, 0, sizeof(*e));
}

void coa_router_free(coa_router *r) {
    if (!r) return;
    coa_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) route_clear(&r->routes[i]);
    free(r->routes);
    coa_mutex_unlock(&r->mtx);
    coa_mutex_destroy(&r->mtx);
    free(r->policy);
    free(r);
}

int coa_router_add(coa_router *r, const char *name, const char *provider,
                  const char *base_url, const char *api_key,
                  const char *model, double weight) {
    return coa_router_add_ex(r, name, provider, base_url, api_key, model,
                            weight, 0, 0, NULL);
}

int coa_router_add_ex(coa_router *r, const char *name, const char *provider,
                     const char *base_url, const char *api_key,
                     const char *model, double weight,
                     int cost_rank, int latency_ms, const char *caps) {
    if (!r || !name || !provider) return -1;
    if (weight <= 0) weight = 1.0;
    coa_mutex_lock(&r->mtx);
    /* upsert: replace an existing route with the same name */
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->routes[i].name, name) == 0) {
            coa_route *e = &r->routes[i];
            free(e->provider); free(e->base_url); free(e->api_key);
            free(e->model); free(e->caps);
            e->provider = coa_strdup(provider);
            e->base_url = base_url ? coa_strdup(base_url) : NULL;
            e->api_key = api_key ? coa_strdup(api_key) : NULL;
            e->model = model ? coa_strdup(model) : NULL;
            e->caps = caps ? coa_strdup(caps) : NULL;
            e->weight = weight;
            e->cost_rank = cost_rank;
            e->latency_ms = latency_ms;
            coa_mutex_unlock(&r->mtx);
            return 0;
        }
    }
    if (r->count == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 8;
        coa_route *nr = (coa_route *)realloc(r->routes, ncap * sizeof(coa_route));
        if (!nr) { coa_mutex_unlock(&r->mtx); return -1; }
        r->routes = nr;
        r->cap = ncap;
    }
    coa_route *e = &r->routes[r->count++];
    memset(e, 0, sizeof(*e));
    e->name = coa_strdup(name);
    e->provider = coa_strdup(provider);
    e->base_url = base_url ? coa_strdup(base_url) : NULL;
    e->api_key = api_key ? coa_strdup(api_key) : NULL;
    e->model = model ? coa_strdup(model) : NULL;
    e->caps = caps ? coa_strdup(caps) : NULL;
    e->weight = weight;
    e->cost_rank = cost_rank;
    e->latency_ms = latency_ms;
    coa_mutex_unlock(&r->mtx);
    return 0;
}

int coa_router_remove(coa_router *r, const char *name) {
    if (!r || !name) return 0;
    coa_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->routes[i].name, name) == 0) {
            route_clear(&r->routes[i]);
            memmove(&r->routes[i], &r->routes[i + 1],
                    (r->count - i - 1) * sizeof(coa_route));
            r->count--;
            coa_mutex_unlock(&r->mtx);
            return 1;
        }
    }
    coa_mutex_unlock(&r->mtx);
    return 0;
}

int coa_router_set_policy(coa_router *r, const char *policy) {
    if (!r) return -1;
    const char *p = (policy && *policy) ? policy : "round_robin";
    if (strcmp(p, "round_robin") != 0 && strcmp(p, "cost") != 0 &&
        strcmp(p, "latency") != 0 && strncmp(p, "capability:", 11) != 0)
        return -1;
    coa_mutex_lock(&r->mtx);
    free(r->policy);
    r->policy = coa_strdup(p);
    coa_mutex_unlock(&r->mtx);
    return 0;
}

const char *coa_router_policy(coa_router *r) {
    if (!r) return "round_robin";
    coa_mutex_lock(&r->mtx);
    const char *p = r->policy ? r->policy : "round_robin";
    coa_mutex_unlock(&r->mtx);
    return p;
}

/* Does a route carry the capability tag (comma-separated exact match)? */
static int route_has_cap(const coa_route *e, const char *tag) {
    if (!e->caps) return 0;
    size_t tlen = strlen(tag);
    const char *p = e->caps;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == tlen && strncmp(p, tag, len) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* Pick under lock. Metrics-only policies restrict to configured routes and
 * choose the best value; equal-value ties rotate (secondary round-robin). */
static coa_route *pick_locked(coa_router *r) {
    if (r->count == 0) return NULL;
    const char *pol = r->policy ? r->policy : "round_robin";
    int is_cost = strcmp(pol, "cost") == 0;
    int is_lat = strcmp(pol, "latency") == 0;
    int is_cap = strncmp(pol, "capability:", 11) == 0;
    if (!is_cost && !is_lat && !is_cap) {
        /* weighted round-robin: advance by the current route's weight */
        coa_route *e = &r->routes[r->cursor % r->count];
        size_t step = (e->weight >= 1.0) ? (size_t)e->weight : 1;
        r->cursor = (r->cursor + step) % r->count;
        return e;
    }

    size_t cand[64];
    size_t n = 0;
    if (is_cap) {
        const char *tag = pol + 11;
        for (size_t i = 0; i < r->count && n < 64; i++)
            if (route_has_cap(&r->routes[i], tag)) cand[n++] = i;
        if (n == 0) { /* nobody carries the tag: degrade to full round-robin */
            for (size_t i = 0; i < r->count && n < 64; i++) cand[n++] = i;
            coa_route *e = &r->routes[cand[r->cursor++ % n]];
            return e;
        }
    } else {
        for (size_t i = 0; i < r->count && n < 64; i++) cand[n++] = i;
    }

    /* best metric value (lower = better; 0 = unknown, never preferred) */
    size_t best = cand[0];
    for (size_t i = 1; i < n; i++) {
        coa_route *a = &r->routes[cand[i]], *b = &r->routes[best];
        int av = is_lat ? a->latency_ms : a->cost_rank;
        int bv = is_lat ? b->latency_ms : b->cost_rank;
        if (av > 0 && (bv <= 0 || av < bv)) best = cand[i];
    }
    int best_v = is_lat ? r->routes[best].latency_ms : r->routes[best].cost_rank;
    if (best_v > 0) {
        size_t ties[64];
        size_t nt = 0;
        for (size_t i = 0; i < n; i++) {
            coa_route *e = &r->routes[cand[i]];
            int v = is_lat ? e->latency_ms : e->cost_rank;
            if (v == best_v) ties[nt++] = cand[i];
        }
        if (nt > 1) {
            r->tie_cursor = (r->tie_cursor + 1) % nt;
            return &r->routes[ties[r->tie_cursor]];
        }
    }
    return &r->routes[best];
}

const coa_route *coa_router_pick(coa_router *r) {
    if (!r) return NULL;
    coa_mutex_lock(&r->mtx);
    coa_route *e = pick_locked(r);
    if (e) e->calls++;
    coa_mutex_unlock(&r->mtx);
    return e;
}

int coa_router_count(coa_router *r) {
    if (!r) return 0;
    coa_mutex_lock(&r->mtx);
    int n = (int)r->count;
    coa_mutex_unlock(&r->mtx);
    return n;
}

const coa_route *coa_router_get(coa_router *r, size_t i) {
    if (!r) return NULL;
    coa_mutex_lock(&r->mtx);
    const coa_route *e = (i < r->count) ? &r->routes[i] : NULL;
    coa_mutex_unlock(&r->mtx);
    return e;
}

char *coa_router_json(coa_router *r) {
    cJSON *arr = cJSON_CreateArray();
    if (!r) return cJSON_PrintUnformatted(arr);
    coa_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        coa_route *e = &r->routes[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "provider", e->provider);
        if (e->base_url) cJSON_AddStringToObject(o, "base_url", e->base_url);
        if (e->model) cJSON_AddStringToObject(o, "model", e->model);
        if (e->api_key) cJSON_AddStringToObject(o, "api_key", e->api_key);
        cJSON_AddNumberToObject(o, "weight", e->weight);
        cJSON_AddNumberToObject(o, "calls", (double)e->calls);
        cJSON_AddNumberToObject(o, "cost_rank", e->cost_rank);
        cJSON_AddNumberToObject(o, "latency_ms", e->latency_ms);
        if (e->caps) cJSON_AddStringToObject(o, "caps", e->caps);
        cJSON_AddItemToArray(arr, o);
    }
    coa_mutex_unlock(&r->mtx);
    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return js;
}

int coa_router_save_file(coa_router *r, const char *path) {
    if (!r || !path) return -1;
    char *js = coa_router_json(r);
    if (!js) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { free(js); return -1; }
    size_t n = strlen(js);
    size_t w = fwrite(js, 1, n, f);
    fclose(f);
    free(js);
    return (w == n) ? 0 : -1;
}

int coa_router_load_file(coa_router *r, const char *path) {
    if (!r || !path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1; /* no saved routes (first run) */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return -1; }
    /* collect routes first, then add them *without* holding r->mtx (coa_router_add
     * locks it internally — holding it here would deadlock on a non-recursive mutex) */
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsObject(it)) continue;
        cJSON *name = cJSON_GetObjectItemCaseSensitive(it, "name");
        cJSON *prov = cJSON_GetObjectItemCaseSensitive(it, "provider");
        if (!name || !cJSON_IsString(name) || !prov || !cJSON_IsString(prov)) continue;
        cJSON *b = cJSON_GetObjectItemCaseSensitive(it, "base_url");
        cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "model");
        cJSON *k = cJSON_GetObjectItemCaseSensitive(it, "api_key");
        cJSON *w = cJSON_GetObjectItemCaseSensitive(it, "weight");
        cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "cost_rank");
        cJSON *l = cJSON_GetObjectItemCaseSensitive(it, "latency_ms");
        cJSON *cp = cJSON_GetObjectItemCaseSensitive(it, "caps");
        double weight = (w && cJSON_IsNumber(w)) ? w->valuedouble : 1.0;
        int cost = (c && cJSON_IsNumber(c)) ? (int)c->valuedouble : 0;
        int lat = (l && cJSON_IsNumber(l)) ? (int)l->valuedouble : 0;
        const char *caps = (cp && cJSON_IsString(cp)) ? cp->valuestring : NULL;
        coa_router_add_ex(r, name->valuestring, prov->valuestring,
                         (b && cJSON_IsString(b)) ? b->valuestring : NULL,
                         (k && cJSON_IsString(k)) ? k->valuestring : NULL,
                         (m && cJSON_IsString(m)) ? m->valuestring : NULL,
                         weight, cost, lat, caps);
    }
    cJSON_Delete(arr);
    return 0;
}
