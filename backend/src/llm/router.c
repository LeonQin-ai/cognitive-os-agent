/* router.c — multi-provider model routing with pluggable selection policy
 * (round_robin / cost / latency / capability:<tag>). */
#include "cognitive-os-agent/llm/router.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct router {
    mutex_t mtx;
    route *routes;
    size_t count, cap;
    size_t cursor;     /* round-robin cursor */
    size_t tie_cursor; /* cursor within a tied best-class */
    char *policy;      /* "round_robin" | "cost" | "latency" | "capability:<tag>" */
};

router *router_new(void) {
    router *r = (router *)calloc(1, sizeof(router));
    if (!r)
        return NULL;
    mutex_init(&r->mtx);
    r->policy = xstrdup("round_robin");
    return r;
}

void route_clear(route *e) {
    free(e->name);
    free(e->provider);
    free(e->base_url);
    free(e->api_key);
    free(e->model);
    free(e->caps);
    memset(e, 0, sizeof(*e));
}

void router_free(router *r) {
    if (!r)
        return;
    mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++)
        route_clear(&r->routes[i]);
    free(r->routes);
    mutex_unlock(&r->mtx);
    mutex_destroy(&r->mtx);
    free(r->policy);
    free(r);
}

int router_add(router *r, const char *name, const char *provider, const char *base_url, const char *api_key,
                   const char *model, double weight) {
    return router_add_ex(r, name, provider, base_url, api_key, model, weight, 0, 0, NULL);
}

int router_add_ex(router *r, const char *name, const char *provider, const char *base_url, const char *api_key,
                      const char *model, double weight, int cost_rank, int latency_ms, const char *caps) {
    route *e;

    if (!r || !name || !provider)
        return -1;
    if (weight <= 0)
        weight = 1.0;
    mutex_lock(&r->mtx);
    /* upsert: replace an existing route with the same name */
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->routes[i].name, name) == 0) {
            route *e = &r->routes[i];
            free(e->provider);
            free(e->base_url);
            free(e->api_key);
            free(e->model);
            free(e->caps);
            e->provider = xstrdup(provider);
            e->base_url = base_url ? xstrdup(base_url) : NULL;
            e->api_key = api_key ? xstrdup(api_key) : NULL;
            e->model = model ? xstrdup(model) : NULL;
            e->caps = caps ? xstrdup(caps) : NULL;
            e->weight = weight;
            e->cost_rank = cost_rank;
            e->latency_ms = latency_ms;
            mutex_unlock(&r->mtx);
            return 0;
        }
    }

    if (r->count == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 8;
        route *nr = (route *)realloc(r->routes, ncap * sizeof(route));
        if (!nr) {
            mutex_unlock(&r->mtx);
            return -1;
        }
        r->routes = nr;
        r->cap = ncap;
    }

    e = &r->routes[r->count++];
    memset(e, 0, sizeof(*e));
    e->name = xstrdup(name);
    e->provider = xstrdup(provider);
    e->base_url = base_url ? xstrdup(base_url) : NULL;
    e->api_key = api_key ? xstrdup(api_key) : NULL;
    e->model = model ? xstrdup(model) : NULL;
    e->caps = caps ? xstrdup(caps) : NULL;
    e->weight = weight;
    e->cost_rank = cost_rank;
    e->latency_ms = latency_ms;
    mutex_unlock(&r->mtx);
    return 0;
}

int router_remove(router *r, const char *name) {
    if (!r || !name)
        return 0;
    mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->routes[i].name, name) == 0) {
            route_clear(&r->routes[i]);
            memmove(&r->routes[i], &r->routes[i + 1], (r->count - i - 1) * sizeof(route));
            r->count--;
            mutex_unlock(&r->mtx);
            return 1;
        }
    }

    mutex_unlock(&r->mtx);
    return 0;
}

int router_set_policy(router *r, const char *policy) {
    const char *p = (policy && *policy) ? policy : "round_robin";

    if (!r)
        return -1;
    if (strcmp(p, "round_robin") != 0 && strcmp(p, "cost") != 0 && strcmp(p, "latency") != 0 &&
        strncmp(p, "capability:", 11) != 0)
        return -1;
    mutex_lock(&r->mtx);
    free(r->policy);
    r->policy = xstrdup(p);
    mutex_unlock(&r->mtx);
    return 0;
}

