#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
int main(void) { return 0; }
#else
/* Include every system header before interposition, including those used by
 * cu_socket.c. This keeps native declarations intact on 32-bit targets too. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define cu_socket_errno boundary_errno
#define cu_socket_listen boundary_listen
#define cu_socket_connect boundary_connect
#define cu_socket_connect_ip_for boundary_ip
#define cu_socket_accept boundary_accept
#define cu_socket_port boundary_port
#define cu_socket_set_timeout boundary_timeout
#define cu_socket_read boundary_read
#define cu_socket_read_for boundary_read_for
#define cu_socket_write boundary_write
#define cu_socket_shutdown_write boundary_shutdown_write
#define cu_socket_close boundary_close
#define cu_poller_init boundary_poller_init
#define cu_poller_add boundary_poller_add
#define cu_poller_wait boundary_poller_wait
#define cu_poller_wake boundary_poller_wake
#define cu_poller_destroy boundary_poller_destroy
#include "cu.h"

#define IO_LIMIT 64
#define CLOCK_LIMIT 256
#define BUDGET 10UL
#define ORIGIN 1997UL
#define FILL 'x'

typedef struct ReceiveStep {
    ssize_t result;
    int error;
    unsigned long tick;
} ReceiveStep;

typedef struct PollStep {
    int result;
    int error;
    short ready;
    unsigned long tick;
} PollStep;

typedef struct ClockStep {
    int fails;
    int error;
    unsigned long tick;
} ClockStep;

typedef struct Model {
    ReceiveStep receives[IO_LIMIT];
    PollStep polls[IO_LIMIT];
    ClockStep clocks[CLOCK_LIMIT];
    unsigned receives_length;
    unsigned polls_length;
    unsigned clock_length;
    unsigned receive_calls;
    unsigned poll_calls;
    unsigned clock_calls;
    unsigned io_calls;
    int io_events[IO_LIMIT];
    unsigned forbid_io_clock;
    unsigned close_calls;
    unsigned long now;
    unsigned long budget;
    char *buffer;
    size_t capacity;
    size_t expected_capacity;
    size_t check_extent;
    int reading;
    int closing;
    int forbid_io;
    int first_io;
} Model;

typedef struct Guard7 {
    unsigned char before;
    char data[7];
    unsigned char after;
} Guard7;

typedef struct Guard1 {
    unsigned char before;
    char data[1];
    unsigned char after;
} Guard1;

static Model model;
static const char payload[7] = { 'A', '\0', 'B', '\177', 'C', 'D', 'E' };

static void io_event(int kind)
{
    assert(model.reading);
    assert(!model.forbid_io && "I/O before a successful unexpired clock check");
    assert(model.io_calls < IO_LIMIT && "read made more than 64 I/O calls");
    model.io_events[model.io_calls++] = kind;
    if (model.first_io == 0)
        model.first_io = kind;
}

static ssize_t model_recv(int fd, void *buffer, size_t capacity, int flags)
{
    ReceiveStep step;

    io_event(1);
    assert(fd == 40);
    assert(buffer == model.buffer);
    /* Supplied capacity has real backing; syscall capacity may be clamped. */
    assert(capacity == model.expected_capacity);
    assert(flags == (model.budget ? MSG_DONTWAIT : 0));
    assert(model.receive_calls < model.receives_length && "recv script exhausted");
    step = model.receives[model.receive_calls++];
    model.now += step.tick;
    if (step.result >= 0) {
        assert((size_t)step.result <= capacity);
        assert((size_t)step.result <= sizeof payload);
        assert((size_t)step.result <= model.check_extent);
        memcpy(buffer, payload, (size_t)step.result);
    }
    errno = step.error;
    return step.result;
}

