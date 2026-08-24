/* ws_server.c — WebSocket server: handshake + per-client reader threads.
 *
 * A client thread loops: flush its outbound queue, wait for readability (200ms
 * so pushed events are delivered promptly), read and parse frames. Text frames
 * go to the registered handler; pings are answered with pongs; close frames and
 * socket errors terminate the thread, which then removes itself from the
 * server's client list and frees its own state. */
#include "cagent/api/ws_server.h"
#include "cagent/api/websocket.h"
#include "cagent/os/os_socket.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/os_time.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct ca_ws_client {
    int id;
    ca_socket *sock;
    struct ca_ws_server *server;
    ca_mutex send_mtx;
    ca_strbuf queue;        /* pending outbound messages, '\n'-separated */
    volatile int closed;
} ca_ws_client;

struct ca_ws_server {
    ca_mutex mtx;
    ca_ws_client **clients;
    size_t count, cap;
    int next_id;
    ca_ws_msg_handler on_msg;
    void *ud;
};

/* Total wire size of the frame starting at b (0 if incomplete). */
static size_t ws_frame_len(const unsigned char *b, size_t len, size_t *payload_len) {
    if (len < 2) return 0;
    size_t plen = b[1] & 0x7F;
    size_t off = 2;
    if (plen == 126) {
        if (len < off + 2) return 0;
        plen = ((size_t)b[2] << 8) | b[3];
        off += 2;
    } else if (plen == 127) {
        if (len < off + 8) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | b[off + i];
        off += 8;
    }
    if (b[1] & 0x80) off += 4; /* masked client frames */
    if (len < off + plen) return 0;
    if (payload_len) *payload_len = plen;
    return off + plen;
}

static void ws_send_frame(ca_ws_client *c, int opcode, const unsigned char *payload, size_t len) {
    if (c->closed) return;
    size_t out_len = 0;
    char *frame = ca_ws_build_frame(opcode, payload, len, 0, &out_len);
    if (!frame) return;
    int n = ca_sock_send(c->sock, frame, out_len);
    free(frame);
    if (n != (int)out_len) c->closed = 1;
}

/* Send all complete queued messages (those ending with '\n') as text frames. */
static void ws_client_flush(ca_ws_client *c) {
    char local[16384];
    size_t local_len = 0;
    ca_mutex_lock(&c->send_mtx);
    if (c->queue.len) {
        size_t take = c->queue.len > sizeof(local) ? sizeof(local) : c->queue.len;
        /* only consume up to the last complete '\n' so no partial message is lost */
        size_t keep = take;
        while (keep > 0 && c->queue.buf[keep - 1] != '\n') keep--;
        if (keep == 0) { ca_mutex_unlock(&c->send_mtx); return; }
        memcpy(local, c->queue.buf, keep);
        memmove(c->queue.buf, c->queue.buf + keep, c->queue.len - keep);
        c->queue.len -= keep;
        local_len = keep;
    }
    ca_mutex_unlock(&c->send_mtx);
    size_t start = 0;
    for (size_t i = 0; i < local_len; i++) {
        if (local[i] == '\n') {
            ws_send_frame(c, 0x1, (unsigned char *)local + start, i - start);
            start = i + 1;
        }
    }
}

static void ws_server_remove(ca_ws_server *s, ca_ws_client *c) {
    ca_mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++) {
        if (s->clients[i] == c) {
            memmove(&s->clients[i], &s->clients[i + 1],
                    (s->count - i - 1) * sizeof(ca_ws_client *));
            s->count--;
            break;
        }
    }
    ca_mutex_unlock(&s->mtx);
}

static void ws_client_loop(void *arg) {
    ca_ws_client *c = (ca_ws_client *)arg;
    unsigned char rbuf[8192];
    while (!c->closed) {
        ws_client_flush(c);
        if (c->closed) break;
        int rd = ca_sock_wait_readable(c->sock, 200);
        if (rd < 0) break;
        if (rd == 0) continue;
        int n = ca_sock_recv(c->sock, rbuf, sizeof(rbuf));
        if (n <= 0) break;
        size_t off = 0;
        while (off < (size_t)n) {
            size_t plen = 0;
            size_t total = ws_frame_len(rbuf + off, (size_t)n - off, &plen);
            if (total == 0) break;
            unsigned char payload[8192];
            size_t parsed = 0;
            int opcode = 0, fin = 0;
            if (ca_ws_parse_frame(rbuf + off, total, payload, &parsed, &opcode, &fin) != 0) break;
            off += total;
            switch (opcode) {
                case 0x1: /* text */
                    if (parsed >= sizeof(payload)) parsed = sizeof(payload) - 1;
                    payload[parsed] = '\0';
                    if (c->server->on_msg)
                        c->server->on_msg((const char *)payload, c->server->ud);
                    break;
                case 0x9: /* ping */
                    ws_send_frame(c, 0xA, payload, parsed); /* pong */
                    break;
                case 0x8: /* close */
                    ws_send_frame(c, 0x8, payload, parsed);
                    c->closed = 1;
                    break;
                default:
                    break;
            }
            if (c->closed) break;
        }
    }
    if (c->sock) ca_sock_close(c->sock);
    ws_server_remove(c->server, c);
    ca_strbuf_free(&c->queue);
    ca_mutex_destroy(&c->send_mtx);
    free(c);
}

