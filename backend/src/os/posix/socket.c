#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "os/os_socket.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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
#include <signal.h>

struct sock {
    int fd;
};
struct listener {
    int fd;
};

#define CLOSEFD(fd) close((fd))

static char g_err[256] = "";

static const char *sock_strerror(int err) {
    return strerror(err);
}

static void set_err(const char *msg) {
    snprintf(g_err, sizeof(g_err), "%s", msg);
}

int sock_init(void) {
    /* MSG_NOSIGNAL is unavailable on macOS and SO_NOSIGPIPE only protects
     * sockets created by this module.  A process-wide ignore is the final
     * safety net for a peer closing an HTTP/SSE connection while a worker is
     * writing its response; callers receive EPIPE instead of the test or
     * server process being terminated by SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void sock_cleanup(void) {
}

const char *sock_error(void) {
    return g_err;
}

static void set_nonblock(int fd, int nb) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

static void suppress_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#else
    (void)fd;
#endif
}

sock *sock_connect(const char *host, uint16_t port, int timeout_ms) {
    char portstr[16];
    struct addrinfo *res = NULL;
    int fd = -1;
    int err = 0;
    int naddrs = 0;
    struct addrinfo *cnt;
    struct addrinfo *ai;
    sock *s;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        set_err("getaddrinfo failed");
        return NULL;
    }

    /* count candidate addresses so we can split the connect budget: a single
     * unreachable address (e.g. an IPv6 ::1 attempt when the server only listens
     * on IPv4) must not be able to consume the whole timeout before we fall back. */
    for (cnt = res; cnt; cnt = cnt->ai_next)
        naddrs++;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        suppress_sigpipe(fd);
        set_nonblock(fd, 1);
        int r = connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen);
        if (r == 0) {
            err = 0;
            break;
        }
        if (errno != EINPROGRESS) {
            err = errno;
            CLOSEFD(fd);
            fd = -1;
            continue;
        }
        /* wait for writability; budget per address = timeout split across all,
         * bounded to [2s, 5s] so a dead address is skipped quickly */
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        int per = naddrs > 1 && timeout_ms > 0 ? timeout_ms / naddrs : timeout_ms;
        if (per > 5000)
            per = 5000;
        /* Respect an explicit small budget: never RAISE a caller's timeout
         * above what they asked for. The 2s floor is only a default for
         * callers that didn't specify one (health probes pass, e.g., 300ms
         * and must fail fast rather than hang the single-threaded server). */
        if (timeout_ms <= 0)
            per = 2000;
        struct timeval tv;
        tv.tv_sec = per / 1000;
        tv.tv_usec = (per % 1000) * 1000;
        int sr = select(fd + 1, NULL, &wset, NULL, &tv);
        if (sr <= 0) {
            err = 10060;
            CLOSEFD(fd);
            fd = -1;
            continue;
        }
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
    if (fd < 0) {
        set_err("no usable address");
        return NULL;
    }

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

    s = malloc(sizeof(sock));
    if (!s) {
        CLOSEFD(fd);
        return NULL;
    }

    s->fd = fd;
    g_err[0] = '\0';
    return s;
}

int sock_send(sock *s, const void *data, size_t len) {
    size_t off = 0;
    const char *p = (const char *)data;
    while (off < len) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        int n = (int)send(s->fd, p + off, (int)(len - off), flags);
        if (n <= 0) {
            if (errno == EINTR)
                continue;
            return (int)off;
        }
        off += (size_t)n;
    }

    return (int)off;
}

int sock_recv(sock *s, void *buf, size_t cap) {
    int n = (int)recv(s->fd, buf, (int)cap, 0);
    if (n == 0)
        return 0; /* EOF */
    if (n < 0) {
        if (errno == EINTR)
            return -1;
        return -1;
    }

    return n;
}

int sock_wait_readable(sock *s, int timeout_ms) {
    int r;

    if (!s || s->fd < 0)
        return -1;
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(s->fd, &rset);
    struct timeval tv;
    if (timeout_ms < 0)
        timeout_ms = 0;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    r = select(s->fd + 1, &rset, NULL, NULL, &tv);
    if (r < 0)
        return -1;
    if (r == 0)
        return 0;
    if (FD_ISSET(s->fd, &rset))
        return 1;
    return 0;
}

listener *listen_addr(const char *host, uint16_t port) {
    int one = 1;
    listener *l;

    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_err("socket() failed");
        return NULL;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (host && *host) {
        /* bind only the given address (default 127.0.0.1): keeps the console
         * off the LAN and avoids firewall prompts on first run */
        addr.sin_addr.s_addr = inet_addr(host);
        if (addr.sin_addr.s_addr == INADDR_NONE)
            addr.sin_addr.s_addr = INADDR_ANY;
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

    l = malloc(sizeof(listener));
    if (!l) {
        CLOSEFD(fd);
        return NULL;
    }

    l->fd = fd;
    g_err[0] = '\0';
    return l;
}

sock *sock_accept(listener *l, int timeout_ms) {
    struct sockaddr_in peer;
    int fd;
    sock *s;

    if (l->fd < 0)
        return NULL;
    /* Wait for an inbound connection with a real timeout. select() before
     * accept() gives a portable timeout: http_server_stop() sets stop_flag
     * and the serve loop wakes within timeout_ms. */
    if (timeout_ms > 0) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(l->fd, &rset);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int r = select(l->fd + 1, &rset, NULL, NULL, &tv);
        if (r <= 0)
            return NULL; /* timeout or error */
    }

    socklen_t plen = sizeof(peer);
    fd = (int)accept(l->fd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) {
        set_err("accept failed");
        return NULL;
    }
    suppress_sigpipe(fd);

    s = malloc(sizeof(sock));
    if (!s) {
        CLOSEFD(fd);
        return NULL;
    }

    s->fd = fd;
    /* SO_RCVTIMEO so a half-open connection (connected but never sends a
     * complete request) cannot wedge the single-threaded HTTP server: recv()
     * times out and the connection is dropped instead of blocking the accept
     * loop forever. */
    struct timeval rto = {30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rto, sizeof(rto));
    return s;
}

void sock_close(sock *s) {
    if (!s)
        return;
    CLOSEFD(s->fd);
    free(s);
}

void listener_close(listener *l) {
    if (!l)
        return;
    CLOSEFD(l->fd);
    free(l);
}
