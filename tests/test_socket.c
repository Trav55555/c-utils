#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "death.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

#if defined(__cerb__) || defined(__wasi__)
/* No sockets here: opening reports ENOSYS and leaves a socket to close. */
int main(void)
{
    CuArena a;
    CuPoller *p;
    CuSocket s;

    assert(!cu_socket_listen(&s, "127.0.0.1", 0, 1));
    assert(cu_socket_errno(&s) == ENOSYS);
    assert(!cu_socket_close(&s));
    assert(!cu_socket_connect(&s, "127.0.0.1", 1));
    assert(cu_socket_errno(&s) == ENOSYS);
    assert(!cu_socket_close(&s));
    cu_arena_init(&a);
    errno = 0;
    assert(!cu_poller_init(&p, &a, 4) && errno == ENOSYS);
    cu_arena_release(&a);
    return 0;
}
#else

#include <signal.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <limits.h>
#include <time.h>

static volatile sig_atomic_t ticks;

static void tick(int sig)
{
    struct itimerval stop;

    (void)sig;
    ticks++;
    if (ticks == 30) {
        stop.it_value.tv_sec = 0;
        stop.it_value.tv_usec = 0;
        stop.it_interval = stop.it_value;
        (void)setitimer(ITIMER_REAL, &stop, NULL);
    }
}

/* A listener on a free loopback port, a client connected to it and the
 * server's side of that connection. */
typedef struct Pair {
    CuSocket listener;
    CuSocket client;
    CuSocket server;
} Pair;

/* Accepts a connection that connect has made: accept does not wait, so
 * retry while the system has not queued it yet. */
