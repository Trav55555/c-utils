/* POSIX networking and clocks, plus atomic Linux descriptor creation.
 * dcheck.conf supplies feature defines before dcheck's forced header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "cu.h"

/* wasm32-wasi (preview 1) cannot listen, and Cerberus models no
 * descriptors. */
#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
#define CU_SOCKET_NONE 1
#endif

#ifndef ENOSYS
#define ENOSYS EINVAL
#endif

/* Entry checks shared by every socket operation but close, as in
 * cu_append.c: NULL and use after close abort; a failed socket reports its
 * saved error. */
static int usable(CuSocket *s)
{
    if (s == NULL || s->state == 0 || s->state == -2)
        abort();
    if (s->state < 0) {
        errno = s->error;
        return 0;
    }
    return 1;
}

/* Records the current errno as s's sticky error. */
static int fail(CuSocket *s)
{
    s->state = -1;
    s->error = errno;
    return 0;
}

/* Starts s as failed and empty, so close is required and safe on every
 * path out of an opening call. */
static void begin(CuSocket *s, const char *host, int listening)
{
    if (s == NULL || host == NULL)
        abort();
    s->fd = -1;
    s->state = -1;
    s->error = 0;
    s->listen = listening;
    s->watched = 0;
    s->timeout_ms = 0;
}

int cu_socket_errno(const CuSocket *s)
{
    if (s == NULL)
        abort();
    return s->error;
}

#ifdef CU_SOCKET_NONE

struct CuPoller {
    int unused;
};

static int no_sockets(CuSocket *s)
{
    errno = ENOSYS;
    return fail(s);
}

int cu_socket_listen(CuSocket *s, const char *host, unsigned port, int backlog)
{
    begin(s, host, 1);
    (void)port;
    (void)backlog;
    return no_sockets(s);
}

int cu_socket_connect(CuSocket *s, const char *host, unsigned port)
{
    begin(s, host, 0);
    (void)port;
    return no_sockets(s);
}

int cu_socket_connect_ip_for(CuSocket *s, const char *ip, unsigned port,
                              unsigned long timeout_ms)
{
    begin(s, ip, 0);
    (void)port;
    (void)timeout_ms;
    return no_sockets(s);
}

/* Every socket here failed to open, so usable() always refuses it. */
int cu_socket_accept(CuSocket *listener, CuSocket *out)
{
    if (out == NULL || out == listener)
        abort();
    return usable(listener);
}

unsigned cu_socket_port(const CuSocket *s)
{
    if (s == NULL || s->state == 0 || s->state == -2)
        abort();
    return 0;
}

int cu_socket_set_timeout(CuSocket *s, unsigned long ms)
{
    (void)ms;
    return usable(s);
}

int cu_socket_read(CuSocket *s, char *buf, size_t cap, size_t *got)
{
    if (buf == NULL || cap == 0 || got == NULL)
        abort();
    return usable(s);
}

int cu_socket_read_for(CuSocket *s, char *buf, size_t cap, size_t *got,
                       unsigned long timeout_ms)
{
    (void)timeout_ms;
    return cu_socket_read(s, buf, cap, got);
}

int cu_socket_write(CuSocket *s, CuStr bytes)
{
    (void)bytes;
    return usable(s);
}

int cu_socket_shutdown_write(CuSocket *s)
{
    return usable(s);
}

int cu_socket_close(CuSocket *s)
{
    if (s == NULL)
        abort();
    if (s->state != 0)
        s->state = s->state < 0 ? -2 : 0;
    if (s->state < 0)
        errno = s->error;
    return s->state == 0;
}

int cu_poller_init(CuPoller **p, CuArena *a, size_t capacity)
{
    if (p == NULL || a == NULL || capacity == 0)
        abort();
    errno = ENOSYS;
    return 0;
}

/* No poller can exist here, so every other call is misuse. */
int cu_poller_add(CuPoller *p, CuSocket *s, void *tag, unsigned long idle_ms)
{
    (void)p;
    (void)s;
    (void)tag;
    (void)idle_ms;
    abort();
    return 0;
}

int cu_poller_wait(CuPoller *p, unsigned long max_ms, CuReady *out, size_t cap, size_t *n)
{
    (void)p;
    (void)max_ms;
    (void)out;
    (void)cap;
    (void)n;
    abort();
    return 0;
}

