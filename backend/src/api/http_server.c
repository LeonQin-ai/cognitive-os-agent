/* http_server.c — minimal HTTP/1.1 server implementation. */
#include "api/http_server.h"
#include "api/ws_server.h"
#include "os/os_socket.h"
#include "os/os_thread.h"
#include "infra/logging.h"
#include "os/os_time.h"
#include <stdatomic.h>
#include <ctype.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define MAX_HEADER_BYTES (16 * 1024)
#define MAX_BODY_BYTES (128 * 1024 * 1024)

typedef struct route {
    char method[16];
    char prefix[256];
    http_handler fn;
    void *ud;
    int concurrent;
} route;

struct http_server {
    uint16_t port;
    listener *listener;
    route *routes;
    size_t n_routes, cap_routes;
    ws_server *ws; /* WebSocket hub (created on first ws route) */
    char ws_path[256];
    ws_handler ws_on_msg;
    void *ws_ud;
    _Atomic int stop_flag;
    mutex_t mtx;
    mutex_t dispatch_mtx;
    cond drained;
    int active;
    sock *connections[16];
};

http_server *http_server_new_bind(const char *host, uint16_t port) {
    http_server *s = calloc(1, sizeof(http_server));
    if (!s)
        return NULL;
    s->port = port;
    s->listener = listen_addr(host, port);
    if (!s->listener) {
        log_error("http server: failed to listen on %s:%u: %s", (host && *host) ? host : "*", (unsigned)port,
                      sock_error());
        free(s);
        return NULL;
    }

    mutex_init(&s->mtx);
    mutex_init(&s->dispatch_mtx);
    cond_init(&s->drained);
    log_info("http server listening on %s:%u", (host && *host) ? host : "*", (unsigned)port);
    return s;
}

void http_server_free(http_server *s) {
    if (!s)
        return;
    http_server_stop(s);
    mutex_lock(&s->mtx);
    while (s->active) cond_wait(&s->drained, &s->mtx);
    mutex_unlock(&s->mtx);
    if (s->listener)
        listener_close(s->listener);
    if (s->ws)
        ws_server_free(s->ws);
    free(s->routes);
    mutex_destroy(&s->mtx);
    mutex_destroy(&s->dispatch_mtx);
    cond_destroy(&s->drained);
    free(s);
}

void http_server_route(http_server *s, const char *method, const char *path_prefix, http_handler fn,
                           void *ud) {
    route *r;

    if (!s || !fn)
        return;
    if (s->n_routes == s->cap_routes) {
        size_t cap = s->cap_routes ? s->cap_routes * 2 : 8;
        route *nr = realloc(s->routes, cap * sizeof(route));
        if (!nr)
            return;
        s->routes = nr;
        s->cap_routes = cap;
    }

    r = &s->routes[s->n_routes++];
    snprintf(r->method, sizeof(r->method), "%s", method ? method : "*");
    snprintf(r->prefix, sizeof(r->prefix), "%s", path_prefix ? path_prefix : "/");
    r->fn = fn;
    r->ud = ud;
    r->concurrent = 0;
}

void http_server_route_concurrent(http_server *s, const char *method, const char *path_prefix,
                                  http_handler fn, void *ud) {
    if(!s || !fn) return;
    size_t previous=s->n_routes;
    http_server_route(s,method,path_prefix,fn,ud);
    if(s->n_routes>previous) s->routes[s->n_routes-1].concurrent=1;
}

void http_server_ws_route(http_server *s, const char *path, ws_handler on_msg, void *ud) {
    if (!s || !path)
        return;
    snprintf(s->ws_path, sizeof(s->ws_path), "%s", path);
    s->ws_on_msg = on_msg;
    s->ws_ud = ud;
    if (!s->ws)
        s->ws = ws_server_new();
    if (s->ws)
        ws_server_on_message(s->ws, on_msg, ud);
}

void http_server_ws_broadcast(http_server *s, const char *json_text) {
    if (s && s->ws)
        ws_server_broadcast(s->ws, json_text);
}

