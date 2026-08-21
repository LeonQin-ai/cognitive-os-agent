/* os_socket.h — cross-platform TCP sockets (client + listener). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_socket ca_socket;
typedef struct ca_listener ca_listener;

/* Initialize the socket subsystem (WSAStartup on Windows). Call once. */
int ca_sock_init(void);
void ca_sock_cleanup(void);

/* Resolve host and connect. timeout_ms <= 0 means no timeout.
 * Returns connected socket, or NULL on failure (see ca_sock_error). */
ca_socket *ca_sock_connect(const char *host, uint16_t port, int timeout_ms);

/* Send len bytes. Returns bytes sent, or -1 on error. */
int ca_sock_send(ca_socket *s, const void *data, size_t len);
/* Receive up to cap bytes. Returns bytes read (>0), 0 on EOF, -1 on error/timeout. */
int ca_sock_recv(ca_socket *s, void *buf, size_t cap);
/* Receive until any byte in delim is seen or cap reached. Returns bytes or -1. */
int ca_sock_recv_until(ca_socket *s, char *buf, size_t cap, const char *delim);

/* Listen on a specific IPv4 address (host, e.g. "127.0.0.1"); NULL host = any
 * interface. NULL on failure. */
ca_listener *ca_listen_addr(const char *host, uint16_t port);
/* Listen on a port (any interface). NULL on failure. */
ca_listener *ca_listen(uint16_t port);
/* Accept a connection; blocks up to timeout_ms (<=0 = forever). NULL on timeout/error. */
ca_socket *ca_accept(ca_listener *l, int timeout_ms);

void ca_sock_close(ca_socket *s);
void ca_listener_close(ca_listener *l);
const char *ca_sock_error(void);

#ifdef __cplusplus
}
#endif
