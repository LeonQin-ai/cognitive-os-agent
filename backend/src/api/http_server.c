/* http_server.c — minimal HTTP/1.1 server implementation. */
#include "cognitive-os-agent/api/http_server.h"
#include "cognitive-os-agent/api/ws_server.h"
#include "cognitive-os-agent/os/os_socket.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define MAX_HEADER_BYTES (16 * 1024)
#define MAX_BODY_BYTES   (32 * 1024 * 1024)

typedef struct route {
    char method[16];
    char prefix[256];
    coa_http_handler fn;
    void *ud;
} route;

struct coa_http_server {
    uint16_t port;
    coa_listener *listener;
    route *routes;
    size_t n_routes, cap_routes;
    coa_ws_server *ws;            /* WebSocket hub (created on first ws route) */
    char ws_path[256];
    coa_ws_handler ws_on_msg;
    void *ws_ud;
    volatile int stop_flag;
    coa_mutex mtx;
};

coa_http_server *coa_http_server_new(uint16_t port) {
    return coa_http_server_new_bind(NULL, port);
}

coa_http_server *coa_http_server_new_bind(const char *host, uint16_t port) {
    coa_http_server *s = calloc(1, sizeof(coa_http_server));
    if (!s) return NULL;
    s->port = port;
    s->listener = coa_listen_addr(host, port);
    if (!s->listener) {
        coa_log_error("http server: failed to listen on %s:%u: %s",
                     (host && *host) ? host : "*", (unsigned)port, coa_sock_error());
        free(s);
        return NULL;
    }
    coa_mutex_init(&s->mtx);
    coa_log_info("http server listening on %s:%u", (host && *host) ? host : "*", (unsigned)port);
    return s;
}

void coa_http_server_free(coa_http_server *s) {
    if (!s) return;
    if (s->listener) coa_listener_close(s->listener);
    if (s->ws) coa_ws_server_free(s->ws);
    free(s->routes);
    coa_mutex_destroy(&s->mtx);
    free(s);
}

void coa_http_server_route(coa_http_server *s, const char *method, const char *path_prefix,
                          coa_http_handler fn, void *ud) {
    if (!s || !fn) return;
    if (s->n_routes == s->cap_routes) {
        size_t cap = s->cap_routes ? s->cap_routes * 2 : 8;
        route *nr = realloc(s->routes, cap * sizeof(route));
        if (!nr) return;
        s->routes = nr;
        s->cap_routes = cap;
    }
    route *r = &s->routes[s->n_routes++];
    snprintf(r->method, sizeof(r->method), "%s", method ? method : "*");
    snprintf(r->prefix, sizeof(r->prefix), "%s", path_prefix ? path_prefix : "/");
    r->fn = fn;
    r->ud = ud;
}

void coa_http_server_ws_route(coa_http_server *s, const char *path,
                             coa_ws_handler on_msg, void *ud) {
    if (!s || !path) return;
    snprintf(s->ws_path, sizeof(s->ws_path), "%s", path);
    s->ws_on_msg = on_msg;
    s->ws_ud = ud;
    if (!s->ws) s->ws = coa_ws_server_new();
    if (s->ws) coa_ws_server_on_message(s->ws, on_msg, ud);
}

void coa_http_server_ws_broadcast(coa_http_server *s, const char *json_text) {
    if (s && s->ws) coa_ws_server_broadcast(s->ws, json_text);
}

void coa_http_server_stop(coa_http_server *s) {
    if (!s) return;
    coa_mutex_lock(&s->mtx);
    s->stop_flag = 1;
    coa_mutex_unlock(&s->mtx);
}

static int startswith(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}

