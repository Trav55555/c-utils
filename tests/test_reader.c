#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "death.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

static void overlap(void)
{
    CuReader reader;
    char bytes[4];
    size_t n;

    memcpy(bytes, "abcd", 4);
    cu_reader_memory(&reader, cu_str_n(bytes, 4));
    assert(cu_reader_read(&reader, bytes + 1, 2, &n) == CU_INPUT_DATA && n == 2);
    assert(memcmp(bytes, "aabd", 4) == 0);
    /* The caller overwrote an unread source byte; subsequent reads see the
     * source's current contents, not an implicit snapshot. */
    assert(cu_reader_read(&reader, bytes, 1, &n) == CU_INPUT_DATA && n == 1 && bytes[0] == 'b');
}

static void memory(void)
{
    static const size_t caps[] = {1, 3, 17};
    static const char bytes[] = "ab\0cdEFghI";
    CuReader r;
    size_t i;
    size_t used;
    size_t n;
    char *buf;
    CuInputStatus status;

    for (i = 0; i < sizeof caps / sizeof caps[0]; i++) {
        buf = (char *)malloc(caps[i]);
        assert(buf != NULL);
        cu_reader_memory(&r, cu_str_n(bytes, sizeof bytes - 1));
        used = 0;
        while ((status = cu_reader_read(&r, buf, caps[i], &n)) == CU_INPUT_DATA) {
            assert(n > 0 && n <= caps[i] && n <= sizeof bytes - 1 - used);
            assert(memcmp(buf, bytes + used, n) == 0);
            used += n;
        }
        assert(status == CU_INPUT_END && n == 0 && used == sizeof bytes - 1);
        n = 99;
        assert(cu_reader_read(&r, buf, caps[i], &n) == CU_INPUT_END && n == 0);
        free(buf);
    }
    cu_reader_memory(&r, cu_str_n(NULL, 0));
    assert(cu_reader_read(&r, &status, sizeof status, &n) == CU_INPUT_END && n == 0);
    cu_reader_memory(&r, cu_str_cstr("unused"));
    r.state = CU_INPUT_ERROR;
    r.error = 0;
    errno = ERANGE;
    assert(cu_reader_read(&r, &status, sizeof status, &n) == CU_INPUT_ERROR && n == 0);
    assert(cu_reader_errno(&r) == 0 && errno == 0 && r.source.memory.len == 6);
}

#if !defined(__cerb__) && !defined(__wasi__) && (defined(__unix__) || defined(__APPLE__))
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static struct timespec stamps[3];
static int stamp_count;
static int clock_calls;
static int bad_at;
static int clock_error;

static int test_clock(clockid_t clock, struct timespec *out)
{
    int i;

    assert(clock == CLOCK_MONOTONIC);
    i = clock_calls++;
    if (i == bad_at) {
        errno = clock_error;
        return -1;
    }
    assert(stamp_count > 0);
    if (i >= stamp_count)
        i = stamp_count - 1;
    *out = stamps[i];
    return 0;
}

void reader_memory_probe(CuReader *r, CuStr bytes);
int reader_socket_probe(CuReader *r, CuSocket *s, unsigned long timeout_ms);
CuInputStatus reader_read_probe(CuReader *r, void *buf, size_t cap, size_t *n);
int reader_errno_probe(const CuReader *r);
#define clock_gettime test_clock
#define cu_reader_memory reader_memory_probe
#define cu_reader_socket reader_socket_probe
#define cu_reader_read reader_read_probe
#define cu_reader_errno reader_errno_probe
#include "../src/cu_reader.c"
#undef clock_gettime
#undef cu_reader_memory
#undef cu_reader_socket
#undef cu_reader_read
#undef cu_reader_errno

static void reset_clock(void)
{
    size_t i;

    stamp_count = 3;
    clock_calls = 0;
    bad_at = -1;
    clock_error = EIO;
    for (i = 0; i < 3; i++) {
        stamps[i].tv_sec = 10;
        stamps[i].tv_nsec = 999500000L;
    }
}