const char *router_policy(router *r) {
    if (!r)
        return "round_robin";
    mutex_lock(&r->mtx);
    const char *p = r->policy ? r->policy : "round_robin";
    mutex_unlock(&r->mtx);
    return p;
}

/* Does a route carry the capability tag (comma-separated exact match)? */
static int route_has_cap(const route *e, const char *tag) {
    size_t tlen;

    if (!e->caps)
        return 0;
    tlen = strlen(tag);
    const char *p = e->caps;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == tlen && strncmp(p, tag, len) == 0)
            return 1;
        if (!comma)
            break;
        p = comma + 1;
    }

    return 0;
}

/* Pick under lock. Metrics-only policies restrict to configured routes and
 * choose the best value; equal-value ties rotate (secondary round-robin). */
static route *pick_locked(router *r) {
    int is_cost;
    int is_lat;
    int is_cap;
    size_t cand[64];
    size_t n = 0;
    /* best metric value (lower = better; 0 = unknown, never preferred) */
    size_t best;
    int best_v;

    if (r->count == 0)
        return NULL;
    const char *pol = r->policy ? r->policy : "round_robin";
    is_cost = strcmp(pol, "cost") == 0;
    is_lat = strcmp(pol, "latency") == 0;
    is_cap = strncmp(pol, "capability:", 11) == 0;
    if (!is_cost && !is_lat && !is_cap) {
        /* weighted round-robin: advance by the current route's weight */
        route *e = &r->routes[r->cursor % r->count];
        size_t step = (e->weight >= 1.0) ? (size_t)e->weight : 1;
        r->cursor = (r->cursor + step) % r->count;
        return e;
    }

    if (is_cap) {
        const char *tag = pol + 11;
        for (size_t i = 0; i < r->count && n < 64; i++)
            if (route_has_cap(&r->routes[i], tag))
                cand[n++] = i;
        if (n == 0) { /* nobody carries the tag: degrade to full round-robin */
            for (size_t i = 0; i < r->count && n < 64; i++)
                cand[n++] = i;
            route *e = &r->routes[cand[r->cursor++ % n]];
            return e;
        }
    } else {
        for (size_t i = 0; i < r->count && n < 64; i++)
            cand[n++] = i;
    }

    /* best metric value (lower = better; 0 = unknown, never preferred) */
    best = cand[0];
    for (size_t i = 1; i < n; i++) {
        route *a = &r->routes[cand[i]], *b = &r->routes[best];
        int av = is_lat ? a->latency_ms : a->cost_rank;
        int bv = is_lat ? b->latency_ms : b->cost_rank;
        if (av > 0 && (bv <= 0 || av < bv))
            best = cand[i];
    }

    best_v = is_lat ? r->routes[best].latency_ms : r->routes[best].cost_rank;
    if (best_v > 0) {
        size_t ties[64];
        size_t nt = 0;
        for (size_t i = 0; i < n; i++) {
            route *e = &r->routes[cand[i]];
            int v = is_lat ? e->latency_ms : e->cost_rank;
            if (v == best_v)
                ties[nt++] = cand[i];
        }
        if (nt > 1) {
            r->tie_cursor = (r->tie_cursor + 1) % nt;
            return &r->routes[ties[r->tie_cursor]];
        }
    }

    return &r->routes[best];
}

const route *router_pick(router *r) {
    route *e;

    if (!r)
        return NULL;
    mutex_lock(&r->mtx);
    e = pick_locked(r);
    if (e)
        e->calls++;
    mutex_unlock(&r->mtx);
    return e;
}

int router_count(router *r) {
    int n;

    if (!r)
        return 0;
    mutex_lock(&r->mtx);
    n = (int)r->count;
    mutex_unlock(&r->mtx);
    return n;
}