/* Portable ASCII case-insensitive prefix match (avoids strncasecmp/_strnicmp). */
static int startswith_icase(const char *s, const char *p) {
    while (*p) {
        char a = *s++, b = *p++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static const char *status_reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

/* Read request head (request line + headers) and body. Returns 0 ok, -1 error. */
static int read_request(coa_socket *sock, char *buf, size_t cap, size_t *head_len, size_t *body_len) {
    size_t got = 0;
    size_t hlen = 0;
    for (;;) {
        /* look for \r\n\r\n terminator */
        size_t i;
        for (i = 0; i + 3 < got; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                hlen = i + 4;
                break;
            }
        }
        if (hlen) break;
        if (got >= cap - 1 || got >= MAX_HEADER_BYTES) return -1;
        int n = coa_sock_recv(sock, buf + got, cap - 1 - got);
        if (n <= 0) return -1;
        got += (size_t)n;
        buf[got] = '\0';
    }

    /* parse Content-Length */
    size_t clen = 0;
    const char *cl = strstr(buf, "Content-Length:");
    if (cl) {
        cl += 15;
        while (*cl == ' ' || *cl == '\t') cl++;
        clen = (size_t)strtoul(cl, NULL, 10);
    }
    if (clen > MAX_BODY_BYTES) return -1;

    /* read remaining body bytes */
    while (got < hlen + clen) {
        if (got >= cap - 1) return -1;
        int n = coa_sock_recv(sock, buf + got, cap - 1 - got);
        if (n <= 0) return -1;
        got += (size_t)n;
        buf[got] = '\0';
    }
    *head_len = hlen;
    *body_len = clen;
    return 0;
}

/* Handle one accepted connection (buf supplied by the heap-allocating
 * wrapper; MAX_BODY_BYTES is 32MB — far too large for the stack). */
static int handle_conn_buf(coa_http_server *s, coa_socket *sock, char *buf) {
    size_t hlen = 0, blen = 0;
    if (read_request(sock, buf, MAX_HEADER_BYTES + MAX_BODY_BYTES, &hlen, &blen) != 0) return 0;

    /* parse request line */
    char method[16], path[1024], query[512];
    method[0] = path[0] = query[0] = '\0';
    char line[1024];
    size_t linelen = 0;
    while (linelen < hlen && linelen + 1 < sizeof(line) &&
           !(buf[linelen] == '\r' && buf[linelen + 1] == '\n')) {
        line[linelen] = buf[linelen];
        linelen++;
    }
    line[linelen] = '\0';
    if (sscanf(line, "%15s %1023s", method, path) != 2) return 0;
    char *qm = strchr(path, '?');
    if (qm) {
        snprintf(query, sizeof(query), "%s", qm + 1);
        *qm = '\0';
    }

    coa_http_request req;
    memset(&req, 0, sizeof(req));
    snprintf(req.method, sizeof(req.method), "%s", method);
    snprintf(req.path, sizeof(req.path), "%s", path);
    snprintf(req.query, sizeof(req.query), "%s", query);

    /* WebSocket upgrade? */
    if (s->ws && s->ws_path[0] && startswith(path, s->ws_path)) {
        char ws_key[160] = "";
        for (size_t k = 0; k + 24 < hlen; k++) {
            if (buf[k] == '\r' && buf[k + 1] == '\n' &&
                startswith_icase(buf + k + 2, "Sec-WebSocket-Key:")) {
                const char *v = buf + k + 2 + 18;
                while (*v == ' ' || *v == '\t') v++;
                size_t vlen = 0;
                while (v[vlen] && v[vlen] != '\r' && v[vlen] != '\n' &&
                       vlen + 1 < sizeof(ws_key)) vlen++;
                memcpy(ws_key, v, vlen);
                ws_key[vlen] = '\0';
                break;
            }
        }
        if (ws_key[0]) {
            coa_ws_server_accept(s->ws, sock, ws_key);
            return 1; /* socket owned by the ws client thread */
        }
    }

    /* capture Authorization header (case-insensitive) if present */
    for (size_t k = 0; k + 15 < hlen; k++) {
        if (buf[k] == '\n' &&
            startswith_icase(buf + k + 1, "Authorization:")) {
            const char *v = buf + k + 1 + 14;
            while (*v == ' ' || *v == '\t') v++;
            size_t vlen = 0;
            while (v[vlen] && v[vlen] != '\r' && v[vlen] != '\n' &&
                   vlen + 1 < sizeof(req.authorization)) vlen++;
            snprintf(req.authorization, sizeof(req.authorization), "%.*s",
                     (int)vlen, v);
            break;
        }
    }
    req.body = blen ? buf + hlen : NULL;
    req.body_len = blen;

    coa_http_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = 0;   /* unknown: filled by the dispatcher unless the handler set it */
    snprintf(resp.content_type, sizeof(resp.content_type), "application/json");
    coa_strbuf_init(&resp.body);

    /* dispatch: among routes whose path prefix matches, prefer one whose
     * method matches the request, then the longest prefix (so "/v1/tasks/123"
     * matches "GET /v1/tasks/" and "POST /v1/routes" is not shadowed by
     * "GET /v1/routes"). The bare "/" catch-all serves the web UI for non-API
     * paths only; an unknown /v1/... endpoint is a genuine 404, not an SPA
     * route. */
    route *best = NULL;
    for (size_t i = 0; i < s->n_routes; i++) {
        route *r = &s->routes[i];
        if (r->prefix[0] == '/' && r->prefix[1] == '\0') continue; /* catch-all: last resort */
        if (!startswith(req.path, r->prefix)) continue;
        int meth = (r->method[0] == '*' || strcmp(r->method, req.method) == 0);
        if (!best) { best = r; continue; }
        int bmeth = (best->method[0] == '*' || strcmp(best->method, req.method) == 0);
        if (meth && !bmeth) best = r;
        else if (meth == bmeth && strlen(r->prefix) > strlen(best->prefix)) best = r;
    }
    int status = 200;
    if (!best && strncmp(req.path, "/v1", 3) != 0) {
        /* no specific API route matched: fall back to the "/" catch-all for
         * non-API paths (web UI); an unknown /v1/... endpoint is a 404 */
        for (size_t i = 0; i < s->n_routes && !best; i++) {
            route *r = &s->routes[i];
            if (r->prefix[0] == '/' && r->prefix[1] == '\0') best = r;
        }
    }
    if (!best) {
        status = 404;
        coa_http_resp_json(&resp, "{\"error\":\"not found\"}");
    } else if (best->method[0] != '*' && strcmp(best->method, req.method) != 0) {
        status = 405;
        coa_http_resp_json(&resp, "{\"error\":\"method not allowed\"}");
    } else if (best->fn(&req, &resp, best->ud) != 0) {
        status = 500;
    }
    /* Respect a status the handler set itself (e.g. 400 on bad input);
     * otherwise fall back to the dispatcher's status. */
    if (resp.status == 0) resp.status = status;

    /* serialize response */
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                     resp.status, status_reason(resp.status), resp.content_type, resp.body.len);
    if (n > 0) coa_sock_send(sock, head, (size_t)n);
    if (resp.body.len > 0) coa_sock_send(sock, resp.body.buf, resp.body.len);
    coa_strbuf_free(&resp.body);
    return 0;
}