void http_server_stop(http_server *s) {
    if (!s)
        return;
    mutex_lock(&s->mtx);
    s->stop_flag = 1;
    for (int i = 0; i < 16; i++)
        if (s->connections[i]) sock_shutdown(s->connections[i]);
    mutex_unlock(&s->mtx);
}

static int startswith(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}

/* Portable ASCII case-insensitive prefix match (avoids strncasecmp/_strnicmp). */
static int startswith_icase(const char *s, const char *p) {
    while (*p) {
        char a = *s++, b = *p++;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }

    return 1;
}

static const char *status_reason(int code) {
    switch (code) {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    default:
        return "OK";
    }
}

/* Read request head (request line + headers) and body. Returns 0 ok, -1 error. */
static int request_recv(http_server *s, sock *c, char *buf, size_t cap, int64_t deadline) {
    while (!s->stop_flag) {
        int64_t left = deadline - time_now_ms();
        if (left <= 0) return -1;
        int ready = sock_wait_readable(c, left > 100 ? 100 : (int)left);
        if (ready < 0) return -1;
        if (ready) return sock_recv(c, buf, cap);
    }
    return -1;
}

static int read_request(http_server *s, sock *c, char **out, size_t *head_len, size_t *body_len) {
    size_t got = 0, hlen = 0, clen = 0;
    int seen = 0;
    char *buf = malloc(MAX_HEADER_BYTES + 1);
    *out = buf;
    if (!buf) return 500;
    int64_t deadline = time_now_ms() + 30000;
    while (!hlen) {
        if (got == MAX_HEADER_BYTES) return 431;
        int n = request_recv(s, c, buf + got, MAX_HEADER_BYTES - got, deadline);
        if (n <= 0) return 408;
        got += (size_t)n; buf[got] = 0;
        char *end = strstr(buf, "\r\n\r\n");
        if (end) hlen = (size_t)(end - buf) + 4;
        if (memchr(buf, 0, hlen ? hlen : got)) return 400;
    }
    char *line = strstr(buf, "\r\n");
    if (!line) return 400;
    line += 2;
    while (line < buf + hlen - 2) {
        char *end = strstr(line, "\r\n");
        if (!end || end >= buf + hlen) return 400;
        char *colon = memchr(line, ':', (size_t)(end - line));
        if (!colon || colon == line) return 400;
        for (char *q = line; q < colon; q++)
            if (!isalnum((unsigned char)*q) && !strchr("!#$%&'*+-.^_`|~", *q)) return 400;
        if ((size_t)(colon-line) == 14 && startswith_icase(line, "Content-Length")) {
            if (seen++) return 400;
            char *q = colon + 1;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q == end || *q < '0' || *q > '9') return 400;
            while (q < end && *q >= '0' && *q <= '9') {
                if (clen > (MAX_BODY_BYTES - (size_t)(*q - '0')) / 10) return 413;
                clen = clen * 10 + (size_t)(*q++ - '0');
            }
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q != end) return 400;
        } else if ((size_t)(colon-line) == 17 && startswith_icase(line, "Transfer-Encoding")) return 400;
        line = end + 2;
    }
    char *grown = realloc(buf, hlen + clen + 1);
    if (!grown) return 500;
    buf = *out = grown;
    if (got > hlen + clen) got = hlen + clen;
    while (got < hlen + clen) {
        int n = request_recv(s, c, buf + got, hlen + clen - got, deadline);
        if (n <= 0) return 408;
        got += (size_t)n;
    }
    buf[hlen + clen] = 0;
    *head_len = hlen; *body_len = clen;
    return 0;
}

