#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cagent/os/os_socket.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#endif

struct ca_socket { int fd; };
struct ca_listener { int fd; };

#if defined(_WIN32)
#define CLOSEFD(fd) closesocket((fd))
#else
#define CLOSEFD(fd) close((fd))
#endif

static char g_err[256] = "";

static const char *sock_strerror(int err) {
#if defined(_WIN32)
    switch (err) {
        case WSAETIMEDOUT: return "connect timeout";
        case WSAECONNREFUSED: return "connection refused";
        case WSAECONNRESET: return "connection reset";
        default: return "socket error";
    }
#else
    return strerror(err);
#endif
}

static void set_err(const char *msg) { snprintf(g_err, sizeof(g_err), "%s", msg); }

#if defined(_WIN32)
/* WSAStartup must precede any Winsock call. Initialization normally happens in
 * cagent_init(), but the socket layer self-initializes too so standalone users
 * (e.g. tests that call the LLM adapters directly) work without it. The guard
 * flag makes this idempotent. */
static int wsa_started = 0;
static int wsa_start(void) {
    if (!wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { set_err("WSAStartup failed"); return -1; }
        wsa_started = 1;
    }
    return 0;
}
#endif

int ca_sock_init(void) {
#if defined(_WIN32)
    return wsa_start();
#endif
    return 0;
}

void ca_sock_cleanup(void) {
#if defined(_WIN32)
    if (wsa_started) { WSACleanup(); wsa_started = 0; }
#endif
}

const char *ca_sock_error(void) { return g_err; }

static void set_nonblock(int fd, int nb) {
#if defined(_WIN32)
    u_long mode = nb ? 1 : 0;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

ca_socket *ca_sock_connect(const char *host, uint16_t port, int timeout_ms) {
#if defined(_WIN32)
    if (wsa_start() != 0) return NULL;
#endif
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        set_err("getaddrinfo failed");
        return NULL;
    }
    int fd = -1;
    int err = 0;
    /* count candidate addresses so we can split the connect budget: a single
     * unreachable address (e.g. an IPv6 ::1 attempt when the server only listens
     * on IPv4) must not be able to consume the whole timeout before we fall back. */
    int naddrs = 0;
    struct addrinfo *cnt;
    for (cnt = res; cnt; cnt = cnt->ai_next) naddrs++;
    struct addrinfo *ai;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        set_nonblock(fd, 1);
        int r = connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen);
        if (r == 0) { err = 0; break; }
#if defined(_WIN32)
        int we = WSAGetLastError();
        if (we != WSAEWOULDBLOCK) { err = we; CLOSEFD(fd); fd = -1; continue; }
#else
        if (errno != EINPROGRESS) { err = errno; CLOSEFD(fd); fd = -1; continue; }
#endif
        /* wait for writability; budget per address = timeout split across all,
         * bounded to [2s, 5s] so a dead address is skipped quickly */
        fd_set wset;
        FD_ZERO(&wset);
#if defined(_WIN32)
        FD_SET((SOCKET)fd, &wset); /* fd_array is SOCKET; cast to avoid sign-compare */
#else
        FD_SET(fd, &wset);
#endif
        int per = naddrs > 1 && timeout_ms > 0 ? timeout_ms / naddrs : timeout_ms;
        if (per > 5000) per = 5000;
        /* Respect an explicit small budget: never RAISE a caller's timeout
         * above what they asked for. The 2s floor is only a default for
         * callers that didn't specify one (health probes pass, e.g., 300ms
         * and must fail fast rather than hang the single-threaded server). */
        if (timeout_ms <= 0) per = 2000;
        struct timeval tv;
        tv.tv_sec = per / 1000;
        tv.tv_usec = (per % 1000) * 1000;
        int sr = select(fd + 1, NULL, &wset, NULL, &tv);
        if (sr <= 0) { err = 10060; CLOSEFD(fd); fd = -1; continue; }
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &slen) < 0 || soerr != 0) {
            err = soerr;
            CLOSEFD(fd);
            fd = -1;
            continue;
        }
        err = 0;
        break;
    }
    freeaddrinfo(res);
    if (fd < 0) { set_err("no usable address"); return NULL; }
    if (err) {
        set_err(sock_strerror(err));
        CLOSEFD(fd);
        return NULL;
    }
    set_nonblock(fd, 0);
    {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    }
    ca_socket *s = malloc(sizeof(ca_socket));
    if (!s) { CLOSEFD(fd); return NULL; }
    s->fd = fd;
    g_err[0] = '\0';
    return s;
}

