#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include <string.h>
#include "cu.h"

#if !defined(__cerb__) && !defined(__wasi__) && (defined(__unix__) || defined(__APPLE__))
#include <sys/socket.h>
#include <time.h>

static const char post[] = "POST /upload HTTP/1.1\r\nHost: example.test\r\nContent-Length: 5\r\n\r\n";
static const char next[] = "GET /next HTTP/1.1\r\nHost: example.test\r\n\r\n";

typedef struct Pair {
    CuSocket listener;
    CuSocket client;
    CuSocket server;
} Pair;

static void open_pair(Pair *p)
{
    assert(cu_socket_listen(&p->listener, "127.0.0.1", 0, 4));
    assert(cu_socket_connect_ip_for(&p->client, "127.0.0.1", cu_socket_port(&p->listener), 1000));
    assert(cu_socket_accept(&p->listener, &p->server));
    assert(cu_socket_set_timeout(&p->client, 777));
}

static void close_pair(Pair *p)
{
    int closed;

    closed = cu_socket_close(&p->client);
    assert(closed || cu_socket_errno(&p->client) == ETIMEDOUT);
    assert(cu_socket_close(&p->server));
    assert(cu_socket_close(&p->listener));
}

static void pause_ms(long ms)
{
    struct timespec t;

    t.tv_sec = ms / 1000;
    t.tv_nsec = ms % 1000 * 1000000L;
    while (nanosleep(&t, &t) != 0)
        assert(errno == EINTR);
}

static void pipeline(void)
{
    Pair p;
    CuReader reader;
    CuHttpRequest request;
    char head[128];
    char body[32];
    char peek;
    size_t n;
    size_t used;

    open_pair(&p);
    assert(cu_socket_write(&p.server, cu_str_cstr(post)));
    assert(cu_socket_write(&p.server, cu_str_n("ab\0cd", 5)));
    assert(cu_socket_write(&p.server, cu_str_cstr(next)));
    assert(cu_reader_socket(&reader, &p.client, 1000));
    assert(cu_http_begin(&request, &reader, head, sizeof head, 2, 5) == CU_HTTP_OK);
    used = 0;
    while (used < 5) {
        assert(cu_http_read(&request, body + used, sizeof body - used, &n) == CU_HTTP_OK);
        assert(n > 0 && n <= 5 - used);
        used += n;
    }
    assert(memcmp(body, "ab\0cd", 5) == 0);
    assert(cu_http_read(&request, body, sizeof body, &n) == CU_HTTP_END && n == 0);
    assert(recv(p.client.fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT) == 1 && peek == 'G');
    assert(p.client.timeout_ms == 777);
    /* First request is complete. Rebinding starts the next request's budget,
     * without changing the socket cursor or its configured timeout. */
    assert(cu_reader_socket(&reader, &p.client, 1000));
    assert(cu_http_begin(&request, &reader, head, sizeof head, 1, 0) == CU_HTTP_OK);
    assert(cu_str_eq(request.target, CU_LIT("/next")));
    assert(cu_http_read(&request, body, sizeof body, &n) == CU_HTTP_END);
    close_pair(&p);
}

static void caller_pause(void)
{
    Pair p;
    CuReader reader;
    CuHttpRequest request;
    char head[128];
    char body[8];
    size_t n;

    open_pair(&p);
    assert(cu_socket_write(&p.server, cu_str_cstr(post)));
    assert(cu_socket_write(&p.server, cu_str_cstr("abcde")));
    assert(cu_reader_socket(&reader, &p.client, 50));
    assert(cu_http_begin(&request, &reader, head, sizeof head, 2, 5) == CU_HTTP_OK);
    pause_ms(80);
    assert(cu_http_read(&request, body, sizeof body, &n) == CU_HTTP_IO && n == 0);
    assert(cu_http_errno(&request) == ETIMEDOUT && p.client.timeout_ms == 777);
    /* Abandon the failed request/view. The pre-read expiry consumed no body
     * and did not change the owner. Production callers close on HTTP errors. */
    assert(cu_socket_read_for(&p.client, body, 5, &n, 1000) && n == 5);
    assert(memcmp(body, "abcde", 5) == 0);
    close_pair(&p);
}

static void drip(void *context)
{
    CuSocket *s;
    size_t i;

    s = (CuSocket *)context;
    assert(cu_socket_write(s, cu_str_cstr(post)));
    for (i = 0; i < 5; i++) {
        pause_ms(100);
        assert(cu_socket_write(s, cu_str_n("x", 1)));
    }
}

static void whole_budget(void)
{
    Pair p;
    CuArena arena;
    CuThread *sender;
    CuReader reader;
    CuHttpRequest request;
    struct timespec start;
    struct timespec end;
    CuHttpResult status;
    char head[128];
    char body[8];
    size_t n;
    size_t total;
    double elapsed;

    open_pair(&p);
    cu_arena_init(&arena);
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    assert(cu_reader_socket(&reader, &p.client, 250));
    assert(cu_thread_start(&sender, &arena, 0, drip, &p.server));
    assert(cu_http_begin(&request, &reader, head, sizeof head, 2, 5) == CU_HTTP_OK);
    total = 0;
    while ((status = cu_http_read(&request, body, sizeof body, &n)) == CU_HTTP_OK) {
        assert(n > 0 && n <= 5 - total);
        total += n;
    }
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    assert(status == CU_HTTP_IO && cu_http_errno(&request) == ETIMEDOUT && n == 0);
    assert(total < 5 && p.client.timeout_ms == 777);
    elapsed = (double)(end.tv_sec - start.tv_sec) +
        (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
    assert(elapsed >= 0.15 && elapsed < 0.8);
    /* Keep the peer open while the sender finishes, avoiding a test-only
     * close/write race. The timeout observation above excludes this join. */
    cu_thread_join(sender);
    cu_arena_release(&arena);
    close_pair(&p);
}

static void override(void)
{
    Pair p;
    char byte;
    size_t n;

    open_pair(&p);
    assert(!cu_socket_read_for(&p.client, &byte, 1, &n, 10));
    assert(cu_socket_errno(&p.client) == ETIMEDOUT && p.client.timeout_ms == 777);
    close_pair(&p);
}
#endif

int main(void)
{
#if !defined(__cerb__) && !defined(__wasi__) && (defined(__unix__) || defined(__APPLE__))
    pipeline();
    caller_pause();
    whole_budget();
    override();
#endif
    return 0;
}