/* Dispatch one completely read request. The buffer is sized to its body. */
static int handle_conn_buf(http_server *s, sock *sock, char *buf, size_t hlen, size_t blen) {
    char line[1024];
    size_t linelen = 0;
    char *qm;
    http_request req;
    http_response resp;
    route *best = NULL;
    int status = 200;
    /* serialize response */
    char head[512];

    /* parse request line */
    char method[16], path[1024], query[512];
    method[0] = path[0] = query[0] = '\0';
    while (linelen < hlen && linelen + 1 < sizeof(line) && !(buf[linelen] == '\r' && buf[linelen + 1] == '\n')) {
        line[linelen] = buf[linelen];
        linelen++;
    }

    line[linelen] = '\0';
    if (sscanf(line, "%15s %1023s", method, path) != 2)
        return 0;
    qm = strchr(path, '?');
    if (qm) {
        snprintf(query, sizeof(query), "%s", qm + 1);
        *qm = '\0';
    }

    memset(&req, 0, sizeof(req));
    snprintf(req.method, sizeof(req.method), "%s", method);
    snprintf(req.path, sizeof(req.path), "%s", path);
    snprintf(req.query, sizeof(req.query), "%s", query);

    /* WebSocket upgrade? */
    if (s->ws && s->ws_path[0] && startswith(path, s->ws_path)) {
        char ws_key[160] = "";
        for (size_t k = 0; k + 24 < hlen; k++) {
            if (buf[k] == '\r' && buf[k + 1] == '\n' && startswith_icase(buf + k + 2, "Sec-WebSocket-Key:")) {
                const char *v = buf + k + 2 + 18;
                while (*v == ' ' || *v == '\t')
                    v++;
                size_t vlen = 0;
                while (v[vlen] && v[vlen] != '\r' && v[vlen] != '\n' && vlen + 1 < sizeof(ws_key))
                    vlen++;
                memcpy(ws_key, v, vlen);
                ws_key[vlen] = '\0';
                break;
            }
        }
        if (ws_key[0]) {
            /* The WS reader may close the socket immediately. Remove it from
             * the HTTP stop list before transferring ownership. */
            mutex_lock(&s->mtx);
            if (s->stop_flag) { mutex_unlock(&s->mtx); return 0; }
            for (int i=0;i<16;i++) if (s->connections[i]==sock) s->connections[i]=NULL;
            ws_server_accept(s->ws, sock, ws_key);
            mutex_unlock(&s->mtx);
            return 1; /* socket owned by the ws client thread */
        }
    }

    /* capture Authorization header (case-insensitive) if present */
    for (size_t k = 0; k + 15 < hlen; k++) {
        if (buf[k] == '\n' && startswith_icase(buf + k + 1, "Authorization:")) {
            const char *v = buf + k + 1 + 14;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t vlen = 0;
            while (v[vlen] && v[vlen] != '\r' && v[vlen] != '\n' && vlen + 1 < sizeof(req.authorization))
                vlen++;
            snprintf(req.authorization, sizeof(req.authorization), "%.*s", (int)vlen, v);
            break;
        }
    }

    req.body = blen ? buf + hlen : NULL;
    req.body_len = blen;

    memset(&resp, 0, sizeof(resp));
    resp.status = 0; /* unknown: filled by the dispatcher unless the handler set it */
    snprintf(resp.content_type, sizeof(resp.content_type), "application/json");
    strbuf_init(&resp.body);

    /* dispatch: among routes whose path prefix matches, prefer one whose
     * method matches the request, then the longest prefix (so "/v1/tasks/123"
     * matches "GET /v1/tasks/" and "POST /v1/routes" is not shadowed by
     * "GET /v1/routes"). The bare "/" catch-all serves the web UI for non-API
     * paths only; an unknown /v1/... endpoint is a genuine 404, not an SPA
     * route. */
    for (size_t i = 0; i < s->n_routes; i++) {
        route *r = &s->routes[i];
        if (r->prefix[0] == '/' && r->prefix[1] == '\0')
            continue; /* catch-all: last resort */
        if (!startswith(req.path, r->prefix))
            continue;
        int meth = (r->method[0] == '*' || strcmp(r->method, req.method) == 0);
        if (!best) {
            best = r;
            continue;
        }
        int bmeth = (best->method[0] == '*' || strcmp(best->method, req.method) == 0);
        if (meth && !bmeth)
            best = r;
        else if (meth == bmeth && strlen(r->prefix) > strlen(best->prefix))
            best = r;
    }

    if (!best && strncmp(req.path, "/v1", 3) != 0) {
        /* no specific API route matched: fall back to the "/" catch-all for
         * non-API paths (web UI); an unknown /v1/... endpoint is a 404 */
        for (size_t i = 0; i < s->n_routes && !best; i++) {
            route *r = &s->routes[i];
            if (r->prefix[0] == '/' && r->prefix[1] == '\0')
                best = r;
        }
    }

    if (!best) {
        status = 404;
        http_resp_json(&resp, "{\"error\":\"not found\"}");
    } else if (best->method[0] != '*' && strcmp(best->method, req.method) != 0) {
        status = 405;
        http_resp_json(&resp, "{\"error\":\"method not allowed\"}");
    } else {
        if(!best->concurrent) mutex_lock(&s->dispatch_mtx);
        if (!s->stop_flag && best->fn(&req, &resp, best->ud) != 0) status = 500;
        if(!best->concurrent) mutex_unlock(&s->dispatch_mtx);
    }

    /* Respect a status the handler set itself (e.g. 400 on bad input);
     * otherwise fall back to the dispatcher's status. */
    if (resp.status == 0)
        resp.status = status;

    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                     resp.status, status_reason(resp.status), resp.content_type, resp.body.len);
    if (n > 0)
        sock_send(sock, head, (size_t)n);
    if (resp.body.len > 0)
        sock_send(sock, resp.body.buf, resp.body.len);
    strbuf_free(&resp.body);
    return 0;
}