static int peer(CuSocket *s)
{
    int fd[2];

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    s->fd = fd[0];
    s->state = 1;
    s->error = 0;
    s->listen = 0;
    s->watched = 0;
    s->timeout_ms = 123;
    assert(write(fd[1], "ab", 2) == 2);
    return fd[1];
}

static void precise_budget(void)
{
    CuSocket s;
    CuReader r;
    char byte;
    size_t n;
    int fd;
    int calls;

    fd = peer(&s);
    reset_clock();
    stamps[1].tv_nsec = 999750000L;
    stamps[2].tv_sec = 11;
    stamps[2].tv_nsec = 100000L;
    assert(reader_socket_probe(&r, &s, 1));
    assert(reader_read_probe(&r, &byte, 1, &n) == CU_INPUT_DATA && n == 1 && byte == 'a');
    assert(s.timeout_ms == 123);
    assert(cu_socket_close(&s) && close(fd) == 0);

    fd = peer(&s);
    reset_clock();
    stamps[2].tv_sec = 11;
    stamps[2].tv_nsec = 500000L;
    assert(reader_socket_probe(&r, &s, 1));
    assert(reader_read_probe(&r, &byte, 1, &n) == CU_INPUT_ERROR && n == 0);
    assert(reader_errno_probe(&r) == ETIMEDOUT && s.timeout_ms == 123);
    calls = clock_calls;
    errno = ERANGE;
    assert(reader_read_probe(&r, &byte, 1, &n) == CU_INPUT_ERROR && n == 0);
    assert(errno == ETIMEDOUT && clock_calls == calls);
    assert(cu_socket_read(&s, &byte, 1, &n) && byte == 'b' && n == 1);
    assert(cu_socket_close(&s) && close(fd) == 0);

    fd = peer(&s);
    reset_clock();
    stamps[0].tv_sec = LONG_MAX - 1;
    stamps[1] = stamps[0];
    stamps[2] = stamps[0];
    assert(reader_socket_probe(&r, &s, ULONG_MAX));
    assert(reader_read_probe(&r, &byte, 1, &n) == CU_INPUT_DATA && n == 1 && byte == 'a');
    assert(cu_socket_close(&s) && close(fd) == 0);
}

static void clock_failures(void)
{
    CuSocket s;
    CuReader r;
    char byte;
    size_t n;
    int fd;
    int phase;

    for (phase = 0; phase < 3; phase++) {
        fd = peer(&s);
        reset_clock();
        bad_at = phase;
        clock_error = phase == 1 ? 0 : EIO;
        if (phase == 0) {
            assert(!reader_socket_probe(&r, &s, 100));
        } else {
            assert(reader_socket_probe(&r, &s, 100));
            assert(reader_read_probe(&r, &byte, 1, &n) == CU_INPUT_ERROR && n == 0);
        }
        assert(reader_errno_probe(&r) == clock_error && s.timeout_ms == 123);
        errno = ERANGE;
        assert(reader_read_probe(&r, &byte, 1, &n) == CU_INPUT_ERROR && n == 0);
        assert(errno == clock_error);
        assert(cu_socket_read(&s, &byte, 1, &n) && n == 1);
        assert(byte == (phase == 2 ? 'b' : 'a'));
        assert(cu_socket_close(&s) && close(fd) == 0);
    }
}
#endif

#ifdef CU_DEATH_TESTS
static void null_reader(void)
{
    cu_reader_memory(NULL, cu_str_n(NULL, 0));
}

static void invalid_view(void)
{
    CuReader r;
    CuStr bytes;

    bytes.ptr = NULL;
    bytes.len = 1;
    cu_reader_memory(&r, bytes);
}

static void zero_capacity(void)
{
    CuReader r;
    char b;
    size_t n;
    CuInputStatus status;

    cu_reader_memory(&r, cu_str_cstr("x"));
    status = cu_reader_read(&r, &b, 0, &n);
    (void)status;
}
#endif

int main(void)
{
    overlap();
    memory();
#if !defined(__cerb__) && !defined(__wasi__) && (defined(__unix__) || defined(__APPLE__))
    precise_budget();
    clock_failures();
#endif
#ifdef CU_DEATH_TESTS
    expect_abort(null_reader);
    expect_abort(invalid_view);
    expect_abort(zero_capacity);
#endif
    return 0;
}
