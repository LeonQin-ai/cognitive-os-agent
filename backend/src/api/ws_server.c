/* ws_server.c — WebSocket server: handshake + per-client reader threads.
 *
 * A client thread loops: flush its outbound queue, wait for readability (200ms
 * so pushed events are delivered promptly), read and parse frames. Text frames
 * go to the registered handler; pings are answered with pongs; close frames and
 * socket errors terminate the thread, which then removes itself from the
 * server's client list and frees its own state. */
#include "api/ws_server.h"
#include "api/websocket.h"
#include "os/os_socket.h"
#include "os/os_thread.h"
#include "os/os_time.h"
#include "infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#define MAX_WS_PAYLOAD (1024u * 1024u)

typedef struct ws_client {
    int id;
    sock *sock;
    struct ws_server *server;
    mutex_t send_mtx;
    strbuf queue; /* pending outbound messages, '\n'-separated */
    strbuf fragment; /* fragmented inbound text message */
    int fragment_opcode;
    _Atomic int closed;
} ws_client;

struct ws_server {
    mutex_t mtx;
    ws_client **clients;
    size_t count, cap;
    int next_id;
    ws_msg_handler on_msg;
    void *ud;
};

/* Total wire size of the frame starting at b (0 if incomplete). */
static size_t ws_frame_len(const unsigned char *b, size_t len, size_t *payload_len) {
    size_t plen;
    size_t off = 2;

    if (len < 2)
        return 0;
    plen = b[1] & 0x7F;
    if (plen == 126) {
        if (len < off + 2)
            return 0;
        plen = ((size_t)b[2] << 8) | b[3];
        off += 2;
    } else if (plen == 127) {
        if (len < off + 8)
            return 0;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | b[off + i];
        off += 8;
    }

    if (plen > MAX_WS_PAYLOAD || plen > SIZE_MAX - off - 4)
        return SIZE_MAX;
    if (b[1] & 0x80)
        off += 4; /* masked client frames */
    if (len < off + plen)
        return 0;
    if (payload_len)
        *payload_len = plen;
    return off + plen;
}

static void ws_send_frame(ws_client *c, int opcode, const unsigned char *payload, size_t len) {
    size_t out_len = 0;
    char *frame;
    int n;

    if (c->closed)
        return;
    frame = ws_build_frame(opcode, payload, len, 0, &out_len);
    if (!frame)
        return;
    n = sock_send(c->sock, frame, out_len);
    free(frame);
    if (n != (int)out_len)
        c->closed = 1;
}

/* Send all complete queued messages (those ending with '\n') as text frames. */
static void ws_client_flush(ws_client *c) {
    char *local = NULL;
    size_t local_len = 0;
    size_t start = 0;

    mutex_lock(&c->send_mtx);
    if (c->queue.len) {
        size_t keep = c->queue.len;
        while (keep > 0 && c->queue.buf[keep - 1] != '\n') keep--;
        if (keep == 0) {
            mutex_unlock(&c->send_mtx);
            return;
        }
        local = malloc(keep);
        if (!local) { mutex_unlock(&c->send_mtx); return; }
        memcpy(local, c->queue.buf, keep);
        memmove(c->queue.buf, c->queue.buf + keep, c->queue.len - keep);
        c->queue.len -= keep;
        local_len = keep;
    }

    mutex_unlock(&c->send_mtx);
    for (size_t i = 0; i < local_len; i++) {
        if (local[i] == '\n') {
            ws_send_frame(c, 0x1, (unsigned char *)local + start, i - start);
            start = i + 1;
        }
    }
    free(local);
}

static void ws_server_remove(ws_server *s, ws_client *c) {
    mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++) {
        if (s->clients[i] == c) {
            memmove(&s->clients[i], &s->clients[i + 1], (s->count - i - 1) * sizeof(ws_client *));
            s->count--;
            break;
        }
    }

    mutex_unlock(&s->mtx);
}

