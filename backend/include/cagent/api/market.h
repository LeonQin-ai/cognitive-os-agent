/* market.h — networked marketplace client (merge remote catalogs, push publish).
 * Kept separate from http_server.h because both http_server.h and os/http.h
 * define a `ca_http_response` type; this module owns the os/http.h dependency
 * so api_rest.c only needs this thin header. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* GET a JSON payload from a marketplace server.
 * Returns a malloc'd string (caller frees) or NULL on any failure
 * (unreachable, non-200, empty body). */
char *ca_market_fetch(const char *base_url, const char *path, int timeout_ms);

/* POST a JSON payload to a marketplace server (best-effort publish).
 * Returns 0 on 2xx, -1 otherwise. */
int ca_market_publish(const char *base_url, const char *path,
                      const char *json_body, int timeout_ms);

#ifdef __cplusplus
}
#endif