/* ---------- public API ---------- */

ca_ws_server *ca_ws_server_new(void) {
    ca_ws_server *s = calloc(1, sizeof(ca_ws_server));
    if (!s) return NULL;
    ca_mutex_init(&s->mtx);
    return s;
}

void ca_ws_server_on_message(ca_ws_server *s, ca_ws_msg_handler fn, void *ud) {
    if (!s) return;
    s->on_msg = fn;
    s->ud = ud;
}

int ca_ws_server_accept(ca_ws_server *s, ca_socket *sock, const char *sec_ws_key) {
    if (!s || !sock || !sec_ws_key) return -1;
    char accept_key[29];
    ca_ws_accept_key(sec_ws_key, accept_key);
    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", accept_key);
    if (ca_sock_send(sock, resp, (size_t)n) != n) { ca_sock_close(sock); return -1; }

    ca_ws_client *c = calloc(1, sizeof(ca_ws_client));
    if (!c) { ca_sock_close(sock); return -1; }
    c->sock = sock;
    c->server = s;
    ca_mutex_init(&c->send_mtx);
    ca_strbuf_init(&c->queue);

    ca_mutex_lock(&s->mtx);
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 8;
        ca_ws_client **nc = realloc(s->clients, cap * sizeof(ca_ws_client *));
        if (!nc) {
            ca_mutex_unlock(&s->mtx);
            ca_sock_close(sock);
            ca_strbuf_free(&c->queue);
            ca_mutex_destroy(&c->send_mtx);
            free(c);
            return -1;
        }
        s->clients = nc;
        s->cap = cap;
    }
    c->id = s->next_id++;
    s->clients[s->count++] = c;
    ca_mutex_unlock(&s->mtx);

    ca_thread *t = ca_thread_create(ws_client_loop, c);
    if (!t) {
        ws_server_remove(s, c);
        ca_sock_close(sock);
        ca_strbuf_free(&c->queue);
        ca_mutex_destroy(&c->send_mtx);
        free(c);
        return -1;
    }
    ca_thread_detach(t);
    return 0;
}

void ca_ws_server_broadcast(ca_ws_server *s, const char *json_text) {
    if (!s || !json_text) return;
    size_t len = strlen(json_text);
    ca_mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++) {
        ca_ws_client *c = s->clients[i];
        if (c->closed) continue;
        ca_mutex_lock(&c->send_mtx);
        if (c->queue.cap < c->queue.len + len + 2) {
            size_t cap = (c->queue.len + len + 2) * 2;
            char *nb = realloc(c->queue.buf, cap);
            if (nb) { c->queue.buf = nb; c->queue.cap = cap; }
        }
        if (c->queue.cap >= c->queue.len + len + 2) {
            memcpy(c->queue.buf + c->queue.len, json_text, len);
            c->queue.len += len;
            c->queue.buf[c->queue.len++] = '\n';
        }
        ca_mutex_unlock(&c->send_mtx);
    }
    ca_mutex_unlock(&s->mtx);
}

int ca_ws_server_count(ca_ws_server *s) {
    if (!s) return 0;
    ca_mutex_lock(&s->mtx);
    int n = (int)s->count;
    ca_mutex_unlock(&s->mtx);
    return n;
}

void ca_ws_server_free(ca_ws_server *s) {
    if (!s) return;
    /* close every client socket; the reader threads exit on their own (within
     * ~200ms) and remove themselves. A short barrier makes shutdown tidy. */
    ca_mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++) {
        s->clients[i]->closed = 1;
        if (s->clients[i]->sock) ca_sock_close(s->clients[i]->sock);
    }
    ca_mutex_unlock(&s->mtx);
    ca_time_sleep_ms(400);
    ca_mutex_destroy(&s->mtx);
    free(s->clients);
    free(s);
}
