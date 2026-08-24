/* ws_server.h — minimal WebSocket server on top of the RFC6455 primitives.
 * Accepts upgraded TCP sockets, performs the 101 handshake, spawns one reader
 * thread per client, and supports text broadcast to all connected clients.
 * Inbound text messages are forwarded to an optional handler. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_ws_server ca_ws_server;
typedef struct ca_socket ca_socket;

/* Called with each inbound text message (NUL-terminated, borrowed). */
typedef void (*ca_ws_msg_handler)(const char *text, void *ud);

ca_ws_server *ca_ws_server_new(void);
void ca_ws_server_free(ca_ws_server *s);

/* Set the inbound-message handler (default: ignore). */
void ca_ws_server_on_message(ca_ws_server *s, ca_ws_msg_handler fn, void *ud);

/* Accept an upgraded socket: send the 101 handshake (using the client's
 * Sec-WebSocket-Key), register the client, and spawn its reader thread.
 * Takes ownership of sock. Returns 0 ok, -1 error. */
int ca_ws_server_accept(ca_ws_server *s, ca_socket *sock, const char *sec_ws_key);

/* Broadcast a text message to every connected client. */
void ca_ws_server_broadcast(ca_ws_server *s, const char *json_text);

/* Number of connected clients. */
int ca_ws_server_count(ca_ws_server *s);

#ifdef __cplusplus
}
#endif
