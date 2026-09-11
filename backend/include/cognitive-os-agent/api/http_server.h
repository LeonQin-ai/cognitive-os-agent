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

typedef struct http_server http_server;

typedef struct http_request {
    char method[16];
    char path[1024];
    char query[512];
    char authorization[512]; /* value of the Authorization header, if any */
    const char *body;        /* NULL if no body */
    size_t body_len;
} http_request;

typedef struct http_response {
    int status;            /* default 200 */
    char content_type[64]; /* default "application/json" */
    strbuf body;       /* fill with http_resp_* helpers */
} http_response;

/* Handler signature. Fills resp; returns 0 ok, -1 -> 500. */
typedef int (*http_handler)(const http_request *req, http_response *resp, void *ud);

/* Inbound WebSocket text message callback (NUL-terminated, borrowed). */
typedef void (*ws_handler)(const char *text, void *ud);

http_server *http_server_new_bind(const char *host, uint16_t port);
void http_server_free(http_server *s);

/* Register a route. First matching (method, prefix) wins; method "*" matches all. */
void http_server_route(http_server *s, const char *method, const char *path_prefix, http_handler fn,
                           void *ud);

/* Register a WebSocket upgrade path (e.g. "/ws"). Inbound text messages are
 * forwarded to on_msg. Broadcast pushes events to every connected client. */
void http_server_ws_route(http_server *s, const char *path, ws_handler on_msg, void *ud);
void http_server_ws_broadcast(http_server *s, const char *json_text);

/* Accept and serve until http_server_stop. Returns 0 on clean stop, -1 on error. */
int http_server_serve(http_server *s);
void http_server_stop(http_server *s);

/* Response helpers. */
void http_resp_append(http_response *resp, const char *s);
void http_resp_appendf(http_response *resp, const char *fmt, ...);
void http_resp_json(http_response *resp, const char *json);

#ifdef __cplusplus
}
#endif