/* Heap-allocating wrapper: the request buffer is up to MAX_BODY_BYTES (32MB),
 * which must not live on the stack. Returns 1 if the socket was handed off to
 * a WebSocket client thread (caller must not close it), 0 otherwise. */
static int handle_conn(coa_http_server *s, coa_socket *sock) {
    char *buf = malloc(MAX_HEADER_BYTES + MAX_BODY_BYTES);
    if (!buf) return 0;
    int rc = handle_conn_buf(s, sock, buf);
    free(buf);
    return rc;
}

int coa_http_server_serve(coa_http_server *s) {
    if (!s || !s->listener) return -1;
    while (!s->stop_flag) {
        coa_socket *c = coa_accept(s->listener, 200);
        if (!c) {
            if (s->stop_flag) break;
            continue;
        }
        if (handle_conn(s, c) == 0)
            coa_sock_close(c);
    }
    return 0;
}

void coa_http_resp_append(coa_http_response *resp, const char *s) {
    if (!resp || !s) return;
    coa_strbuf_append(&resp->body, s);
}

void coa_http_resp_appendf(coa_http_response *resp, const char *fmt, ...) {
    if (!resp) return;
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    coa_strbuf_append(&resp->body, tmp);
}

void coa_http_resp_json(coa_http_response *resp, const char *json) {
    if (!resp || !json) return;
    snprintf(resp->content_type, sizeof(resp->content_type), "application/json");
    coa_strbuf_append(&resp->body, json);
}