static void ws_client_loop(void *arg) {
    ws_client *c = (ws_client *)arg;
    unsigned char chunk[8192];
    unsigned char *rbuf = NULL;
    size_t rlen = 0, rcap = 0;
    while (!c->closed) {
        ws_client_flush(c);
        if (c->closed)
            break;
        int rd = sock_wait_readable(c->sock, 200);
        if (rd < 0)
            break;
        if (rd == 0)
            continue;
        int n = sock_recv(c->sock, chunk, sizeof(chunk));
        if (n <= 0)
            break;
        if (rlen + (size_t)n > MAX_WS_PAYLOAD + 14) {
            c->closed = 1;
            break;
        }
        if (rlen + (size_t)n > rcap) {
            size_t cap = rcap ? rcap : 8192;
            while (cap < rlen + (size_t)n)
                cap *= 2;
            unsigned char *nb = realloc(rbuf, cap);
            if (!nb) { c->closed = 1; break; }
            rbuf = nb;
            rcap = cap;
        }
        memcpy(rbuf + rlen, chunk, (size_t)n);
        rlen += (size_t)n;
        size_t off = 0;
        while (off < rlen) {
            size_t plen = 0;
            size_t total = ws_frame_len(rbuf + off, rlen - off, &plen);
            if (total == SIZE_MAX) { c->closed = 1; break; }
            if (total == 0)
                break;
            unsigned char *payload = malloc(plen + 1);
            if (!payload) { c->closed = 1; break; }
            size_t parsed = 0;
            int opcode = 0, fin = 0;
            if (ws_parse_frame(rbuf + off, total, payload, &parsed, &opcode, &fin) != 0) {
                free(payload);
                c->closed = 1;
                break;
            }
            off += total;
            switch (opcode) {
            case 0x1: /* text */
                payload[parsed] = '\0';
                if (fin && c->fragment_opcode == 0 && c->server->on_msg)
                    c->server->on_msg((const char *)payload, c->server->ud);
                else if (!fin && c->fragment_opcode == 0) {
                    c->fragment_opcode = opcode;
                    strbuf_append_n(&c->fragment, (const char *)payload, parsed);
                } else {
                    c->closed = 1; /* nested fragmented message */
                }
                break;
            case 0x0: /* continuation */
                if (c->fragment_opcode == 0) {
                    c->closed = 1;
                    break;
                }
                {
                    size_t before = c->fragment.len;
                    if (parsed > MAX_WS_PAYLOAD - before) {
                        c->closed = 1;
                        break;
                    }
                    strbuf_append_n(&c->fragment, (const char *)payload, parsed);
                    if (c->fragment.len != before + parsed) {
                        c->closed = 1;
                        break;
                    }
                }
                if (fin) {
                    if (c->server->on_msg)
                        c->server->on_msg(c->fragment.buf ? c->fragment.buf : "", c->server->ud);
                    strbuf_free(&c->fragment);
                    strbuf_init(&c->fragment);
                    c->fragment_opcode = 0;
                }
                break;
            case 0x9:                                   /* ping */
                if (!fin || parsed > 125) c->closed = 1;
                else ws_send_frame(c, 0xA, payload, parsed); /* pong */
                break;
            case 0x8: /* close */
                if (fin && parsed <= 125)
                    ws_send_frame(c, 0x8, payload, parsed);
                c->closed = 1;
                break;
            default:
                c->closed = 1;
                break;
            }
            free(payload);
            if (c->closed)
                break;
        }
        if (off > 0 && off <= rlen) {
            memmove(rbuf, rbuf + off, rlen - off);
            rlen -= off;
        }
    }

    free(rbuf);
    if (c->sock)
        sock_close(c->sock);
    ws_server_remove(c->server, c);
    strbuf_free(&c->queue);
    strbuf_free(&c->fragment);
    mutex_destroy(&c->send_mtx);
    free(c);
}

/* ---------- public API ---------- */

ws_server *ws_server_new(void) {
    ws_server *s = calloc(1, sizeof(ws_server));
    if (!s)
        return NULL;
    mutex_init(&s->mtx);
    return s;
}

void ws_server_on_message(ws_server *s, ws_msg_handler fn, void *ud) {
    if (!s)
        return;
    s->on_msg = fn;
    s->ud = ud;
}

int ws_server_accept(ws_server *s, sock *sock, const char *sec_ws_key) {
    char accept_key[29];
    char resp[512];
    thread_t *t;

    if (!s || !sock || !sec_ws_key)
        return -1;
    ws_accept_key(sec_ws_key, accept_key);
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept_key);
    if (sock_send(sock, resp, (size_t)n) != n) {
        sock_close(sock);
        return -1;
    }

    ws_client *c = calloc(1, sizeof(ws_client));
    if (!c) {
        sock_close(sock);
        return -1;
    }

    c->sock = sock;
    c->server = s;
    mutex_init(&c->send_mtx);
    strbuf_init(&c->queue);
    strbuf_init(&c->fragment);

    mutex_lock(&s->mtx);
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 8;
        ws_client **nc = realloc(s->clients, cap * sizeof(ws_client *));
        if (!nc) {
            mutex_unlock(&s->mtx);
            sock_close(sock);
            strbuf_free(&c->queue);
            mutex_destroy(&c->send_mtx);
            free(c);
            return -1;
        }
        s->clients = nc;
        s->cap = cap;
    }

    c->id = s->next_id++;
    s->clients[s->count++] = c;
    mutex_unlock(&s->mtx);

    t = thread_create(ws_client_loop, c);
    if (!t) {
        ws_server_remove(s, c);
        sock_close(sock);
        strbuf_free(&c->queue);
        mutex_destroy(&c->send_mtx);
        free(c);
        return -1;
    }

    thread_detach(t);
    return 0;
}

void ws_server_broadcast(ws_server *s, const char *json_text) {
    size_t len;

    if (!s || !json_text)
        return;
    len = strlen(json_text);
    mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++) {
        ws_client *c = s->clients[i];
        if (c->closed)
            continue;
        mutex_lock(&c->send_mtx);
        if (c->queue.cap < c->queue.len + len + 2) {
            size_t cap = (c->queue.len + len + 2) * 2;
            char *nb = realloc(c->queue.buf, cap);
            if (nb) {
                c->queue.buf = nb;
                c->queue.cap = cap;
            }
        }
        if (c->queue.cap >= c->queue.len + len + 2) {
            memcpy(c->queue.buf + c->queue.len, json_text, len);
            c->queue.len += len;
            c->queue.buf[c->queue.len++] = '\n';
        }
        mutex_unlock(&c->send_mtx);
    }

    mutex_unlock(&s->mtx);
}

void ws_server_free(ws_server *s) {
    if (!s)
        return;
    /* close every client socket; the reader threads exit on their own (within
     * ~200ms) and remove themselves. A short barrier makes shutdown tidy. */
    mutex_lock(&s->mtx);
    for (size_t i = 0; i < s->count; i++) {
        s->clients[i]->closed = 1;
        if (s->clients[i]->sock)
            sock_close(s->clients[i]->sock);
    }

    mutex_unlock(&s->mtx);
    time_sleep_ms(400);
    mutex_destroy(&s->mtx);
    free(s->clients);
    free(s);
}
