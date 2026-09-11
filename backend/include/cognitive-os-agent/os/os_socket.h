/* os_socket.h — cross-platform TCP sockets (client + listener). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sock sock;
typedef struct listener listener;

/* Initialize the socket subsystem (WSAStartup on Windows). Call once. */
int sock_init(void);
void sock_cleanup(void);

/* Resolve host and connect. timeout_ms <= 0 means no timeout.
 * Returns connected socket, or NULL on failure (see sock_error). */
sock *sock_connect(const char *host, uint16_t port, int timeout_ms);

/* Send len bytes. Returns bytes sent, or -1 on error. */
int sock_send(sock *s, const void *data, size_t len);
/* Receive up to cap bytes. Returns bytes read (>0), 0 on EOF, -1 on error/timeout. */
int sock_recv(sock *s, void *buf, size_t cap);

/* Wait up to timeout_ms for the socket to become readable.
 * Returns 1 readable, 0 timeout, -1 error. */
int sock_wait_readable(sock *s, int timeout_ms);

/* Listen on a specific IPv4 address (host, e.g. "127.0.0.1"); NULL host = any
 * interface. NULL on failure. */
listener *listen_addr(const char *host, uint16_t port);
/* Accept a connection; blocks up to timeout_ms (<=0 = forever). NULL on timeout/error. */
sock *sock_accept(listener *l, int timeout_ms);

void sock_close(sock *s);
void listener_close(listener *l);
const char *sock_error(void);

#ifdef __cplusplus
}
#endif
