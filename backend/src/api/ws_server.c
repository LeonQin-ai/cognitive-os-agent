/* ws_server.c — WebSocket server: handshake + per-client reader threads.
 *
 * A client thread loops: flush its outbound queue, wait for readability (200ms
 * so pushed events are delivered promptly), read and parse frames. Text frames
 * go to the registered handler; pings are answered with pongs; close frames and
 * socket errors terminate the thread, which then removes itself from the
 * server's client list and frees its own state. */
#include "cognitive-os-agent/api/ws_server.h"
#include "cognitive-os-agent/api/websocket.h"
#include "cognitive-os-agent/os/os_socket.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct coa_ws_client {
    int id;
    coa_socket *sock;
    struct coa_ws_server *server;
    coa_mutex send_mtx;
    coa_strbuf queue;        /* pending outbound messages, '\n'-separated */
    volatile int closed;
} coa_ws_client;

struct coa_ws_server {
    coa_mutex mtx;
    coa_ws_client **clients;
    size_t count, cap;
    int next_id;
    coa_ws_msg_handler on_msg;
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

static void ws_send_frame(coa_ws_client *c, int opcode, const unsigned char *payload, size_t len) {
    if (c->closed) return;
    size_t out_len = 0;
    char *frame = coa_ws_build_frame(opcode, payload, len, 0, &out_len);
    if (!frame) return;
    int n = coa_sock_send(c->sock, frame, out_len);
    free(frame);
    if (n != (int)out_len) c->closed = 1;
}

/* Send all complete queued messages (those ending with '\n') as text frames. */
static void ws_client_flush(coa_ws_client *c) {
    char local[16384];
    size_t local_len = 0;
    coa_mutex_lock(&c->send_mtx);
    if (c->queue.len) {
        size_t take = c->queue.len > sizeof(local) ? sizeof(local) : c->queue.len;
        /* only consume up to the last complete '\n' so no partial message is lost */
        size_t keep = take;
        while (keep > 0 && c->queue.buf[keep - 1] != '\n') keep--;
        if (keep == 0) { coa_mutex_unlock(&c->send_mtx); return; }
        memcpy(local, c->queue.buf, keep);
        memmove(c->queue.buf, c->queue.buf + keep, c->queue.len - keep);
        c->queue.len -= keep;
        local_len = keep;
    }
    coa_mutex_unlock(&c->send_mtx);
    size_t start = 0;
    for (size_t i = 0; i < local_len; i++) {
        if (local[i] == '\n') {
            ws_send_frame(c, 0x1, (unsigned char *)local + start, i - start);
            start = i + 1;
        }
    }
}

static void ws_server_remove(coa_ws_server *s, coa_ws_client *c) {
    coa_mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++) {
        if (s->clients[i] == c) {
            memmove(&s->clients[i], &s->clients[i + 1],
                    (s->count - i - 1) * sizeof(coa_ws_client *));
            s->count--;
            break;
        }
    }
    coa_mutex_unlock(&s->mtx);
}

static void ws_client_loop(void *arg) {
    coa_ws_client *c = (coa_ws_client *)arg;
    unsigned char rbuf[8192];
    while (!c->closed) {
        ws_client_flush(c);
        if (c->closed) break;
        int rd = coa_sock_wait_readable(c->sock, 200);
        if (rd < 0) break;
        if (rd == 0) continue;
        int n = coa_sock_recv(c->sock, rbuf, sizeof(rbuf));
        if (n <= 0) break;
        size_t off = 0;
        while (off < (size_t)n) {
            size_t plen = 0;
            size_t total = ws_frame_len(rbuf + off, (size_t)n - off, &plen);
            if (total == 0) break;
            unsigned char payload[8192];
            size_t parsed = 0;
            int opcode = 0, fin = 0;
            if (coa_ws_parse_frame(rbuf + off, total, payload, &parsed, &opcode, &fin) != 0) break;
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
    if (c->sock) coa_sock_close(c->sock);
    ws_server_remove(c->server, c);
    coa_strbuf_free(&c->queue);
    coa_mutex_destroy(&c->send_mtx);
    free(c);
}

/* ---------- public API ---------- */

coa_ws_server *coa_ws_server_new(void) {
    coa_ws_server *s = calloc(1, sizeof(coa_ws_server));
    if (!s) return NULL;
    coa_mutex_init(&s->mtx);
    return s;
}

void coa_ws_server_on_message(coa_ws_server *s, coa_ws_msg_handler fn, void *ud) {
    if (!s) return;
    s->on_msg = fn;
    s->ud = ud;
}

int coa_ws_server_accept(coa_ws_server *s, coa_socket *sock, const char *sec_ws_key) {
    if (!s || !sock || !sec_ws_key) return -1;
    char accept_key[29];
    coa_ws_accept_key(sec_ws_key, accept_key);
    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", accept_key);
    if (coa_sock_send(sock, resp, (size_t)n) != n) { coa_sock_close(sock); return -1; }

    coa_ws_client *c = calloc(1, sizeof(coa_ws_client));
    if (!c) { coa_sock_close(sock); return -1; }
    c->sock = sock;
    c->server = s;
    coa_mutex_init(&c->send_mtx);
    coa_strbuf_init(&c->queue);

    coa_mutex_lock(&s->mtx);
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 8;
        coa_ws_client **nc = realloc(s->clients, cap * sizeof(coa_ws_client *));
        if (!nc) {
            coa_mutex_unlock(&s->mtx);
            coa_sock_close(sock);
            coa_strbuf_free(&c->queue);
            coa_mutex_destroy(&c->send_mtx);
            free(c);
            return -1;
        }
        s->clients = nc;
        s->cap = cap;
    }
    c->id = s->next_id++;
    s->clients[s->count++] = c;
    coa_mutex_unlock(&s->mtx);

    coa_thread *t = coa_thread_create(ws_client_loop, c);
    if (!t) {
        ws_server_remove(s, c);
        coa_sock_close(sock);
        coa_strbuf_free(&c->queue);
        coa_mutex_destroy(&c->send_mtx);
        free(c);
        return -1;
    }
    coa_thread_detach(t);
    return 0;
}

void coa_ws_server_broadcast(coa_ws_server *s, const char *json_text) {
    if (!s || !json_text) return;
    size_t len = strlen(json_text);
    coa_mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++) {
        coa_ws_client *c = s->clients[i];
        if (c->closed) continue;
        coa_mutex_lock(&c->send_mtx);
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
        coa_mutex_unlock(&c->send_mtx);
    }
    coa_mutex_unlock(&s->mtx);
}

int coa_ws_server_count(coa_ws_server *s) {
    if (!s) return 0;
    coa_mutex_lock(&s->mtx);
    int n = (int)s->count;
    coa_mutex_unlock(&s->mtx);
    return n;
}

void coa_ws_server_free(coa_ws_server *s) {
    if (!s) return;
    /* close every client socket; the reader threads exit on their own (within
     * ~200ms) and remove themselves. A short barrier makes shutdown tidy. */
    coa_mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++) {
        s->clients[i]->closed = 1;
        if (s->clients[i]->sock) coa_sock_close(s->clients[i]->sock);
    }
    coa_mutex_unlock(&s->mtx);
    coa_time_sleep_ms(400);
    coa_mutex_destroy(&s->mtx);
    free(s->clients);
    free(s);
}