static int model_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    PollStep step;
    unsigned long elapsed;

    io_event(2);
    assert(model.budget != 0 && "unlimited read must not poll");
    assert(fds != NULL && count == 1);
    assert(fds[0].fd == 40);
    assert(fds[0].events == POLLIN);
    assert(timeout > 0);
    elapsed = model.now - ORIGIN;
    assert(elapsed < model.budget && "poll started after the original budget");
    assert((unsigned long)timeout <= model.budget - elapsed &&
           "poll reset or enlarged the original budget");
    assert(model.poll_calls < model.polls_length && "poll script exhausted");
    step = model.polls[model.poll_calls++];
    model.now += step.tick;
    fds[0].revents = step.ready;
    errno = step.error;
    return step.result;
}

static int model_clock_gettime(clockid_t clock_id, struct timespec *out)
{
    ClockStep step;

    assert(model.reading);
    assert(model.budget != 0 && "unlimited read must not read the clock");
    assert(clock_id == CLOCK_MONOTONIC);
    assert(out != NULL);
    assert(model.clock_calls < model.clock_length && "clock script exhausted");
    if (model.clock_calls == model.forbid_io_clock)
        model.forbid_io = 1;
    step = model.clocks[model.clock_calls++];
    model.now += step.tick;
    if (step.fails) {
        errno = step.error;
        return -1;
    }
    out->tv_sec = (time_t)(model.now / 1000UL);
    out->tv_nsec = (long)(model.now % 1000UL) * 1000000L;
    return 0;
}

static int model_fcntl(int fd, int command, ...)
{
    (void)fd;
    (void)command;
    assert(!model.reading && "read must not query or mutate descriptor flags");
    assert(0 && "unexpected fcntl outside read");
    return -1;
}

static int model_close(int fd)
{
    assert(!model.reading && "read must not close its descriptor");
    assert(model.closing);
    assert(fd == 40);
    assert(model.close_calls == 0 && "descriptor closed more than once");
    model.close_calls++;
    /* Successful cleanup is allowed to clobber errno. */
    errno = EBUSY;
    return 0;
}

/* Compile a private copy against static model boundaries, without GNU
 * linker wrapping or replacement of the real APIs used by other tests. */
#define recv model_recv
#define poll model_poll
#define clock_gettime model_clock_gettime
#define fcntl model_fcntl
#define close model_close
#include "../src/cu_socket.c"
#undef recv
#undef poll
#undef clock_gettime
#undef fcntl
#undef close

static void setup_checked(const char *name, unsigned long budget,
                          char *buffer, size_t capacity,
                          size_t expected_capacity, size_t check_extent)
{
    unsigned i;

    fprintf(stderr, "case: %s\n", name);
    model.receives_length = IO_LIMIT;
    model.polls_length = IO_LIMIT;
    model.clock_length = CLOCK_LIMIT;
    model.receive_calls = 0;
    model.poll_calls = 0;
    model.clock_calls = 0;
    model.io_calls = 0;
    model.forbid_io_clock = CLOCK_LIMIT;
    model.close_calls = 0;
    model.now = ORIGIN;
    model.budget = budget;
    model.buffer = buffer;
    model.capacity = capacity;
    model.expected_capacity = expected_capacity;
    model.check_extent = check_extent;
    assert(expected_capacity <= capacity);
    assert(check_extent <= capacity);
    model.reading = 0;
    model.closing = 0;
    model.forbid_io = 0;
    model.first_io = 0;
    for (i = 0; i < IO_LIMIT; i++) {
        model.receives[i].result = -1;
        model.receives[i].error = EAGAIN;
        model.receives[i].tick = 0;
        model.polls[i].result = 1;
        model.polls[i].error = 0;
        model.polls[i].ready = POLLIN;
        model.polls[i].tick = 0;
    }
    for (i = 0; i < CLOCK_LIMIT; i++) {
        model.clocks[i].fails = 0;
        model.clocks[i].error = 0;
        model.clocks[i].tick = 0;
    }
    memset(buffer, FILL, check_extent);
}

static void setup(const char *name, unsigned long budget,
                  char *buffer, size_t capacity)
{
    setup_checked(name, budget, buffer, capacity, capacity, capacity);
}

static void expect_io(const char *events)
{
    unsigned i;

    assert(strlen(events) == (size_t)model.io_calls);
    for (i = 0; i < model.io_calls; i++)
        assert(events[i] == (model.io_events[i] == 1 ? 'R' : 'P'));
}

