#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "death.h"
#include "cu.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef CU_DEATH_TESTS
/* No fixture setup or assertions can masquerade as the required abort. */
static void null_socket(void)
{
    int ok;
    ok = cu_socket_connect_ip_for(NULL, "127.0.0.1", 1, 1000);
    (void)ok;
}

static void null_ip(void)
{
    CuSocket s;
    int ok;
    ok = cu_socket_connect_ip_for(&s, NULL, 1, 1000);
    (void)ok;
}
#endif

#if (defined(__unix__) || defined(__APPLE__)) && !defined(__cerb__) && !defined(__wasi__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

/* Independent kernel fixture: never use the implementation's listen/parser. */
static int bound4(struct sockaddr_in *addr)
{
    static const struct sockaddr_in empty;
    int fd;
    socklen_t len;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    *addr = empty;
    addr->sin_family = AF_INET;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    addr->sin_len = (unsigned char)sizeof *addr;
#endif
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr *)addr, sizeof *addr) == 0);
    len = sizeof *addr;
    assert(getsockname(fd, (struct sockaddr *)addr, &len) == 0);
    assert(len == sizeof *addr && addr->sin_port != 0);
    return fd;
}

static void ready(int fd, short events)
{
    struct pollfd p;
    int rc;

    p.fd = fd;
    p.events = events;
    p.revents = 0;
    do {
        rc = poll(&p, 1, 5000);
    } while (rc < 0 && errno == EINTR);
    assert(rc == 1 && (p.revents & events));
}

static void failed(CuSocket *s, int error)
{
    char byte;
    size_t n;

    assert(s->fd == -1 && cu_socket_errno(s) == error);
    errno = 0;
    assert(!cu_socket_read(s, &byte, 1, &n) && errno == error);
    errno = 0;
    assert(!cu_socket_write(s, CU_LIT("x")) && errno == error);
    errno = 0;
    assert(!cu_socket_close(s) && errno == error);
    errno = 0;
    assert(!cu_socket_close(s) && errno == error);
}

static void exchange(int listener, const char *ip, unsigned port,
                     unsigned long budget)
{
    static const char outgoing[] = {0, 1, (char)255, 'x', 0, '\r', '\n'};
    static const char incoming[] = {(char)128, 0, 'z', (char)254, 7};
    CuSocket c;
    int peer;
    int flags;
    char buf[16];
    char borrowed[46];
    size_t have;
    size_t n;
    ssize_t got;

    strcpy(borrowed, ip);
    assert(cu_socket_connect_ip_for(&c, borrowed, port, budget));
    memset(borrowed, '!', sizeof borrowed);
    assert(c.fd >= 0 && c.state == 1 && !c.listen && !c.watched);
    assert(c.timeout_ms == 0 && cu_socket_errno(&c) == 0);
    flags = fcntl(c.fd, F_GETFL);
    assert(flags >= 0 && !(flags & O_NONBLOCK));
    flags = fcntl(c.fd, F_GETFD);
    assert(flags >= 0 && (flags & FD_CLOEXEC));
    ready(listener, POLLIN);
    peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
    assert(cu_socket_write(&c, cu_str_n(outgoing, sizeof outgoing)));
    have = 0;
    while (have < sizeof outgoing) {
        ready(peer, POLLIN);
        got = recv(peer, buf + have, sizeof outgoing - have, 0);
        assert(got > 0);
        have += (size_t)got;
    }
    assert(memcmp(buf, outgoing, sizeof outgoing) == 0);
    assert(send(peer, incoming, sizeof incoming, 0) == (ssize_t)sizeof incoming);
    /* Explicitly disabling later I/O deadlines remains independent. */
    assert(cu_socket_set_timeout(&c, 0));
    have = 0;
    while (have < sizeof incoming) {
        assert(cu_socket_read(&c, buf + have, sizeof incoming - have, &n));
        assert(n > 0);
        have += n;
    }
    assert(memcmp(buf, incoming, sizeof incoming) == 0);
    assert(c.timeout_ms == 0);
    /* A later caller-selected budget really applies, without timing bounds. */
    assert(cu_socket_set_timeout(&c, 30));
    assert(!cu_socket_read(&c, buf, sizeof buf, &n));
    assert(cu_socket_errno(&c) == ETIMEDOUT);
    assert(!cu_socket_write(&c, CU_LIT("late")) && errno == ETIMEDOUT);
    assert(!cu_socket_close(&c));
    assert(close(peer) == 0);
}

static void ipv4(void)
{
    struct sockaddr_in addr;
    int fd;

    fd = bound4(&addr);
    assert(listen(fd, 8) == 0);
    exchange(fd, "127.0.0.1", (unsigned)ntohs(addr.sin_port), 5000);
    exchange(fd, "127.0.0.1", (unsigned)ntohs(addr.sin_port), 0);
    exchange(fd, "127.0.0.1", (unsigned)ntohs(addr.sin_port), ULONG_MAX);
    assert(close(fd) == 0);
}

