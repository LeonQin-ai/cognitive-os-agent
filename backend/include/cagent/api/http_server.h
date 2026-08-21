/* http_server.h — minimal single-threaded HTTP/1.1 server.
 * Route table of (method, path prefix) -> handler, one request per connection
 * (Connection: close). Backed by the os_socket listener/accept primitives.
 * Handlers run on the accepting thread, so the server is safe without locks. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cagent/infra/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_http_server ca_http_server;

typedef struct ca_http_request {
    char method[16];
    char path[1024];
    char query[512];
    char authorization[512];  /* value of the Authorization header, if any */
    const char *body;   /* NULL if no body */
    size_t body_len;
} ca_http_request;

typedef struct ca_http_response {
    int status;                 /* default 200 */
    char content_type[64];      /* default "application/json" */
    ca_strbuf body;             /* fill with ca_http_resp_* helpers */
} ca_http_response;

/* Handler signature. Fills resp; returns 0 ok, -1 -> 500. */
typedef int (*ca_http_handler)(const ca_http_request *req, ca_http_response *resp, void *ud);

ca_http_server *ca_http_server_new(uint16_t port);
ca_http_server *ca_http_server_new_bind(const char *host, uint16_t port);
void ca_http_server_free(ca_http_server *s);

/* Register a route. First matching (method, prefix) wins; method "*" matches all. */
void ca_http_server_route(ca_http_server *s, const char *method, const char *path_prefix,
                          ca_http_handler fn, void *ud);

/* Accept and serve until ca_http_server_stop. Returns 0 on clean stop, -1 on error. */
int ca_http_server_serve(ca_http_server *s);
void ca_http_server_stop(ca_http_server *s);

/* Response helpers. */
void ca_http_resp_append(ca_http_response *resp, const char *s);
void ca_http_resp_appendf(ca_http_response *resp, const char *fmt, ...);
void ca_http_resp_json(ca_http_response *resp, const char *json);

#ifdef __cplusplus
}
#endif