static void socket_init(CuSocket *s)
{
    s->fd = 40;
    s->state = 1;
    s->error = 0;
    s->listen = 0;
    s->watched = 0;
    s->timeout_ms = 777;
}

static void unchanged(const CuSocket *s)
{
    assert(s->fd == 40);
    assert(s->listen == 0);
    assert(s->watched == 0);
    assert(s->timeout_ms == 777 && "explicit budget changed configured timeout");
    assert(model.close_calls == 0);
}

static int read_once(CuSocket *s, size_t *got)
{
    int ok;

    model.reading = 1;
    ok = cu_socket_read_for(s, model.buffer, model.capacity, got, model.budget);
    model.reading = 0;
    unchanged(s);
    return ok;
}

static void check_bytes(size_t count)
{
    size_t i;

    assert(count <= model.capacity);
    assert(count <= model.check_extent);
    assert(memcmp(model.buffer, payload, count) == 0);
    for (i = count; i < model.check_extent; i++)
        assert(model.buffer[i] == FILL);
}

static void check_success(CuSocket *s, size_t got, size_t expected)
{
    assert(s->state == 1);
    assert(s->error == 0);
    assert(got == expected);
    check_bytes(got);
}

static void check_failed(CuSocket *s, int error)
{
    unsigned io_before;
    unsigned clocks_before;
    size_t ignored;
    int ok;

    assert(s->state == -1);
    assert(s->error == error);
    assert(errno == error);
    assert(cu_socket_errno(s) == error);
    io_before = model.io_calls;
    clocks_before = model.clock_calls;
    ignored = (size_t)123;
    errno = EDOM;
    assert(!read_once(s, &ignored));
    assert(errno == error);
    errno = ERANGE;
    model.reading = 1;
    ok = cu_socket_read(s, model.buffer, model.capacity, &ignored);
    model.reading = 0;
    assert(!ok && errno == error);
    unchanged(s);
    assert(model.io_calls == io_before && model.clock_calls == clocks_before);
    /* The socket contract specifies *got only on success, not on failure. */
}

static void finish(CuSocket *s, int failed, int error)
{
    int ok;

    model.closing = 1;
    ok = cu_socket_close(s);
    assert(ok == !failed);
    assert(model.close_calls == 1 && s->fd == -1);
    assert(s->state == (failed ? -2 : 0));
    if (failed)
        assert(errno == error && s->error == error);
    errno = EDOM;
    ok = cu_socket_close(s);
    assert(ok == !failed);
    assert(model.close_calls == 1);
    if (failed)
        assert(errno == error);
    model.closing = 0;
}

static void success_case(const char *name, size_t bytes,
                         unsigned long budget, int fast, int recover)
{
    Guard7 guard;
    CuSocket s;
    size_t got;
    unsigned index;

    guard.before = 0xa5;
    guard.after = 0x5a;
    setup(name, budget, guard.data, sizeof guard.data);
    socket_init(&s);
    index = 0;
    if (recover == 1) {
        model.receives[0].error = EAGAIN;
        model.receives[1].error = EINTR;
        model.receives[2].error = EWOULDBLOCK;
        index = 3;
    } else if (recover == 2) {
        model.receives[0].error = EINTR;
        index = 1;
    }
    model.receives[index].result = (ssize_t)bytes;
    model.receives_length = index + 1;
    got = (size_t)123;
    assert(read_once(&s, &got));
    check_success(&s, got, bytes);
    if (fast) {
        assert(model.poll_calls == 0 && "warm DATA/EOF must require zero polls");
        assert(model.first_io == 1 && "warm DATA/EOF first I/O must be recv");
    }
    if (!budget)
        assert(model.poll_calls == 0 && model.clock_calls == 0);
    assert(guard.before == 0xa5 && guard.after == 0x5a);
    finish(&s, 0, 0);
}