static int no_ipv6(int error)
{
    return error == EAFNOSUPPORT || error == EPROTONOSUPPORT ||
           error == EADDRNOTAVAIL || error == ENETUNREACH;
}

static void ipv6(void)
{
    static const struct sockaddr_in6 empty;
    struct sockaddr_in6 addr;
    socklen_t len;
    int fd;
    int error;

    fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        error = errno;
        assert(no_ipv6(error));
        fprintf(stderr, "SKIP socket_connect IPv6 ::1: socket errno=%d\n", error);
        return;
    }
    addr = empty;
    addr.sin6_family = AF_INET6;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    addr.sin6_len = (unsigned char)sizeof addr;
#endif
    addr.sin6_addr.s6_addr[15] = 1;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        error = errno;
        assert(close(fd) == 0);
        assert(no_ipv6(error));
        fprintf(stderr, "SKIP socket_connect IPv6 ::1: bind errno=%d\n", error);
        return;
    }
    assert(listen(fd, 8) == 0);
    len = sizeof addr;
    assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
    assert(len == sizeof addr && addr.sin6_port != 0);
    /* Once listening works, any connect failure is a failure, not a skip. */
    exchange(fd, "::1", (unsigned)ntohs(addr.sin6_port), 5000);
    assert(close(fd) == 0);
}

static void invalid(void)
{
    static const char *const bad[] = {
        "localhost", "", "[::1]", "::1%lo", "fe80::1%1", "127.0.0.256",
        "127.1", "1.2.3.4.5", ":::1", " 127.0.0.1", "127.0.0.1 ",
        "2001:db8::g", "1:2:3:4:5:6:7:8:9"
    };
    CuSocket s;
    struct sockaddr_in addr;
    char long_ip[128];
    size_t i;
    size_t length;
    int anchor;
    int before;
    int after;

    anchor = bound4(&addr);
    before = dup(anchor);
    assert(before >= 0 && close(before) == 0);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        errno = 0;
        assert(!cu_socket_connect_ip_for(&s, bad[i], 80, 5000));
        assert(errno == EINVAL);
        failed(&s, EINVAL);
    }
    /* Exactly 45 characters plus NUL is within the inspection boundary;
     * 46 and longer must also fail without looking beyond the string. */
    for (length = 45; length < sizeof long_ip; length++) {
        memset(long_ip, '1', length);
        long_ip[length] = '\0';
        assert(!cu_socket_connect_ip_for(&s, long_ip, 80, 0));
        assert(errno == EINVAL);
        failed(&s, EINVAL);
    }
    assert(!cu_socket_connect_ip_for(&s, "127.0.0.1", 65536U, 5000));
    assert(errno == EINVAL);
    failed(&s, EINVAL);
    after = dup(anchor);
    assert(after == before && close(after) == 0);
    assert(close(anchor) == 0);
}

static void refusal(void)
{
    struct sockaddr_in addr;
    CuSocket s;
    int reservation;
    int probe;
    int error;
    int before;
    int after;
    int i;

    /* Keep a bound, non-listening socket open throughout both oracles.
     * No SO_REUSEADDR: another process cannot steal this endpoint. */
    reservation = bound4(&addr);
    probe = socket(AF_INET, SOCK_STREAM, 0);
    assert(probe >= 0);
    assert(connect(probe, (struct sockaddr *)&addr, sizeof addr) == -1);
    error = errno;
    assert(error == ECONNREFUSED);
    assert(close(probe) == 0);
    before = dup(reservation);
    assert(before >= 0 && close(before) == 0);
    for (i = 0; i < 32; i++) {
        errno = 0;
        assert(!cu_socket_connect_ip_for(&s, "127.0.0.1",
                                        (unsigned)ntohs(addr.sin_port), 5000));
        assert(errno == error);
        failed(&s, error);
    }
    after = dup(reservation);
    assert(after == before && close(after) == 0);
    assert(close(reservation) == 0);
}
#endif

int main(void)
{
#if (defined(__unix__) || defined(__APPLE__)) && !defined(__cerb__) && !defined(__wasi__)
    /* Tests-first: the ENOSYS stub must fail the first real connection. */
    ipv4();
    ipv6();
    invalid();
    refusal();
#else
    CuSocket s;
    int expected;

#ifdef ENOSYS
    expected = ENOSYS;
#else
    expected = EINVAL;
#endif
    assert(!cu_socket_connect_ip_for(&s, "127.0.0.1", 1, 0));
    assert(s.fd == -1 && cu_socket_errno(&s) == expected);
    errno = 0;
    assert(!cu_socket_close(&s) && errno == expected);
    errno = 0;
    assert(!cu_socket_close(&s) && errno == expected);
#endif
#ifdef CU_DEATH_TESTS
    expect_abort(null_socket);
    expect_abort(null_ip);
#endif
    return 0;
}