void cu_poller_wake(CuPoller *p)
{
    (void)p;
    abort();
}

void cu_poller_destroy(CuPoller *p)
{
    (void)p;
    abort();
}

#else

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* The most one recv() or send() call is asked for, so its result fits
 * ssize_t on 32-bit systems. */
#define MAX_CHUNK ((size_t)1 << 30)

/* Linux says per call that a closed peer must not raise SIGPIPE; Apple
 * says it per socket (SO_NOSIGPIPE, in configure). */
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

/* Close-on-exec, and no SIGPIPE where that is a socket option. Returns 1,
 * or 0 with errno set. */
static int configure(int fd)
{
#ifdef SO_NOSIGPIPE
    int one;

    one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0)
        return 0;
#elif !defined(MSG_NOSIGNAL)
    /* Do not expose a socket whose write could terminate the process. */
    (void)fd;
    errno = ENOSYS;
    return 0;
#endif
    return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

/* Sets or clears O_NONBLOCK on fd. */
static int set_nonblocking(int fd, int on)
{
    int flags;

    flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return 0;
    flags = on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) == 0;
}

/* Closes fd, keeping the errno that explains why. */
static void discard(int fd)
{
    int saved;

    saved = errno;
    (void)close(fd);
    errno = saved;
}

/* Resolves host and port into *res (freeaddrinfo it). Returns 1, or 0
 * with errno set: EADDRNOTAVAIL for a host that does not resolve. */
static const struct addrinfo no_hints;

static int resolve(const char *host, unsigned port, int passive, struct addrinfo **res)
{
    struct addrinfo hints;
    char service[24];
    int rc;

    if (port > 65535) {
        errno = EINVAL;
        return 0;
    }
    sprintf(service, "%u", port);
    /* POSIX wants every other member zero or null, and getaddrinfo may
     * read padding too, so start from an object zero throughout. */
    hints = no_hints;
    hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, service, &hints, res);
    if (rc == 0)
        return 1;
    if (rc != EAI_SYSTEM || errno == 0)
        errno = EADDRNOTAVAIL;
    return 0;
}

/* Opens a socket on each address in turn until one binds and listens (or
 * connects). Returns the descriptor, or -1 with errno from the last
 * failure. */
static int first_address(struct addrinfo *res, int listening, int backlog)
{
    struct addrinfo *ai;
    int fd;
    int one;

    errno = EADDRNOTAVAIL;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
#ifdef __linux__
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
#else
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
#endif
        if (fd < 0)
            continue;
        one = 1;
        if (listening) {
            /* Nonblocking, so an accept never waits: a connection that
             * was ready can be gone by then. */
            if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0 &&
                bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, backlog) == 0 &&
                configure(fd) && set_nonblocking(fd, 1))
                return fd;
        } else if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 && configure(fd)) {
            return fd;
        }
        discard(fd);
    }
    return -1;
}

static int open_on(CuSocket *s, const char *host, unsigned port, int backlog)
{
    struct addrinfo *res;
    int fd;

    if (!resolve(host, port, s->listen, &res))
        return fail(s);
    fd = first_address(res, s->listen, backlog);
    freeaddrinfo(res);
    if (fd < 0)
        return fail(s);
    s->fd = fd;
    s->state = 1;
    return 1;
}

int cu_socket_listen(CuSocket *s, const char *host, unsigned port, int backlog)
{
    begin(s, host, 1);
    return open_on(s, host, port, backlog);
}

int cu_socket_connect(CuSocket *s, const char *host, unsigned port)
{
    begin(s, host, 0);
    return open_on(s, host, port, 0);
}