static void one_byte_case(void)
{
    Guard1 guard;
    CuSocket s;
    size_t got;

    guard.before = 0xa5;
    guard.after = 0x5a;
    setup("exact capacity one", BUDGET, guard.data, sizeof guard.data);
    socket_init(&s);
    model.receives[0].result = 1;
    model.receives_length = 1;
    got = (size_t)123;
    assert(read_once(&s, &got));
    check_success(&s, got, 1);
    assert(guard.before == 0xa5 && guard.after == 0x5a);
    finish(&s, 0, 0);
}

enum FailureKind {
    RECV_ERROR, POLL_ERROR, CLOCK_START_ERROR, CLOCK_CHECK_ERROR,
    PRE_IO_EXPIRY, RECV_INTERRUPTS, POLL_INTERRUPTS, SPURIOUS_READY,
    WOULD_BLOCK
};

static void failure_case(const char *name, enum FailureKind kind,
                         int native_error, unsigned long budget)
{
    Guard7 guard;
    CuSocket s;
    size_t ignored;
    unsigned i;
    int expected;

    guard.before = 0xa5;
    guard.after = 0x5a;
    setup(name, budget, guard.data, sizeof guard.data);
    socket_init(&s);
    expected = native_error;
    if (kind == RECV_ERROR) {
        model.receives[0].error = native_error;
        model.receives_length = 1;
        if (native_error == EAGAIN || native_error == EWOULDBLOCK)
            expected = ETIMEDOUT;
    } else if (kind == POLL_ERROR) {
        model.polls[0].result = -1;
        model.polls[0].error = native_error;
        model.polls_length = 1;
    } else if (kind == CLOCK_START_ERROR || kind == CLOCK_CHECK_ERROR) {
        i = kind == CLOCK_START_ERROR ? 0 : 1;
        model.clocks[i].fails = 1;
        model.clocks[i].error = native_error;
        model.forbid_io = 1;
    } else if (kind == PRE_IO_EXPIRY) {
        model.clocks[1].tick = BUDGET;
        model.forbid_io = 1;
        expected = ETIMEDOUT;
    } else {
        expected = ETIMEDOUT;
        for (i = 0; i < IO_LIMIT; i++) {
            if (kind == RECV_INTERRUPTS) {
                model.receives[i].error = EINTR;
                model.receives[i].tick = 4;
            } else if (kind == POLL_INTERRUPTS) {
                model.polls[i].result = -1;
                model.polls[i].error = EINTR;
                model.polls[i].tick = 4;
            } else {
                model.receives[i].error = native_error;
                model.receives[i].tick = 2;
                model.polls[i].tick = 1;
                if (kind == SPURIOUS_READY)
                    model.polls[i].ready = 0;
            }
        }
    }
    ignored = (size_t)123;
    assert(!read_once(&s, &ignored));
    check_failed(&s, expected);
    if (model.forbid_io)
        assert(model.io_calls == 0);
    if (!budget)
        assert(model.poll_calls == 0 && model.clock_calls == 0);
    assert(guard.before == 0xa5 && guard.after == 0x5a);
    finish(&s, 1, expected);
}

static void poll_timeout_case(void)
{
    Guard7 guard;
    CuSocket s;
    size_t ignored;
    unsigned i;

    guard.before = 0xa5;
    guard.after = 0x5a;
    setup("poll zero results spend original budget", BUDGET,
          guard.data, sizeof guard.data);
    socket_init(&s);
    for (i = 0; i < IO_LIMIT; i++) {
        model.polls[i].result = 0;
        model.polls[i].ready = 0;
        model.polls[i].tick = 4;
    }
    ignored = (size_t)123;
    assert(!read_once(&s, &ignored));
    check_failed(&s, ETIMEDOUT);
    assert(guard.before == 0xa5 && guard.after == 0x5a);
    finish(&s, 1, ETIMEDOUT);
}

