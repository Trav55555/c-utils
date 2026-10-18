/* getaddrinfo, AI_NUMERICSERV and clock_gettime need POSIX 2008; as in
 * cu_append.c, dcheck.conf supplies _DEFAULT_SOURCE for dcheck's builds. */
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

/* Every socket here failed to open, so usable() always refuses it. */
int cu_socket_accept(CuSocket *listener, CuSocket *out)
{
    if (out == NULL)
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

int cu_socket_write(CuSocket *s, CuStr bytes)
{
    (void)bytes;
    return usable(s);
}

int cu_socket_close(CuSocket *s)
{
    if (s == NULL)
        abort();
    if (s->state != 0)
        s->state = s->state < 0 ? -2 : 0;
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
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
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

    if (out == NULL)
        abort();
    if (!usable(listener))
        return 0;
    if (!listener->listen)
        abort();
    do
        fd = accept(listener->fd, NULL, NULL);
    while (fd < 0 && errno == EINTR);
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
    struct timeval tv;

    if (!usable(s))
        return 0;
    tv.tv_sec = (time_t)(ms / 1000);
    tv.tv_usec = (long)(ms % 1000) * 1000;
    if (setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0 ||
        setsockopt(s->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0)
        return fail(s);
    return 1;
}

/* A blocking call that gave up at the time limit reports EAGAIN. */
static int fail_io(CuSocket *s)
{
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        errno = ETIMEDOUT;
    return fail(s);
}

int cu_socket_read(CuSocket *s, char *buf, size_t cap, size_t *got)
{
    ssize_t r;

    if (buf == NULL || cap == 0 || got == NULL)
        abort();
    if (!usable(s))
        return 0;
    if (s->listen)
        abort();
    do
        r = recv(s->fd, buf, cap < MAX_CHUNK ? cap : MAX_CHUNK, 0);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        return fail_io(s);
    *got = (size_t)r;
    return 1;
}

int cu_socket_write(CuSocket *s, CuStr bytes)
{
    const char *p;
    size_t left;
    ssize_t r;

    if (!usable(s))
        return 0;
    if (s->listen)
        abort();
    p = bytes.ptr;
    left = bytes.len;
    while (left > 0) {
        r = send(s->fd, p, left < MAX_CHUNK ? left : MAX_CHUNK, SEND_FLAGS);
        if (r < 0) {
            if (errno == EINTR)
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

/* As cu_append_close: state -2 marks a socket closed after a failure, so
 * repeats keep returning 0. */
int cu_socket_close(CuSocket *s)
{
    if (s == NULL || s->watched)
        abort();
    if (s->state == 0)
        return 1;
    if (s->state == -2)
        return 0;
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
    int timed; /* 1: deadline applies */
    struct timespec deadline;
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

static int before(struct timespec a, struct timespec b)
{
    return a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec);
}

/* Milliseconds from t to deadline d, rounded up and clamped to
 * [0, INT_MAX], so a wait never wakes just short of a deadline. */
static int ms_until(struct timespec t, struct timespec d)
{
    long sec;
    long nsec;

    if (!before(t, d))
        return 0;
    sec = (long)(d.tv_sec - t.tv_sec);
    nsec = d.tv_nsec - t.tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }
    if (sec >= INT_MAX / 1000 - 1)
        return INT_MAX;
    return (int)(sec * 1000 + (nsec + 999999L) / 1000000L);
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
    if (pipe(px->wake) != 0) {
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
    w->timed = idle_ms != 0;
    if (w->timed) {
        now(&w->deadline);
        w->deadline.tv_sec += (time_t)(idle_ms / 1000);
        w->deadline.tv_nsec += (long)(idle_ms % 1000) * 1000000L;
        if (w->deadline.tv_nsec >= 1000000000L) {
            w->deadline.tv_sec++;
            w->deadline.tv_nsec -= 1000000000L;
        }
    }
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
    r = write(p->wake[1], "w", 1);
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
        if (p->watches[i].timed) {
            r = ms_until(t, p->watches[i].deadline);
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
        if ((ready || (p->watches[i].timed && !before(t, p->watches[i].deadline))) && got < cap) {
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
