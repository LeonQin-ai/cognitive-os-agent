/* os_socket.h — cross-platform TCP sockets (client + listener). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_socket coa_socket;
typedef struct coa_listener coa_listener;

/* Initialize the socket subsystem (WSAStartup on Windows). Call once. */
int coa_sock_init(void);
void coa_sock_cleanup(void);

/* Resolve host and connect. timeout_ms <= 0 means no timeout.
 * Returns connected socket, or NULL on failure (see coa_sock_error). */
coa_socket *coa_sock_connect(const char *host, uint16_t port, int timeout_ms);

/* Send len bytes. Returns bytes sent, or -1 on error. */
int coa_sock_send(coa_socket *s, const void *data, size_t len);
/* Receive up to cap bytes. Returns bytes read (>0), 0 on EOF, -1 on error/timeout. */
int coa_sock_recv(coa_socket *s, void *buf, size_t cap);
/* Receive until any byte in delim is seen or cap reached. Returns bytes or -1. */
int coa_sock_recv_until(coa_socket *s, char *buf, size_t cap, const char *delim);

/* Toggle blocking mode on a connected socket. Returns 0 ok, -1 error. */
int coa_sock_set_blocking(coa_socket *s, int blocking);
/* Wait up to timeout_ms for the socket to become readable.
 * Returns 1 readable, 0 timeout, -1 error. */
int coa_sock_wait_readable(coa_socket *s, int timeout_ms);

/* Listen on a specific IPv4 address (host, e.g. "127.0.0.1"); NULL host = any
 * interface. NULL on failure. */
coa_listener *coa_listen_addr(const char *host, uint16_t port);
/* Listen on a port (any interface). NULL on failure. */
coa_listener *coa_listen(uint16_t port);
/* Accept a connection; blocks up to timeout_ms (<=0 = forever). NULL on timeout/error. */
coa_socket *coa_accept(coa_listener *l, int timeout_ms);

void coa_sock_close(coa_socket *s);
void coa_listener_close(coa_listener *l);
const char *coa_sock_error(void);

#ifdef __cplusplus
}
#endif