static void accept_one(CuSocket *listener, CuSocket *out)
{
    while (!cu_socket_accept(listener, out))
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void pair_open(Pair *p)
{
    unsigned port;

    assert(cu_socket_listen(&p->listener, "127.0.0.1", 0, 16));
    port = cu_socket_port(&p->listener);
    assert(port != 0);
    assert(cu_socket_connect(&p->client, "127.0.0.1", port));
    accept_one(&p->listener, &p->server);
}

static void pair_close(Pair *p)
{
    int ok;

    ok = cu_socket_close(&p->client);
    (void)ok;
    ok = cu_socket_close(&p->server);
    (void)ok;
    assert(cu_socket_close(&p->listener));
}

/* Reads exactly n bytes into buf. */
static void read_exactly(CuSocket *s, char *buf, size_t n)
{
    size_t have;
    size_t got;

    for (have = 0; have < n; have += got) {
        assert(cu_socket_read(s, buf + have, n - have, &got));
        assert(got > 0);
    }
}

/* Bytes go both ways, the port is the one bound, a peer's close reads as
 * 0 bytes, and clean closes return 1, twice. */
static void test_round_trip(void)
{
    Pair p;
    char buf[16];
    size_t got;

    pair_open(&p);
    assert(p.client.timeout_ms == 0 && p.server.timeout_ms == 0);
    assert(cu_socket_set_timeout(&p.server, ULONG_MAX));
    assert(cu_socket_write(&p.client, cu_str_cstr("hello")));
    read_exactly(&p.server, buf, 5);
    assert(memcmp(buf, "hello", 5) == 0);
    assert(cu_socket_write(&p.server, cu_str_cstr("back")));
    read_exactly(&p.client, buf, 4);
    assert(memcmp(buf, "back", 4) == 0);
    assert(cu_socket_write(&p.client, cu_str_n("", 0)));
    assert(cu_socket_close(&p.client));
    assert(cu_socket_close(&p.client));
    assert(cu_socket_read(&p.server, buf, sizeof buf, &got) && got == 0);
    assert(cu_socket_close(&p.server));
    assert(cu_socket_close(&p.listener));
}

#define BIG ((size_t)4 << 20)

typedef struct Sender {
    CuSocket *s;
    char *bytes;
    int ok;
} Sender;

static void send_all(void *context)
{
    Sender *w;

    w = (Sender *)context;
    w->ok = cu_socket_write(w->s, cu_str_n(w->bytes, BIG));
}

/* 4 MiB, far over any socket buffer, arrive whole and in order through
 * many partial reads, while another thread writes. */
static void test_large_transfer(void)
{
    CuArena a;
    CuThread *t;
    Pair p;
    Sender w;
    char *got;
    size_t i;

    cu_arena_init(&a);
    pair_open(&p);
    w.s = &p.client;
    w.bytes = CU_PUSH(&a, char, BIG);
    got = CU_PUSH(&a, char, BIG);
    assert(w.bytes != NULL && got != NULL);
    for (i = 0; i < BIG; i++)
        w.bytes[i] = (char)(i * 7 % 251);
    w.ok = 0;
    assert(cu_thread_start(&t, &a, 0, send_all, &w));
    read_exactly(&p.server, got, BIG);
    cu_thread_join(t);
    assert(w.ok);
    assert(memcmp(got, w.bytes, BIG) == 0);
    pair_close(&p);
    cu_arena_release(&a);
}

/* A read that waits past the limit fails with ETIMEDOUT, and the error
 * sticks: writes fail with it and close reports it. */
static void test_timeout(void)
{
    Pair p;
    char buf[4];
    size_t got;

    pair_open(&p);
    assert(cu_socket_set_timeout(&p.server, 50));
    errno = 0;
    assert(!cu_socket_read(&p.server, buf, sizeof buf, &got));
    assert(errno == ETIMEDOUT && cu_socket_errno(&p.server) == ETIMEDOUT);
    errno = 0;
    assert(!cu_socket_write(&p.server, cu_str_cstr("x")) && errno == ETIMEDOUT);
    assert(!cu_socket_close(&p.server));
    assert(!cu_socket_close(&p.server) && errno == ETIMEDOUT);
    assert(cu_socket_set_timeout(&p.client, 0));
    assert(cu_socket_close(&p.client));
    assert(cu_socket_close(&p.listener));
}

/* Repeated signals must not restart a public call's budget. */
static void test_signal_timeout(int writing)
{
    Pair p;
    struct sigaction sa;
    struct sigaction old;
    struct itimerval timer;
    struct timespec start;
    struct timespec end;
    char buf[1];
    size_t got;
    double elapsed;
    char *bytes;
    int small;

    bytes = (char *)malloc(BIG);
    assert(bytes != NULL);
    memset(bytes, 'x', BIG);
    pair_open(&p);
    small = 4096;
    assert(setsockopt(p.server.fd, SOL_SOCKET, SO_SNDBUF,
                     &small, sizeof small) == 0);
    sa.sa_handler = tick;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    assert(sigaction(SIGALRM, &sa, &old) == 0);
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 10000;
    timer.it_interval = timer.it_value;
    ticks = 0;
    assert(cu_socket_set_timeout(&p.server, 60));
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    if (writing)
        assert(!cu_socket_write(&p.server, cu_str_n(bytes, BIG)));
    else
        assert(!cu_socket_read(&p.server, buf, sizeof buf, &got));
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 0;
    timer.it_interval = timer.it_value;
    assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    assert(sigaction(SIGALRM, &old, NULL) == 0);
    elapsed = (double)(end.tv_sec - start.tv_sec) +
        (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
    assert(ticks >= 2);
    assert(elapsed >= 0.05 && elapsed < 0.25);
    free(bytes);
    assert(cu_socket_errno(&p.server) == ETIMEDOUT);
    pair_close(&p);
}

/* A peer that stops reading makes a write wait; past the limit it fails
 * with ETIMEDOUT. */
static void test_write_timeout(void)
{
    Pair p;
    char chunk[65536];
    size_t i;
    int ok;

    for (i = 0; i < sizeof chunk; i++)
        chunk[i] = (char)i;
    pair_open(&p);
    assert(cu_socket_set_timeout(&p.server, 50));
    ok = 1;
    for (i = 0; i < 4096 && ok; i++)
        ok = cu_socket_write(&p.server, cu_str_n(chunk, sizeof chunk));
    assert(!ok && cu_socket_errno(&p.server) == ETIMEDOUT);
    assert(!cu_socket_close(&p.server));
    assert(cu_socket_close(&p.client));
    assert(cu_socket_close(&p.listener));
}

/* With no connection waiting, accept fails at once with EAGAIN and the
 * listener stays usable. */
static void test_accept_nothing_waiting(void)
{
    CuSocket l;
    CuSocket c;
    CuSocket s;

    assert(cu_socket_listen(&l, "127.0.0.1", 0, 1));
    errno = 0;
    assert(!cu_socket_accept(&l, &s));
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(cu_socket_errno(&l) == 0);
    assert(cu_socket_connect(&c, "127.0.0.1", cu_socket_port(&l)));
    accept_one(&l, &s);
    assert(cu_socket_close(&s) && cu_socket_close(&c) && cu_socket_close(&l));
}

/* Writing to a peer that has gone fails with EPIPE (or ECONNRESET) and
 * does not kill the process with SIGPIPE. */
static void test_peer_gone(void)
{
    Pair p;
    int i;
    int ok;

    pair_open(&p);
    assert(cu_socket_close(&p.client));
    ok = 1;
    for (i = 0; i < 1000 && ok; i++)
        ok = cu_socket_write(&p.server, cu_str_cstr("data that nobody reads"));
    assert(!ok);
    assert(cu_socket_errno(&p.server) == EPIPE || cu_socket_errno(&p.server) == ECONNRESET);
    assert(!cu_socket_close(&p.server));
    assert(cu_socket_close(&p.listener));
}

/* Each failure leaves a failed socket that close reports; nothing leaks. */
static void test_open_errors(void)
{
    CuSocket a;
    CuSocket b;
    unsigned port;

    assert(cu_socket_listen(&a, "127.0.0.1", 0, 1));
    port = cu_socket_port(&a);
    assert(!cu_socket_listen(&b, "127.0.0.1", port, 1));
    assert(cu_socket_errno(&b) == EADDRINUSE);
    assert(cu_socket_port(&b) == 0);
    assert(!cu_socket_close(&b));
    assert(cu_socket_close(&a));

    /* Nothing listens on a port just given up. */
    assert(!cu_socket_connect(&b, "127.0.0.1", port));
    assert(cu_socket_errno(&b) == ECONNREFUSED);
    assert(!cu_socket_close(&b));

    assert(!cu_socket_listen(&b, "127.0.0.1", 65536, 1));
    assert(cu_socket_errno(&b) == EINVAL);
    assert(!cu_socket_close(&b));
    assert(!cu_socket_connect(&b, "host.invalid", 80));
    assert(cu_socket_errno(&b) == EADDRNOTAVAIL);
    assert(!cu_socket_close(&b));
    assert(!cu_socket_listen(&b, "not an address", 0, 1));
    assert(cu_socket_errno(&b) == EADDRNOTAVAIL);
    assert(!cu_socket_close(&b));
}

/* IPv6 loopback, where the host has it. */
static void test_ipv6(void)
{
    CuSocket l;
    CuSocket c;
    CuSocket s;
    char buf[2];

    if (!cu_socket_listen(&l, "::1", 0, 1)) {
        assert(!cu_socket_close(&l));
        return;
    }
    assert(cu_socket_connect(&c, "::1", cu_socket_port(&l)));
    accept_one(&l, &s);
    assert(cu_socket_write(&c, cu_str_cstr("v6")));
    read_exactly(&s, buf, 2);
    assert(memcmp(buf, "v6", 2) == 0);
    assert(cu_socket_close(&c) && cu_socket_close(&s) && cu_socket_close(&l));
}

/* ---- Poller ---- */

#define CONNS 3

/* Reports only the readable socket, once: a watch is one-shot. Watching
 * it again after its bytes are read finds it idle. */
static void test_poller_ready(void)
{
    CuArena a;
    CuPoller *p;
    Pair pairs[CONNS];
    CuReady out[CONNS];
    char buf[8];
    size_t n;
    int i;

    cu_arena_init(&a);
    assert(cu_poller_init(&p, &a, CONNS));
    for (i = 0; i < CONNS; i++) {
        pair_open(&pairs[i]);
        assert(cu_poller_add(p, &pairs[i].server, &pairs[i], 0));
    }
    assert(cu_socket_write(&pairs[1].client, cu_str_cstr("ping")));
    assert(cu_poller_wait(p, 5000, out, CONNS, &n));
    assert(n == 1 && out[0].tag == &pairs[1] && !out[0].expired);
    assert(cu_poller_wait(p, 30, out, CONNS, &n) && n == 0);
    read_exactly(&pairs[1].server, buf, 4);
    assert(cu_poller_add(p, &pairs[1].server, &pairs[1], 0));
    assert(cu_poller_wait(p, 30, out, CONNS, &n) && n == 0);

    /* A peer's close is readable too: the read says why. */
    assert(cu_socket_close(&pairs[2].client));
    assert(cu_poller_wait(p, 5000, out, CONNS, &n));
    assert(n == 1 && out[0].tag == &pairs[2] && !out[0].expired);

    cu_poller_destroy(p);
    for (i = 0; i < CONNS; i++)
        pair_close(&pairs[i]);
    cu_arena_release(&a);
}

/* An idle limit reports the watch as expired, not before it runs out, and
 * forgets it; a watch without one is kept. */
static void test_poller_idle(void)
{
    CuArena a;
    CuPoller *p;
    Pair x;
    Pair y;
    CuReady out[2];
    size_t n;

    cu_arena_init(&a);
    assert(cu_poller_init(&p, &a, 2));
    pair_open(&x);
    pair_open(&y);
    assert(cu_poller_add(p, &x.server, &x, 40));
    assert(cu_poller_add(p, &y.server, &y, 0));
    assert(cu_poller_wait(p, 5, out, 2, &n) && n == 0);
    assert(cu_poller_wait(p, 0, out, 2, &n));
    assert(n == 1 && out[0].tag == &x && out[0].expired);
    assert(cu_poller_wait(p, 30, out, 2, &n) && n == 0);
    cu_poller_destroy(p);
    pair_close(&x);
    pair_close(&y);
    cu_arena_release(&a);
}

/* An expired watch that does not fit in out is reported by the next wait,
 * still as expired. */
static void test_poller_deferred(void)
{
    CuArena a;
    CuPoller *p;
    Pair x;
    Pair y;
    CuReady out[1];
    size_t n;
    int expired;
    int readable;

    cu_arena_init(&a);
    assert(cu_poller_init(&p, &a, 2));
    pair_open(&x);
    pair_open(&y);
    assert(cu_poller_add(p, &x.server, &x, 1));
    assert(cu_poller_add(p, &y.server, &y, 0));
    assert(cu_socket_write(&y.client, cu_str_cstr("b")));
    expired = 0;
    readable = 0;
    while (expired + readable < 2) {
        assert(cu_poller_wait(p, 5000, out, 1, &n) && n <= 1);
        if (n == 1 && out[0].tag == &x && out[0].expired)
            expired++;
        else if (n == 1)
            readable += out[0].tag == &y && !out[0].expired;
    }
    assert(expired == 1 && readable == 1);
    cu_poller_destroy(p);
    pair_close(&x);
    pair_close(&y);
    cu_arena_release(&a);
}

/* Capacity counts every watch not yet reported; reporting frees a place.
 * More ready than out holds stay for the next wait. */
static void test_poller_limits(void)
{
    CuArena a;
    CuPoller *p;
    Pair x;
    Pair y;
    CuReady out[1];
    size_t n;

    cu_arena_init(&a);
    assert(cu_poller_init(&p, &a, 2));
    pair_open(&x);
    pair_open(&y);
    assert(cu_poller_add(p, &x.server, &x, 0));
    assert(cu_poller_add(p, &y.server, &y, 0));
    errno = 0;
    assert(!cu_poller_add(p, &x.listener, &x.listener, 0) && errno == ENOSPC);
    assert(cu_socket_write(&x.client, cu_str_cstr("a")));
    assert(cu_socket_write(&y.client, cu_str_cstr("b")));
    assert(cu_poller_wait(p, 5000, NULL, 0, &n) && n == 0);
    assert(cu_poller_wait(p, 5000, out, 1, &n) && n == 1);
    assert(cu_poller_add(p, &x.listener, &x.listener, 0));
    assert(cu_poller_wait(p, 5000, out, 1, &n) && n == 1);
    cu_poller_destroy(p);
    pair_close(&x);
    pair_close(&y);
    cu_arena_release(&a);
}

typedef struct Adder {
    CuPoller *p;
    CuSocket *s;
} Adder;

static void add_from_thread(void *context)
{
    Adder *w;

    w = (Adder *)context;
    if (w->s == NULL)
        cu_poller_wake(w->p);
    else if (!cu_poller_add(w->p, w->s, w->s, 0))
        abort();
}

/* Another thread's add or wake ends a wait that would last forever. */
static void test_poller_threads(void)
{
    CuArena a;
    CuPoller *p;
    CuThread *t;
    Pair x;
    Adder w;
    CuReady out[1];
    size_t n;

    cu_arena_init(&a);
    assert(cu_poller_init(&p, &a, 1));
    pair_open(&x);
    assert(cu_socket_write(&x.client, cu_str_cstr("a")));
    w.p = p;
    w.s = &x.server;
    assert(cu_thread_start(&t, &a, 0, add_from_thread, &w));
    do
        assert(cu_poller_wait(p, 0, out, 1, &n));
    while (n == 0);
    assert(out[0].tag == &x.server);
    cu_thread_join(t);

    /* Empty the wake pipe first, or the add's leftover wake could end the
     * next wait instead of the thread's. */
    assert(cu_poller_wait(p, 1, out, 1, &n) && n == 0);
    w.s = NULL;
    assert(cu_thread_start(&t, &a, 0, add_from_thread, &w));
    assert(cu_poller_wait(p, 0, out, 1, &n) && n == 0);
    cu_thread_join(t);
    cu_poller_destroy(p);
    pair_close(&x);
    cu_arena_release(&a);
}

/* A watched listener is readable when a connection waits to be accepted. */
static void test_poller_listener(void)
{
    CuArena a;
    CuPoller *p;
    CuSocket l;
    CuSocket c;
    CuSocket s;
    CuReady out[1];
    size_t n;

    cu_arena_init(&a);
    assert(cu_poller_init(&p, &a, 1));
    assert(cu_socket_listen(&l, "127.0.0.1", 0, 4));
    assert(cu_poller_add(p, &l, &l, 0));
    assert(cu_socket_connect(&c, "127.0.0.1", cu_socket_port(&l)));
    assert(cu_poller_wait(p, 5000, out, 1, &n) && n == 1 && out[0].tag == &l);
    accept_one(&l, &s);
    cu_poller_destroy(p);
    assert(cu_socket_close(&s) && cu_socket_close(&c) && cu_socket_close(&l));
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
static void listen_null(void)
{
    int ok;

    ok = cu_socket_listen(NULL, "127.0.0.1", 0, 1);
    (void)ok;
}

static void read_zero_cap(void)
{
    Pair p;
    char buf[1];
    size_t got;
    int ok;

    pair_open(&p);
    ok = cu_socket_read(&p.server, buf, 0, &got);
    (void)ok;
}

static void read_after_close(void)
{
    Pair p;
    char buf[1];
    size_t got;
    int ok;

    pair_open(&p);
    assert(cu_socket_close(&p.server));
    ok = cu_socket_read(&p.server, buf, 1, &got);
    (void)ok;
}

static void read_listener(void)
{
    Pair p;
    char buf[1];
    size_t got;
    int ok;

    pair_open(&p);
    ok = cu_socket_read(&p.listener, buf, 1, &got);
    (void)ok;
}

static void accept_alias(void)
{
    Pair p;
    int ok;

    pair_open(&p);
    ok = cu_socket_accept(&p.listener, &p.listener);
    (void)ok;
}

static void accept_on_connection(void)
{
    Pair p;
    CuSocket s;
    int ok;

    pair_open(&p);
    ok = cu_socket_accept(&p.client, &s);
    (void)ok;
}

static void poller_after_destroy(void)
{
    CuArena a;
    CuPoller *p;

    cu_arena_init(&a);
    assert(cu_poller_init(&p, &a, 1));
    cu_poller_destroy(p);
    cu_poller_wake(p);
}

static void poller_zero_capacity(void)
{
    CuArena a;
    CuPoller *p;
    int ok;

    cu_arena_init(&a);
    ok = cu_poller_init(&p, &a, 0);
    (void)ok;
}

static void add_twice(void)
{
    CuArena a;
    CuPoller *p;
    Pair x;
    int ok;

    cu_arena_init(&a);
    assert(cu_poller_init(&p, &a, 2));
    pair_open(&x);
    assert(cu_poller_add(p, &x.server, &x, 0));
    ok = cu_poller_add(p, &x.server, &x, 0);
    (void)ok;
}

static void close_while_watched(void)
{
    CuArena a;
    CuPoller *p;
    Pair x;
    int ok;

    cu_arena_init(&a);
    assert(cu_poller_init(&p, &a, 1));
    pair_open(&x);
    assert(cu_poller_add(p, &x.server, &x, 0));
    ok = cu_socket_close(&x.server);
    (void)ok;
}

static void death_tests(void)
{
    expect_abort(accept_alias);
    expect_abort(add_twice);
    expect_abort(close_while_watched);
    expect_abort(listen_null);
    expect_abort(read_zero_cap);
    expect_abort(read_after_close);
    expect_abort(read_listener);
    expect_abort(accept_on_connection);
    expect_abort(poller_after_destroy);
    expect_abort(poller_zero_capacity);
}
#endif

int main(void)
{
    test_round_trip();
    test_large_transfer();
    test_timeout();
    test_signal_timeout(0);
    test_signal_timeout(1);
    test_write_timeout();
    test_accept_nothing_waiting();
    test_peer_gone();
    test_open_errors();
    test_ipv6();
    test_poller_ready();
    test_poller_idle();
    test_poller_deferred();
    test_poller_limits();
    test_poller_threads();
    test_poller_listener();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    cu_scratch_release();
    return 0;
}
#endif
