/* ws_server.h — minimal WebSocket server on top of the RFC6455 primitives.
 * Accepts upgraded TCP sockets, performs the 101 handshake, spawns one reader
 * thread per client, and supports text broadcast to all connected clients.
 * Inbound text messages are forwarded to an optional handler. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_ws_server coa_ws_server;
typedef struct coa_socket coa_socket;

/* Called with each inbound text message (NUL-terminated, borrowed). */
typedef void (*coa_ws_msg_handler)(const char *text, void *ud);

coa_ws_server *coa_ws_server_new(void);
void coa_ws_server_free(coa_ws_server *s);

/* Set the inbound-message handler (default: ignore). */
void coa_ws_server_on_message(coa_ws_server *s, coa_ws_msg_handler fn, void *ud);

/* Accept an upgraded socket: send the 101 handshake (using the client's
 * Sec-WebSocket-Key), register the client, and spawn its reader thread.
 * Takes ownership of sock. Returns 0 ok, -1 error. */
int coa_ws_server_accept(coa_ws_server *s, coa_socket *sock, const char *sec_ws_key);

/* Broadcast a text message to every connected client. */
void coa_ws_server_broadcast(coa_ws_server *s, const char *json_text);

/* Number of connected clients. */
int coa_ws_server_count(coa_ws_server *s);

#ifdef __cplusplus
}
#endif
