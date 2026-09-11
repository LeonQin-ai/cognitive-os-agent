/* ws_server.h — minimal WebSocket server on top of the RFC6455 primitives.
 * Accepts upgraded TCP sockets, performs the 101 handshake, spawns one reader
 * thread per client, and supports text broadcast to all connected clients.
 * Inbound text messages are forwarded to an optional handler. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ws_server ws_server;
typedef struct sock sock;

/* Called with each inbound text message (NUL-terminated, borrowed). */
typedef void (*ws_msg_handler)(const char *text, void *ud);

ws_server *ws_server_new(void);
void ws_server_free(ws_server *s);

/* Set the inbound-message handler (default: ignore). */
void ws_server_on_message(ws_server *s, ws_msg_handler fn, void *ud);

/* Accept an upgraded socket: send the 101 handshake (using the client's
 * Sec-WebSocket-Key), register the client, and spawn its reader thread.
 * Takes ownership of sock. Returns 0 ok, -1 error. */
int ws_server_accept(ws_server *s, sock *sock, const char *sec_ws_key);

/* Broadcast a text message to every connected client. */
void ws_server_broadcast(ws_server *s, const char *json_text);

#ifdef __cplusplus
}
#endif