int ca_sock_send(ca_socket *s, const void *data, size_t len) {
    size_t off = 0;
    const char *p = (const char *)data;
    while (off < len) {
        int n = (int)send(s->fd, p + off, (int)(len - off), 0);
        if (n <= 0) {
#if !defined(_WIN32)
            if (errno == EINTR) continue;
#endif
            return (int)off;
        }
        off += (size_t)n;
    }
    return (int)off;
}

int ca_sock_recv(ca_socket *s, void *buf, size_t cap) {
    int n = (int)recv(s->fd, buf, (int)cap, 0);
    if (n == 0) return 0; /* EOF */
    if (n < 0) {
#if !defined(_WIN32)
        if (errno == EINTR) return -1;
#endif
        return -1;
    }
    return n;
}

int ca_sock_recv_until(ca_socket *s, char *buf, size_t cap, const char *delim) {
    size_t n = 0;
    char ch;
    while (n + 1 < cap) {
        int r = ca_sock_recv(s, &ch, 1);
        if (r <= 0) return (int)n;
        buf[n++] = ch;
        if (strchr(delim, ch)) break;
    }
    buf[n] = '\0';
    return (int)n;
}

int ca_sock_set_blocking(ca_socket *s, int blocking) {
    if (!s) return -1;
    set_nonblock(s->fd, blocking ? 0 : 1);
    return 0;
}

int ca_sock_wait_readable(ca_socket *s, int timeout_ms) {
    if (!s || s->fd < 0) return -1;
    fd_set rset;
    FD_ZERO(&rset);
#if defined(_WIN32)
    FD_SET((SOCKET)s->fd, &rset);
#else
    FD_SET(s->fd, &rset);
#endif
    struct timeval tv;
    if (timeout_ms < 0) timeout_ms = 0;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(s->fd + 1, &rset, NULL, NULL, &tv);
    if (r < 0) return -1;
    if (r == 0) return 0;
#if defined(_WIN32)
    if (FD_ISSET((SOCKET)s->fd, &rset)) return 1;
#else
    if (FD_ISSET(s->fd, &rset)) return 1;
#endif
    return 0;
}

ca_listener *ca_listen_addr(const char *host, uint16_t port) {
#if defined(_WIN32)
    if (wsa_start() != 0) return NULL;
#endif
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { set_err("socket() failed"); return NULL; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (host && *host) {
        /* bind only the given address (default 127.0.0.1): keeps the console
         * off the LAN and avoids the Windows Firewall authorization prompt on
         * first run */
        addr.sin_addr.s_addr = inet_addr(host);
        if (addr.sin_addr.s_addr == INADDR_NONE) addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        set_err("bind failed");
        CLOSEFD(fd);
        return NULL;
    }
    if (listen(fd, 16) < 0) {
        set_err("listen failed");
        CLOSEFD(fd);
        return NULL;
    }
    ca_listener *l = malloc(sizeof(ca_listener));
    if (!l) { CLOSEFD(fd); return NULL; }
    l->fd = fd;
    g_err[0] = '\0';
    return l;
}

ca_listener *ca_listen(uint16_t port) {
    return ca_listen_addr(NULL, port);
}

ca_socket *ca_accept(ca_listener *l, int timeout_ms) {
    if (l->fd < 0) return NULL;
    /* Wait for an inbound connection with a real timeout. SO_RCVTIMEO does
     * NOT unblock accept() on Windows/Winsock (it only affects recv), so the
     * old code could hang a serving thread forever and ignore stop requests.
     * select() before accept() gives a portable timeout: ca_http_server_stop()
     * sets stop_flag and the serve loop wakes within timeout_ms. */
    if (timeout_ms > 0) {
        fd_set rset;
        FD_ZERO(&rset);
#if defined(_WIN32)
        FD_SET((SOCKET)l->fd, &rset);
#else
        FD_SET(l->fd, &rset);
#endif
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int r = select(l->fd + 1, &rset, NULL, NULL, &tv);
        if (r <= 0) return NULL; /* timeout or error */
    }
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd = (int)accept(l->fd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) {
        set_err("accept failed");
        return NULL;
    }
    ca_socket *s = malloc(sizeof(ca_socket));
    if (!s) { CLOSEFD(fd); return NULL; }
    s->fd = fd;
    return s;
}

void ca_sock_close(ca_socket *s) {
    if (!s) return;
    CLOSEFD(s->fd);
    free(s);
}

void ca_listener_close(ca_listener *l) {
    if (!l) return;
    CLOSEFD(l->fd);
    free(l);
}
