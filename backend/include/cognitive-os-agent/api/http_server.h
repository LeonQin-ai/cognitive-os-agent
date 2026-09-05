/* http_server.h — minimal single-threaded HTTP/1.1 server.
 * Route table of (method, path prefix) -> handler, one request per connection
 * (Connection: close). Backed by the os_socket listener/accept primitives.
 * Handlers run on the accepting thread, so the server is safe without locks. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cognitive-os-agent/infra/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_http_server coa_http_server;

typedef struct coa_http_request {
    char method[16];
    char path[1024];
    char query[512];
    char authorization[512];  /* value of the Authorization header, if any */
    const char *body;   /* NULL if no body */
    size_t body_len;
} coa_http_request;

typedef struct coa_http_response {
    int status;                 /* default 200 */
    char content_type[64];      /* default "application/json" */
    coa_strbuf body;             /* fill with coa_http_resp_* helpers */
} coa_http_response;

/* Handler signature. Fills resp; returns 0 ok, -1 -> 500. */
typedef int (*coa_http_handler)(const coa_http_request *req, coa_http_response *resp, void *ud);

/* Inbound WebSocket text message callback (NUL-terminated, borrowed). */
typedef void (*coa_ws_handler)(const char *text, void *ud);

coa_http_server *coa_http_server_new(uint16_t port);
coa_http_server *coa_http_server_new_bind(const char *host, uint16_t port);
void coa_http_server_free(coa_http_server *s);

/* Register a route. First matching (method, prefix) wins; method "*" matches all. */
void coa_http_server_route(coa_http_server *s, const char *method, const char *path_prefix,
                          coa_http_handler fn, void *ud);

/* Register a WebSocket upgrade path (e.g. "/ws"). Inbound text messages are
 * forwarded to on_msg. Broadcast pushes events to every connected client. */
void coa_http_server_ws_route(coa_http_server *s, const char *path,
                             coa_ws_handler on_msg, void *ud);
void coa_http_server_ws_broadcast(coa_http_server *s, const char *json_text);

/* Accept and serve until coa_http_server_stop. Returns 0 on clean stop, -1 on error. */
int coa_http_server_serve(coa_http_server *s);
void coa_http_server_stop(coa_http_server *s);

/* Response helpers. */
void coa_http_resp_append(coa_http_response *resp, const char *s);
void coa_http_resp_appendf(coa_http_response *resp, const char *fmt, ...);
void coa_http_resp_json(coa_http_response *resp, const char *json);

#ifdef __cplusplus
}
#endif