int cu_socket_accept(CuSocket *listener, CuSocket *out)
{
    int fd;

    if (out == NULL || out == listener)
        abort();
    if (!usable(listener))
        return 0;
    if (!listener->listen)
        abort();
    do {
#ifdef __linux__
        fd = accept4(listener->fd, NULL, NULL, SOCK_CLOEXEC);
#else
        fd = accept(listener->fd, NULL, NULL);
#endif
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return 0;
    /* BSD and macOS hand on the listener's O_NONBLOCK; reads and writes
     * here block up to the time limit. */
    if (!configure(fd) || !set_nonblocking(fd, 0)) {
        discard(fd);
        return 0;
    }
    out->fd = fd;
    out->state = 1;
    out->error = 0;
    out->listen = 0;
    out->watched = 0;
    out->timeout_ms = 0;
    return 1;
}

unsigned cu_socket_port(const CuSocket *s)
{
    struct sockaddr_storage addr;
    socklen_t len;

    if (s == NULL || s->state == 0 || s->state == -2)
        abort();
    if (s->state < 0)
        return 0;
    len = sizeof addr;
    if (getsockname(s->fd, (struct sockaddr *)&addr, &len) != 0)
        return 0;
    if (addr.ss_family == AF_INET)
        return ntohs(((struct sockaddr_in *)&addr)->sin_port);
    if (addr.ss_family == AF_INET6)
        return ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
    return 0;
}

int cu_socket_set_timeout(CuSocket *s, unsigned long ms)
{
    if (!usable(s))
        return 0;
    s->timeout_ms = ms;
    return 1;
}

/* A blocking call that gave up at the time limit reports EAGAIN. */
static int fail_io(CuSocket *s)
{
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        errno = ETIMEDOUT;
    return fail(s);
}

/* Remaining milliseconds, rounded up. Never add a duration to time_t. */
static unsigned long remaining_ms(struct timespec start, struct timespec t, unsigned long ms)
{
    unsigned long sec;
    unsigned long used;
    unsigned long left;
    long ns;

    sec = (unsigned long)t.tv_sec - (unsigned long)start.tv_sec;
    ns = t.tv_nsec - start.tv_nsec;
    if (ns < 0) {
        sec--;
        ns += 1000000000L;
    }
    if (sec > ms / 1000)
        return 0;
    left = ms - sec * 1000;
    used = (unsigned long)(ns / 1000000L);
    return used >= left ? 0 : left - used;
}

static int io_wait(CuSocket *s, unsigned long ms, struct timespec start, short events)
{
    struct timespec t;
    unsigned long left;
    struct pollfd fd;
    int r;

    if (!ms)
        return 1;
    for (;;) {
        if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
            return 0;
        left = remaining_ms(start, t, ms);
        if (left == 0)
            break;
        fd.fd = s->fd;
        fd.events = events;
        fd.revents = 0;
        r = poll(&fd, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (r > 0)
            return 1;
        if (r < 0 && errno != EINTR)
            return 0;
    }
    errno = ETIMEDOUT;
    return 0;
}

static int io_start(unsigned long ms, struct timespec *t)
{
    t->tv_sec = 0;
    t->tv_nsec = 0;
    return !ms || clock_gettime(CLOCK_MONOTONIC, t) == 0;
}

/* Numeric connect shares the existing duration arithmetic, not the
 * configured later read/write timeout. Zero still means unlimited. */
static int connect_delay(struct timespec start, unsigned long ms, int *delay)
{
    struct timespec t;
    unsigned long left;

    if (ms == 0) {
        *delay = -1;
        return 1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return 0;
    left = remaining_ms(start, t, ms);
    if (left == 0) {
        errno = ETIMEDOUT;
        return 0;
    }
    *delay = left > (unsigned long)INT_MAX ? INT_MAX : (int)left;
    return 1;
}

static int connect_ready(int socket_fd, struct timespec start, unsigned long ms)
{
    struct pollfd ready;
    socklen_t length;
    int delay;
    int result;
    int socket_error;

    for (;;) {
        if (!connect_delay(start, ms, &delay))
            return 0;
        ready.fd = socket_fd;
        ready.events = POLLOUT;
        ready.revents = 0;
        result = poll(&ready, 1, delay);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (result == 0)
            continue;
        if ((ready.revents & POLLNVAL) != 0) {
            errno = EBADF;
            return 0;
        }
        if ((ready.revents & (POLLOUT | POLLERR | POLLHUP)) == 0)
            continue;
        socket_error = -1;
        length = sizeof socket_error;
        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0)
            return 0;
        if (length != sizeof socket_error || socket_error < 0) {
            errno = EIO;
            return 0;
        }
        if (socket_error == EINPROGRESS || socket_error == EALREADY)
            continue;
        if (socket_error != 0) {
            errno = socket_error;
            return 0;
        }
        if ((ready.revents & POLLOUT) != 0)
            return 1;
        errno = ECONNABORTED;
        return 0;
    }
}

static const struct sockaddr_in empty_ipv4;
static const struct sockaddr_in6 empty_ipv6;

int cu_socket_connect_ip_for(CuSocket *s, const char *ip, unsigned port,
                              unsigned long timeout_ms)
{
    struct sockaddr_in ipv4;
    struct sockaddr_in6 ipv6;
    const struct sockaddr *endpoint;
    struct timespec start;
    socklen_t endpoint_length;
    size_t text_length;
    int family;
    int parsed;
    int socket_fd;
    int result;
    int delay;

    begin(s, ip, 0);
    if (!io_start(timeout_ms, &start))
        return fail(s);
    for (text_length = 0; text_length < INET6_ADDRSTRLEN; text_length++) {
        if (ip[text_length] == '\0')
            break;
    }
    if (port > 65535 || text_length == 0 || text_length == INET6_ADDRSTRLEN) {
        errno = EINVAL;
        return fail(s);
    }
    ipv4 = empty_ipv4;
    parsed = inet_pton(AF_INET, ip, &ipv4.sin_addr);
    if (parsed < 0)
        return fail(s);
    if (parsed == 1) {
        family = AF_INET;
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons((unsigned short)port);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
        ipv4.sin_len = (unsigned char)sizeof ipv4;
#endif
        endpoint = (const struct sockaddr *)&ipv4;
        endpoint_length = sizeof ipv4;
    } else {
        ipv6 = empty_ipv6;
        parsed = inet_pton(AF_INET6, ip, &ipv6.sin6_addr);
        if (parsed != 1) {
            if (parsed == 0)
                errno = EINVAL;
            return fail(s);
        }
        family = AF_INET6;
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons((unsigned short)port);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
        ipv6.sin6_len = (unsigned char)sizeof ipv6;
#endif
        endpoint = (const struct sockaddr *)&ipv6;
        endpoint_length = sizeof ipv6;
    }
    for (;;) {
        if (!connect_delay(start, timeout_ms, &delay))
            return fail(s);
#ifdef __linux__
        socket_fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
        socket_fd = socket(family, SOCK_STREAM, 0);
#endif
        if (socket_fd < 0)
            return fail(s);
        if (!configure(socket_fd)
#ifndef __linux__
            || !set_nonblocking(socket_fd, 1)
#endif
            || !connect_delay(start, timeout_ms, &delay)) {
            discard(socket_fd);
            return fail(s);
        }
        result = connect(socket_fd, endpoint, endpoint_length);
        if (result != 0 && errno == EINTR) {
            /* POSIX leaves an interrupted connect's state unspecified. */
            discard(socket_fd);
            continue;
        }
        if ((result != 0 && errno != EINPROGRESS && errno != EALREADY) ||
            (result != 0 && !connect_ready(socket_fd, start, timeout_ms)) ||
            !connect_delay(start, timeout_ms, &delay) ||
            !set_nonblocking(socket_fd, 0) ||
            !connect_delay(start, timeout_ms, &delay)) {
            discard(socket_fd);
            return fail(s);
        }
        s->fd = socket_fd;
        s->state = 1;
        return 1;
    }
}

int cu_socket_read_for(CuSocket *s, char *buf, size_t cap, size_t *got,
                       unsigned long timeout_ms)
{
    ssize_t r;
    struct timespec start;

    if (buf == NULL || cap == 0 || got == NULL)
        abort();
    if (!usable(s))
        return 0;
    if (s->listen)
        abort();
    if (!io_start(timeout_ms, &start))
        return fail(s);
    for (;;) {
        if (!io_wait(s, timeout_ms, start, POLLIN))
            return fail(s);
        r = recv(s->fd, buf, cap < MAX_CHUNK ? cap : MAX_CHUNK,
                 timeout_ms ? MSG_DONTWAIT : 0);
        if (r >= 0 || (errno != EINTR && !(timeout_ms &&
            (errno == EAGAIN || errno == EWOULDBLOCK))))
            break;
    }
    if (r < 0)
        return fail_io(s);
    *got = (size_t)r;
    return 1;
}

int cu_socket_read(CuSocket *s, char *buf, size_t cap, size_t *got)
{
    if (s == NULL)
        abort();
    return cu_socket_read_for(s, buf, cap, got, s->timeout_ms);
}

int cu_socket_write(CuSocket *s, CuStr bytes)
{
    const char *p;
    size_t left;
    ssize_t r;
    struct timespec start;

    if (!usable(s))
        return 0;
    if (s->listen)
        abort();
    if (!io_start(s->timeout_ms, &start))
        return fail(s);
    p = bytes.ptr;
    left = bytes.len;
    while (left > 0) {
        if (!io_wait(s, s->timeout_ms, start, POLLOUT))
            return fail(s);
        r = send(s->fd, p, left < MAX_CHUNK ? left : MAX_CHUNK,
                 SEND_FLAGS | (s->timeout_ms ? MSG_DONTWAIT : 0));
        if (r < 0) {
            if (errno == EINTR || (s->timeout_ms &&
                (errno == EAGAIN || errno == EWOULDBLOCK)))
                continue;
            return fail_io(s);
        }
        if (r == 0) {
            errno = EIO;
            return fail(s);
        }
        p += r;
        left -= (size_t)r;
    }
    return 1;
}

int cu_socket_shutdown_write(CuSocket *s)
{
    if (!usable(s))
        return 0;
    if (s->listen)
        abort();
    if (shutdown(s->fd, SHUT_WR) != 0)
        return fail(s);
    return 1;
}

/* As cu_append_close: state -2 marks a socket closed after a failure, so
 * repeats keep returning 0. */
int cu_socket_close(CuSocket *s)
{
    if (s == NULL || s->watched)
        abort();
    if (s->state == 0)
        return 1;
    if (s->state == -2) {
        errno = s->error;
        return 0;
    }
    if (s->fd >= 0) {
        if (close(s->fd) != 0 && s->state > 0) {
            s->state = -1;
            s->error = errno;
        }
        s->fd = -1;
    }
    if (s->state < 0) {
        s->state = -2;
        errno = s->error;
        return 0;
    }
    s->state = 0;
    return 1;
}

/* ---- Poller: the poll() backend ---- */

typedef struct Watch {
    CuSocket *socket; /* its watched flag is cleared when reported */
    int fd;
    void *tag;
    unsigned long idle_ms; /* 0: no expiry */
    struct timespec start;
} Watch;

/* Adds go to pending under the lock, because the waiting thread reads
 * watches and fds without it while poll() runs. count, also under the
 * lock, is every watch not yet reported. */
struct CuPoller {
    CuMutex *lock;
    int wake[2]; /* a pipe: wait polls the read end, wake writes the other */
    Watch *watches; /* the waiting thread's own */
    size_t n;
    Watch *pending;
    size_t n_pending;
    size_t count;
    size_t capacity;
    struct pollfd *fds; /* capacity + 1: the wake pipe, then the watches */
    int live;
};

static void now(struct timespec *t)
{
    if (clock_gettime(CLOCK_MONOTONIC, t) != 0)
        abort();
}

static void check(const CuPoller *p)
{
    if (p == NULL || !p->live)
        abort();
}

static int nonblocking(int fd)
{
    return set_nonblocking(fd, 1) && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

int cu_poller_init(CuPoller **p, CuArena *a, size_t capacity)
{
    CuPoller *px;

    if (p == NULL || a == NULL || capacity == 0)
        abort();
    px = capacity == (size_t)-1 ? NULL : CU_PUSH(a, CuPoller, 1);
    if (px == NULL) {
        errno = ENOMEM;
        return 0;
    }
    px->watches = CU_PUSH(a, Watch, capacity);
    px->pending = CU_PUSH(a, Watch, capacity);
    px->fds = CU_PUSH(a, struct pollfd, capacity + 1);
    if (px->watches == NULL || px->pending == NULL || px->fds == NULL) {
        errno = ENOMEM;
        return 0;
    }
    if (!cu_mutex_init(&px->lock, a))
        return 0;
#ifdef __linux__
    if (pipe2(px->wake, O_CLOEXEC | O_NONBLOCK) != 0) {
#else
    if (pipe(px->wake) != 0) {
#endif
        cu_mutex_destroy(px->lock);
        return 0;
    }
    if (!nonblocking(px->wake[0]) || !nonblocking(px->wake[1])) {
        discard(px->wake[0]);
        discard(px->wake[1]);
        cu_mutex_destroy(px->lock);
        return 0;
    }
    px->n = 0;
    px->n_pending = 0;
    px->count = 0;
    px->capacity = capacity;
    px->live = 1;
    *p = px;
    return 1;
}

int cu_poller_add(CuPoller *p, CuSocket *s, void *tag, unsigned long idle_ms)
{
    Watch *w;

    check(p);
    if (!usable(s))
        return 0;
    cu_mutex_lock(p->lock);
    if (s->watched)
        abort();
    if (p->count == p->capacity) {
        cu_mutex_unlock(p->lock);
        errno = ENOSPC;
        return 0;
    }
    s->watched = 1;
    w = &p->pending[p->n_pending++];
    w->socket = s;
    w->fd = s->fd;
    w->tag = tag;
    w->idle_ms = idle_ms;
    now(&w->start);
    p->count++;
    cu_mutex_unlock(p->lock);
    cu_poller_wake(p);
    return 1;
}

void cu_poller_wake(CuPoller *p)
{
    ssize_t r;

    check(p);
    /* A full pipe already holds a wake, so a failed write loses nothing. */
    do
        r = write(p->wake[1], "w", 1);
    while (r < 0 && errno == EINTR);
    (void)r;
}

/* Empties the wake pipe, which is nonblocking. */
static void drain(int fd)
{
    char buf[64];

    while (read(fd, buf, sizeof buf) > 0)
        continue;
}

int cu_poller_wait(CuPoller *p, unsigned long max_ms, CuReady *out, size_t cap, size_t *n)
{
    struct timespec t;
    size_t i;
    size_t kept;
    size_t got;
    int timeout;
    int r;
    int ready;
    unsigned long left;

    check(p);
    if (n == NULL || (out == NULL && cap != 0))
        abort();
    *n = 0;
    cu_mutex_lock(p->lock);
    for (i = 0; i < p->n_pending; i++)
        p->watches[p->n++] = p->pending[i];
    p->n_pending = 0;
    cu_mutex_unlock(p->lock);

    now(&t);
    if (max_ms == 0)
        timeout = -1;
    else
        timeout = max_ms > INT_MAX ? INT_MAX : (int)max_ms;
    p->fds[0].fd = p->wake[0];
    p->fds[0].events = POLLIN;
    p->fds[0].revents = 0;
    for (i = 0; i < p->n; i++) {
        p->fds[i + 1].fd = p->watches[i].fd;
        p->fds[i + 1].events = POLLIN;
        p->fds[i + 1].revents = 0;
        if (p->watches[i].idle_ms != 0) {
            left = remaining_ms(p->watches[i].start, t, p->watches[i].idle_ms);
            r = left > INT_MAX ? INT_MAX : (int)left;
            if (timeout < 0 || r < timeout)
                timeout = r;
        }
    }
    r = poll(p->fds, (nfds_t)(p->n + 1), timeout);
    if (r < 0)
        return errno == EINTR;
    if (p->fds[0].revents != 0)
        drain(p->wake[0]);

    /* Report readable and expired watches, as many as fit; keep the rest
     * in order. POLLHUP, POLLERR and POLLNVAL count as readable: the
     * caller's read sees the cause. Under the lock, because adds read the
     * watched flags this clears. */
    now(&t);
    kept = 0;
    got = 0;
    cu_mutex_lock(p->lock);
    for (i = 0; i < p->n; i++) {
        ready = p->fds[i + 1].revents != 0;
        if ((ready || (p->watches[i].idle_ms != 0 &&
            remaining_ms(p->watches[i].start, t, p->watches[i].idle_ms) == 0)) && got < cap) {
            p->watches[i].socket->watched = 0;
            out[got].tag = p->watches[i].tag;
            out[got].expired = !ready;
            got++;
        } else {
            p->watches[kept++] = p->watches[i];
        }
    }
    p->n = kept;
    p->count -= got;
    cu_mutex_unlock(p->lock);
    *n = got;
    return 1;
}

void cu_poller_destroy(CuPoller *p)
{
    size_t i;

    check(p);
    cu_mutex_lock(p->lock);
    for (i = 0; i < p->n; i++)
        p->watches[i].socket->watched = 0;
    for (i = 0; i < p->n_pending; i++)
        p->pending[i].socket->watched = 0;
    cu_mutex_unlock(p->lock);
    (void)close(p->wake[0]);
    (void)close(p->wake[1]);
    cu_mutex_destroy(p->lock);
    p->live = 0;
}

#endif
