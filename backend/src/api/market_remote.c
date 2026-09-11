/* market_remote.c — networked marketplace client over the platform HTTP stack.
 * Windows: os/http_winhttp.c (WinHTTP). Linux: os/http.c (raw sockets). */
#include "cognitive-os-agent/api/market.h"
#include "cognitive-os-agent/os/http.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>

char *market_fetch(const char *base_url, const char *path, int timeout_ms) {
    if (!base_url || !*base_url || !path)
        return NULL;
    if (timeout_ms <= 0)
        timeout_ms = 4000;
    http_response *r = http_get(base_url, path, NULL, timeout_ms);
    if (!r)
        return NULL;
    if (r->status < 200 || r->status >= 300) {
        http_response_free(r);
        return NULL;
    }
    char *out = r->body && r->body_len ? xstrdup(r->body) : NULL;
    http_response_free(r);
    return out;
}

int market_publish(const char *base_url, const char *path, const char *json_body, int timeout_ms) {
    if (!base_url || !*base_url || !path)
        return -1;
    if (timeout_ms <= 0)
        timeout_ms = 4000;
    http_response *r =
        http_post(base_url, path, json_body ? json_body : "{}", "application/json", NULL, timeout_ms);
    if (!r)
        return -1;
    int ok = (r->status >= 200 && r->status < 300);
    http_response_free(r);
    return ok ? 0 : -1;
}
