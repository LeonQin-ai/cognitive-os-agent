/* router.c — weighted round-robin provider routing. */
#include "cagent/llm/router.h"
#include "cagent/os/os_thread.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct ca_router {
    ca_mutex mtx;
    ca_route *routes;
    size_t count, cap;
    size_t cursor;
    double total_weight;
};

ca_router *ca_router_new(void) {
    ca_router *r = (ca_router *)calloc(1, sizeof(ca_router));
    if (!r) return NULL;
    ca_mutex_init(&r->mtx);
    return r;
}

void ca_router_free(ca_router *r) {
    if (!r) return;
    ca_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        ca_route *e = &r->routes[i];
        free(e->name); free(e->provider); free(e->base_url);
        free(e->api_key); free(e->model);
    }
    free(r->routes);
    ca_mutex_unlock(&r->mtx);
    ca_mutex_destroy(&r->mtx);
    free(r);
}

int ca_router_add(ca_router *r, const char *name, const char *provider,
                  const char *base_url, const char *api_key,
                  const char *model, double weight) {
    if (!r || !name || !provider) return -1;
    if (weight <= 0) weight = 1.0;
    ca_mutex_lock(&r->mtx);
    /* upsert: replace an existing route with the same name */
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->routes[i].name, name) == 0) {
            ca_route *e = &r->routes[i];
            r->total_weight -= e->weight;
            free(e->provider); free(e->base_url); free(e->api_key); free(e->model);
            e->provider = ca_strdup(provider);
            e->base_url = base_url ? ca_strdup(base_url) : NULL;
            e->api_key = api_key ? ca_strdup(api_key) : NULL;
            e->model = model ? ca_strdup(model) : NULL;
            e->weight = weight;
            r->total_weight += weight;
            ca_mutex_unlock(&r->mtx);
            return 0;
        }
    }
    if (r->count == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 8;
        ca_route *nr = (ca_route *)realloc(r->routes, ncap * sizeof(ca_route));
        if (!nr) { ca_mutex_unlock(&r->mtx); return -1; }
        r->routes = nr;
        r->cap = ncap;
    }
    ca_route *e = &r->routes[r->count++];
    memset(e, 0, sizeof(*e));
    e->name = ca_strdup(name);
    e->provider = ca_strdup(provider);
    e->base_url = base_url ? ca_strdup(base_url) : NULL;
    e->api_key = api_key ? ca_strdup(api_key) : NULL;
    e->model = model ? ca_strdup(model) : NULL;
    e->weight = weight;
    r->total_weight += weight;
    ca_mutex_unlock(&r->mtx);
    return 0;
}

int ca_router_remove(ca_router *r, const char *name) {
    if (!r || !name) return 0;
    ca_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->routes[i].name, name) == 0) {
            ca_route *e = &r->routes[i];
            r->total_weight -= e->weight;
            free(e->name); free(e->provider); free(e->base_url);
            free(e->api_key); free(e->model);
            /* shift remaining routes down */
            memmove(&r->routes[i], &r->routes[i + 1],
                    (r->count - i - 1) * sizeof(ca_route));
            r->count--;
            ca_mutex_unlock(&r->mtx);
            return 1;
        }
    }
    ca_mutex_unlock(&r->mtx);
    return 0;
}

const ca_route *ca_router_pick(ca_router *r) {
    if (!r) return NULL;
    ca_mutex_lock(&r->mtx);
    if (r->count == 0) { ca_mutex_unlock(&r->mtx); return NULL; }
    /* weighted round-robin: advance cursor by weight to spread load */
    ca_route *e = &r->routes[r->cursor];
    e->calls++;
    r->cursor = (r->cursor + 1) % r->count;
    ca_mutex_unlock(&r->mtx);
    return e;
}

int ca_router_count(ca_router *r) {
    if (!r) return 0;
    ca_mutex_lock(&r->mtx);
    int n = (int)r->count;
    ca_mutex_unlock(&r->mtx);
    return n;
}

const ca_route *ca_router_get(ca_router *r, size_t i) {
    if (!r) return NULL;
    ca_mutex_lock(&r->mtx);
    const ca_route *e = (i < r->count) ? &r->routes[i] : NULL;
    ca_mutex_unlock(&r->mtx);
    return e;
}

char *ca_router_json(ca_router *r) {
    cJSON *arr = cJSON_CreateArray();
    if (!r) return cJSON_PrintUnformatted(arr);
    ca_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        ca_route *e = &r->routes[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "provider", e->provider);
        if (e->base_url) cJSON_AddStringToObject(o, "base_url", e->base_url);
        if (e->model) cJSON_AddStringToObject(o, "model", e->model);
        if (e->api_key) cJSON_AddStringToObject(o, "api_key", e->api_key);
        cJSON_AddNumberToObject(o, "weight", e->weight);
        cJSON_AddNumberToObject(o, "calls", (double)e->calls);
        cJSON_AddItemToArray(arr, o);
    }
    ca_mutex_unlock(&r->mtx);
    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return js;
}

int ca_router_save_file(ca_router *r, const char *path) {
    if (!r || !path) return -1;
    char *js = ca_router_json(r);
    if (!js) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { free(js); return -1; }
    size_t n = strlen(js);
    size_t w = fwrite(js, 1, n, f);
    fclose(f);
    free(js);
    return (w == n) ? 0 : -1;
}

int ca_router_load_file(ca_router *r, const char *path) {
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
    /* collect routes first, then add them *without* holding r->mtx (ca_router_add
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
        const char *base = (b && cJSON_IsString(b)) ? b->valuestring : NULL;
        const char *model = (m && cJSON_IsString(m)) ? m->valuestring : NULL;
        const char *key = (k && cJSON_IsString(k)) ? k->valuestring : NULL;
        double weight = 1.0;
        cJSON *w = cJSON_GetObjectItemCaseSensitive(it, "weight");
        if (w && cJSON_IsNumber(w)) weight = w->valuedouble;
        ca_router_add(r, name->valuestring, prov->valuestring, base, key, model, weight);
    }
    cJSON_Delete(arr);
    return 0;
}