/* Bounded concurrent connection readers. Application handlers are serialized. */
typedef struct conn_job { http_server *s; sock *c; int slot; } conn_job;
static void connection_main(void *arg) {
    conn_job *job = arg;
    http_server *s = job->s;
    char *buf = NULL;
    size_t hlen = 0, blen = 0;
    int error = read_request(s, job->c, &buf, &hlen, &blen);
    int handed = 0;
    if (!error && !s->stop_flag) handed = handle_conn_buf(s, job->c, buf, hlen, blen);
    else if (!s->stop_flag) {
        char response[128];
        int n = snprintf(response, sizeof response,
            "HTTP/1.1 %d Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", error);
        sock_send(job->c, response, (size_t)n);
    }
    free(buf);
    mutex_lock(&s->mtx);
    if (s->connections[job->slot] == job->c) s->connections[job->slot] = NULL;
    if (!handed) sock_close(job->c);
    s->active--;
    cond_broadcast(&s->drained);
    mutex_unlock(&s->mtx);
    free(job);
}

int http_server_serve(http_server *s) {
    if (!s || !s->listener) return -1;
    while (!s->stop_flag) {
        sock *c = sock_accept(s->listener, 100);
        if (!c) continue;
        sock_set_timeout(c, 1000);
        mutex_lock(&s->mtx);
        int i = 0;
        while (i < 16 && s->connections[i]) i++;
        conn_job *job = i < 16 && s->active < 16 && !s->stop_flag ? malloc(sizeof(*job)) : NULL;
        if (!job) { mutex_unlock(&s->mtx); sock_close(c); continue; }
        *job = (conn_job){s, c, i};
        s->connections[i] = c; s->active++;
        thread_t *worker = thread_create(connection_main, job);
        if (!worker) { s->connections[i] = NULL; s->active--; free(job); sock_close(c); }
        else thread_detach(worker);
        mutex_unlock(&s->mtx);
    }
    mutex_lock(&s->mtx);
    while (s->active) cond_wait(&s->drained, &s->mtx);
    mutex_unlock(&s->mtx);
    return 0;
}

void http_resp_append(http_response *resp, const char *s) {
    if (!resp || !s)
        return;
    strbuf_append(&resp->body, s);
}

void http_resp_appendf(http_response *resp, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;

    if (!resp)
        return;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    strbuf_append(&resp->body, tmp);
}

void http_resp_json(http_response *resp, const char *json) {
    if (!resp || !json)
        return;
    snprintf(resp->content_type, sizeof(resp->content_type), "application/json");
    strbuf_append(&resp->body, json);
}