const route *router_get(router *r, size_t i) {
    if (!r)
        return NULL;
    mutex_lock(&r->mtx);
    const route *e = (i < r->count) ? &r->routes[i] : NULL;
    mutex_unlock(&r->mtx);
    return e;
}

char *router_json(router *r) {
    cJSON *arr = cJSON_CreateArray();
    char *js;

    if (!r)
        return cJSON_PrintUnformatted(arr);
    mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        route *e = &r->routes[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "provider", e->provider);
        if (e->base_url)
            cJSON_AddStringToObject(o, "base_url", e->base_url);
        if (e->model)
            cJSON_AddStringToObject(o, "model", e->model);
        if (e->api_key)
            cJSON_AddStringToObject(o, "api_key", e->api_key);
        cJSON_AddNumberToObject(o, "weight", e->weight);
        cJSON_AddNumberToObject(o, "calls", (double)e->calls);
        cJSON_AddNumberToObject(o, "cost_rank", e->cost_rank);
        cJSON_AddNumberToObject(o, "latency_ms", e->latency_ms);
        if (e->caps)
            cJSON_AddStringToObject(o, "caps", e->caps);
        cJSON_AddItemToArray(arr, o);
    }

    mutex_unlock(&r->mtx);
    js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return js;
}

int router_save_file(router *r, const char *path) {
    char *js;
    FILE *f;
    size_t n;
    size_t w;

    if (!r || !path)
        return -1;
    js = router_json(r);
    if (!js)
        return -1;
    f = fopen(path, "wb");
    if (!f) {
        free(js);
        return -1;
    }

    n = strlen(js);
    w = fwrite(js, 1, n, f);
    fclose(f);
    free(js);
    return (w == n) ? 0 : -1;
}

int router_load_file(router *r, const char *path) {
    FILE *f;
    long sz;
    char *buf;
    size_t rd;
    cJSON *arr;
    cJSON *it;

    if (!r || !path)
        return -1;
    f = fopen(path, "rb");
    if (!f)
        return -1; /* no saved routes (first run) */
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return 0;
    }

    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }

    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr)
            cJSON_Delete(arr);
        return -1;
    }

    /* collect routes first, then add them *without* holding r->mtx (router_add
     * locks it internally — holding it here would deadlock on a non-recursive mutex) */
    cJSON_ArrayForEach(it, arr) {
    cJSON *name;
    cJSON *prov;
    cJSON *b;
    cJSON *m;
    cJSON *k;
    cJSON *w;
    cJSON *c;
    cJSON *l;
    cJSON *cp;
    double weight;
    int cost;
    int lat;

        if (!cJSON_IsObject(it))
            continue;
        name = cJSON_GetObjectItemCaseSensitive(it, "name");
        prov = cJSON_GetObjectItemCaseSensitive(it, "provider");
        if (!name || !cJSON_IsString(name) || !prov || !cJSON_IsString(prov))
            continue;
        b = cJSON_GetObjectItemCaseSensitive(it, "base_url");
        m = cJSON_GetObjectItemCaseSensitive(it, "model");
        k = cJSON_GetObjectItemCaseSensitive(it, "api_key");
        w = cJSON_GetObjectItemCaseSensitive(it, "weight");
        c = cJSON_GetObjectItemCaseSensitive(it, "cost_rank");
        l = cJSON_GetObjectItemCaseSensitive(it, "latency_ms");
        cp = cJSON_GetObjectItemCaseSensitive(it, "caps");
        weight = (w && cJSON_IsNumber(w)) ? w->valuedouble : 1.0;
        cost = (c && cJSON_IsNumber(c)) ? (int)c->valuedouble : 0;
        lat = (l && cJSON_IsNumber(l)) ? (int)l->valuedouble : 0;
        const char *caps = (cp && cJSON_IsString(cp)) ? cp->valuestring : NULL;
        router_add_ex(r, name->valuestring, prov->valuestring, (b && cJSON_IsString(b)) ? b->valuestring : NULL,
                          (k && cJSON_IsString(k)) ? k->valuestring : NULL,
                          (m && cJSON_IsString(m)) ? m->valuestring : NULL, weight, cost, lat, caps);
    }

    cJSON_Delete(arr);
    return 0;
}