static void late_ready_case(void)
{
    Guard7 guard;
    CuSocket s;
    size_t got;
    int ok;

    guard.before = 0xa5;
    guard.after = 0x5a;
    setup("raw late readiness has no stronger post-poll oracle", BUDGET,
          guard.data, sizeof guard.data);
    socket_init(&s);
    model.polls[0].tick = 12;
    model.receives[1].result = 3;
    model.receives_length = 2;
    got = (size_t)123;
    ok = read_once(&s, &got);
    /* Baseline can recv after late readiness. The optimistic call can
     * consume the EAGAIN earlier, so either valid outcome is accepted. */
    if (ok)
        check_success(&s, got, 3);
    else
        check_failed(&s, ETIMEDOUT);
    assert(guard.before == 0xa5 && guard.after == 0x5a);
    finish(&s, !ok, ETIMEDOUT);
}

static void fallback_sequence_case(const char *name, int initial_error,
                                   int repeated_eagain, int poll_eintr)
{
    Guard7 guard;
    CuSocket s;
    size_t got;
    unsigned data_index;

    guard.before = 0xa5;
    guard.after = 0x5a;
    setup(name, BUDGET, guard.data, sizeof guard.data);
    socket_init(&s);
    model.receives[0].error = initial_error;
    data_index = repeated_eagain ? 2 : 1;
    model.receives[data_index].result = 3;
    model.receives_length = data_index + 1;
    model.polls_length = data_index + (poll_eintr ? 1 : 0);
    if (poll_eintr) {
        model.polls[0].result = -1;
        model.polls[0].error = EINTR;
    }
    got = (size_t)123;
    assert(read_once(&s, &got));
    check_success(&s, got, 3);
    /* Compare recorded calls, not a second implementation of the loop. */
    if (poll_eintr)
        expect_io("RPPR");
    else if (repeated_eagain)
        expect_io("RPRPR");
    else
        expect_io("RPR");
    assert(guard.before == 0xa5 && guard.after == 0x5a);
    finish(&s, 0, 0);
}

static void fallback_clock_case(const char *name, int expires, int error)
{
    Guard7 guard;
    CuSocket s;
    size_t ignored;
    int expected;

    guard.before = 0xa5;
    guard.after = 0x5a;
    setup(name, BUDGET, guard.data, sizeof guard.data);
    socket_init(&s);
    model.receives_length = 1;
    model.polls_length = 0;
    /* Start clock 0, optimistic precheck 1, recv EAGAIN, fallback check 2.
     * Permit that first recv, but forbid BOTH poll and recv at/after check 2. */
    model.forbid_io_clock = 2;
    model.clocks[2].fails = !expires;
    model.clocks[2].error = error;
    model.clocks[2].tick = expires ? BUDGET : 0;
    expected = expires ? ETIMEDOUT : error;
    ignored = (size_t)123;
    assert(!read_once(&s, &ignored));
    assert(model.clock_calls == 3 && model.forbid_io);
    expect_io("R");
    check_failed(&s, expected);
    check_bytes(0);
    assert(guard.before == 0xa5 && guard.after == 0x5a);
    finish(&s, 1, expected);
}

static void large_capacity_cases(void)
{
    char *buffer;
    size_t chunk;
    size_t capacity;
    size_t got;
    unsigned fallback;
    CuSocket s;

    chunk = (size_t)1 << 30;
    capacity = chunk + 1;
    /* Real logical backing for the entire supplied capacity, allocated once.
     * Only seven bytes are initialized/checked; recv copies three binary bytes.
     * This tests the argument clamp, not a native 1 GiB transfer. */
    buffer = (char *)malloc(capacity);
    if (buffer == NULL) {
        fprintf(stderr, "MAX_CHUNK fixture allocation failed; not skipped\n");
        exit(EXIT_FAILURE);
    }
    for (fallback = 0; fallback < 2; fallback++) {
        setup_checked(fallback ? "fast MAX_CHUNK fallback" :
                      "fast MAX_CHUNK warm", BUDGET,
                      buffer, capacity, chunk, 7);
        socket_init(&s);
        model.receives[fallback].result = 3;
        model.receives_length = fallback + 1;
        model.polls_length = fallback;
        got = (size_t)123;
        assert(read_once(&s, &got));
        check_success(&s, got, 3);
        expect_io(fallback ? "RPR" : "R");
        finish(&s, 0, 0);
    }
    free(buffer);
}

static void contracts(void)
{
    success_case("warm full binary DATA", 7, BUDGET, 0, 0);
    success_case("partial DATA", 3, BUDGET, 0, 0);
    success_case("warm EOF", 0, BUDGET, 0, 0);
    one_byte_case();
    success_case("EAGAIN/EINTR/EWOULDBLOCK then DATA", 3, BUDGET, 0, 1);
    success_case("zero unlimited DATA", 7, 0, 0, 0);
    success_case("zero unlimited EOF", 0, 0, 0, 0);
    success_case("zero unlimited EINTR/retry DATA", 3, 0, 0, 2);
    failure_case("recv EIO sticky", RECV_ERROR, EIO, BUDGET);
    failure_case("recv errno zero sticky", RECV_ERROR, 0, BUDGET);
    failure_case("poll EIO sticky", POLL_ERROR, EIO, BUDGET);
    failure_case("poll errno zero sticky", POLL_ERROR, 0, BUDGET);
    failure_case("start clock EIO before I/O", CLOCK_START_ERROR, EIO, BUDGET);
    failure_case("start clock zero before I/O", CLOCK_START_ERROR, 0, BUDGET);
    failure_case("check clock EIO before I/O", CLOCK_CHECK_ERROR, EIO, BUDGET);
    failure_case("check clock zero before I/O", CLOCK_CHECK_ERROR, 0, BUDGET);
    failure_case("pre-I/O expiry", PRE_IO_EXPIRY, 0, BUDGET);
    failure_case("recv EINTR original budget", RECV_INTERRUPTS, EINTR, BUDGET);
    failure_case("poll EINTR original budget", POLL_INTERRUPTS, EINTR, BUDGET);
    failure_case("repeated EAGAIN/spurious readiness", SPURIOUS_READY,
                 EAGAIN, BUDGET);
    failure_case("repeated EWOULDBLOCK", WOULD_BLOCK, EWOULDBLOCK, BUDGET);
    poll_timeout_case();
    failure_case("zero unlimited terminal EIO", RECV_ERROR, EIO, 0);
    failure_case("zero unlimited terminal errno zero", RECV_ERROR, 0, 0);
    failure_case("zero unlimited EAGAIN maps to timeout", RECV_ERROR, EAGAIN, 0);
    late_ready_case();
}

int main(void)
{
    const char *large;

    /* Default: 26 baseline-compatible contracts plus 10 small candidate
     * cases (36 total). CU_TEST_SOCKET_LARGE=1 opts into two more candidate
     * clamp cases (38 total) with 1 GiB + 1 bytes of actual backing.
     * Keep that allocation out of the default MSan gate: poisoning it with
     * origins can eagerly touch more than 3 GiB despite our seven-byte use. */
    contracts();
    success_case("fast warm DATA", 7, BUDGET, 1, 0);
    success_case("fast warm EOF", 0, BUDGET, 1, 0);
    fallback_sequence_case("fast EAGAIN then poll/DATA", EAGAIN, 0, 0);
    fallback_sequence_case("fast EINTR then poll/DATA", EINTR, 0, 0);
    fallback_sequence_case("fast repeated EAGAIN polls before recv",
                           EAGAIN, 1, 0);
    fallback_sequence_case("fast EAGAIN then poll EINTR/retry/DATA",
                           EAGAIN, 0, 1);
    fallback_sequence_case("fast EINTR then poll EINTR/retry/DATA",
                           EINTR, 0, 1);
    fallback_clock_case("fast fallback clock EIO", 0, EIO);
    fallback_clock_case("fast fallback clock errno zero", 0, 0);
    fallback_clock_case("fast fallback expiry", 1, 0);
    large = getenv("CU_TEST_SOCKET_LARGE");
    if (large != NULL && strcmp(large, "1") == 0)
        large_capacity_cases();
    printf("socket read: model checks passed\n");
    return EXIT_SUCCESS;
}
#endif
