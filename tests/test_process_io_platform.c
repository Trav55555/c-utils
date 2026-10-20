#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__cerb__) || defined(__wasi__) || !defined(__linux__)
int main(void) { return 0; }
#else
#include "death.h"
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#define cu_process_io_start boundary_start
#define cu_process_io_child boundary_child
#define cu_process_io_pump boundary_pump
#define cu_process_io_close boundary_close
#include "cu.h"

/* Only model descriptor identities are passed to these replacements. */
static int live_fd[3], close_count[3], read_count[3];
static int fault, fault_errno, close_errno, poll_count, clock_count;
static int write_count, accepted, block_count, restore_count, drain_count;
static int prior_pending, drain_mode, mask_error, pending_error, pending_bad, restore_error;
static int transient_io, output_size, poll_interrupt, invalid_fd, guard_active;
static unsigned long virtual_ms, tick_ms;
static sigset_t model_mask;
static CuArena child_token;
enum { READ_BAD = 1, WRITE_BAD, POLL_BAD, CLOCK_BAD };

static int slot(int fd)
{
    assert(fd >= 20 && fd <= 22);
    assert(live_fd[fd - 20]);
    return fd - 20;
}
static int model_close(int fd)
{
    int index;
    index = slot(fd);
    live_fd[index] = 0;
    close_count[index]++;
    errno = close_errno;
    return close_errno == ERANGE ? 0 : -1;
}
static ssize_t model_read(int fd, void *buffer, size_t amount)
{
    int index, call;
    size_t remaining, count;
    index = slot(fd);
    assert(index != 0 && amount > 0 && amount <= 16384);
    call = read_count[index]++;
    if (fault == READ_BAD) { errno = fault_errno; return -1; }
    if (transient_io && call < 2) {
        errno = call == 0 ? EINTR : EAGAIN; return -1;
    }
    if (index == 2) return 0;
    remaining = (size_t)output_size;
    if (remaining == 0) return 0;
    count = amount < remaining ? amount : remaining;
    if (count > 2) count = 2;
    memset(buffer, 'x', count);
    output_size -= (int)count;
    return (ssize_t)count;
}
static ssize_t model_write(int fd, const void *buffer, size_t amount)
{
    int call;
    size_t count;
    assert(slot(fd) == 0 && buffer != NULL && amount > 0 && amount <= 16384);
    assert(sigismember(&model_mask, SIGPIPE) == 1);
    call = write_count++;
    if (fault == WRITE_BAD) { errno = fault_errno; return -1; }
    if (transient_io && call < 2) {
        errno = call == 0 ? EAGAIN : EINTR; return -1;
    }
    count = amount > 2 ? 2 : amount;
    accepted += (int)count;
    errno = EDOM;
    return (ssize_t)count;
}
static int model_poll(struct pollfd *fds, nfds_t count, int delay)
{
    int index;
    if (guard_active) _exit(95);
    assert(count == 3 && ++poll_count < 100);
    assert(delay == -1 || delay > 0);
    virtual_ms += tick_ms;
    if (fault == POLL_BAD) { errno = fault_errno; return -1; }
    if (poll_interrupt && poll_count == 1) { errno = EINTR; return -1; }
    for (index = 0; index < 3; index++) {
        if (fds[index].fd < 0) continue;
        assert(slot(fds[index].fd) == index);
        fds[index].revents = invalid_fd ? POLLNVAL :
            (index == 0 ? POLLOUT : (POLLIN | POLLHUP));
    }
    return 3;
}
static int model_clock(clockid_t id, struct timespec *stamp)
{
    assert(id == CLOCK_MONOTONIC);
    clock_count++;
    if (fault == CLOCK_BAD) { errno = fault_errno; return -1; }
    stamp->tv_sec = (time_t)(virtual_ms / 1000UL);
    stamp->tv_nsec = (long)((virtual_ms % 1000UL) * 1000000UL);
    return 0;
}
static int model_mask_call(int how, const sigset_t *set, sigset_t *old)
{
    if (how == SIG_BLOCK) {
        block_count++;
        assert(sigismember(set, SIGPIPE) == 1 && old != NULL);
        if (mask_error) return mask_error;
        *old = model_mask;
        assert(sigaddset(&model_mask, SIGPIPE) == 0);
    } else {
        assert(how == SIG_SETMASK && old == NULL);
        restore_count++;
        if (restore_error) return restore_error;
        model_mask = *set;
    }
    errno = ERANGE;
    return 0;
}
static int model_pending(sigset_t *set)
{
    if (pending_error || pending_bad) { errno = pending_error; return -1; }
    assert(sigemptyset(set) == 0);
    if (prior_pending) assert(sigaddset(set, SIGPIPE) == 0);
    return 0;
}
static int model_wait(const sigset_t *set, siginfo_t *info,
                      const struct timespec *duration)
{
    assert(sigismember(set, SIGPIPE) == 1 && info == NULL);
    assert(duration->tv_sec == 0 && duration->tv_nsec == 0);
    assert(++drain_count < 4 && !prior_pending);
    if (drain_mode == 1 && drain_count == 1) { errno = EINTR; return -1; }
    if (drain_mode == 2) { errno = EAGAIN; return -1; }
    if (drain_mode == 3) { errno = EINVAL; return -1; }
    return SIGPIPE;
}
#define read model_read
#define write model_write
#define close model_close
#define poll model_poll
#define clock_gettime model_clock
#define pthread_sigmask model_mask_call
#define sigpending model_pending
#define sigtimedwait model_wait
#include "../src/cu_process_io.c"
#undef read
#undef write
#undef close
#undef poll
#undef clock_gettime
#undef pthread_sigmask
#undef sigpending
#undef sigtimedwait

static CuProcessIO bundle;
static CuProcessCapture capture;
static char output[8], error_output[8];
static void reset(void)
{
    int index;
    for (index = 0; index < 3; index++) {
        live_fd[index] = 1; close_count[index] = read_count[index] = 0;
        bundle.fd[index] = 20 + index;
    }
    bundle.child = (void *)&child_token;
    bundle.used = bundle.failed = bundle.saved_error = 0;
    capture.out = output; capture.out_cap = sizeof(output);
    capture.err = error_output; capture.err_cap = sizeof(error_output);
    capture.out_len = capture.err_len = capture.input_sent = 99;
    fault = poll_count = clock_count = write_count = accepted = 0;
    block_count = restore_count = drain_count = 0;
    prior_pending = drain_mode = mask_error = pending_error = pending_bad = restore_error = 0;
    transient_io = output_size = poll_interrupt = invalid_fd = guard_active = 0;
    fault_errno = EIO; close_errno = ERANGE;
    virtual_ms = tick_ms = 0;
    assert(sigemptyset(&model_mask) == 0);
    assert(sigaddset(&model_mask, SIGUSR1) == 0);
}
static void run(size_t length, unsigned long budget, int expected, int error)
{
    int result, saved, index, before;
    result = boundary_pump(&bundle, cu_str_n("abcde", length), &capture, budget);
    saved = errno;
    assert(result == expected);
    if (!expected) assert(saved == error);
    assert(capture.input_sent == (size_t)accepted && capture.input_sent <= length);
    assert(boundary_child(&bundle) == (void *)&child_token);
    assert(sigismember(&model_mask, SIGPIPE) == 0);
    assert(sigismember(&model_mask, SIGUSR1) == 1);
    for (index = 0; index < 3; index++) assert(close_count[index] == 1);
    before = poll_count;
    result = boundary_close(&bundle);
    assert(result == expected && poll_count == before);
    if (!expected) assert(errno == error);
    for (index = 0; index < 3; index++) assert(close_count[index] == 1);
}
static int misuse;
static void death_case(void)
{
    int result;
    CuStr input;
    reset(); input = cu_str_n(NULL, 0);
    if (misuse <= 6) guard_active = 1;
    switch (misuse) {
    case 0: result = boundary_close(NULL); break;
    case 1: (void)boundary_child(NULL); return;
    case 2: result = boundary_pump(NULL, input, &capture, 0); break;
    case 3: result = boundary_pump(&bundle, input, NULL, 0); break;
    case 4: capture.out = NULL; result = boundary_pump(&bundle, input, &capture, 0); break;
    case 5: capture.err = NULL; result = boundary_pump(&bundle, input, &capture, 0); break;
    case 6: input.ptr = NULL; input.len = 1; result = boundary_pump(&bundle, input, &capture, 0); break;
    case 7: result = boundary_close(&bundle);
        if (!result) _exit(96);
        guard_active = 1; result = boundary_pump(&bundle, input, &capture, 0); break;
    case 8: result = boundary_pump(&bundle, input, &capture, 0);
        if (!result) _exit(96);
        guard_active = 1; result = boundary_pump(&bundle, input, &capture, 0); break;
    case 9: restore_error = EINVAL;
        result = boundary_pump(&bundle, cu_str_n("a", 1), &capture, 0); break;
    default: fault = WRITE_BAD; fault_errno = EPIPE; drain_mode = 3;
        result = boundary_pump(&bundle, cu_str_n("a", 1), &capture, 0); break;
    }
    (void)result;
}
int main(void)
{
    int kind, zero, mode, delay, result;
    reset(); transient_io = 1; output_size = 5; run(5, 0, 1, 0);
    assert(clock_count == 0 && capture.out_len == 5);
    assert(memcmp(output, "xxxxx", 5) == 0 && capture.err_len == 0);
    reset(); output_size = 8; run(0, 0, 1, 0);
    assert(capture.out_len == 8 && read_count[1] == 5);
    reset(); output_size = 9; run(0, 0, 0, EFBIG); assert(capture.out_len == 8);
    reset(); capture.out = NULL; capture.out_cap = 0; run(0, 0, 1, 0);
    reset(); capture.out = NULL; capture.out_cap = 0; output_size = 1;
    run(0, 0, 0, EFBIG); assert(capture.out_len == 0);
    reset(); invalid_fd = 1; run(5, 0, 0, EBADF);
    for (kind = READ_BAD; kind <= CLOCK_BAD; kind++) {
        for (zero = 0; zero < 2; zero++) {
            reset(); fault = kind; fault_errno = zero ? 0 : EIO;
            close_errno = EINTR;
            run(5, kind == CLOCK_BAD ? 10UL : 0UL, 0, fault_errno);
        }
    }
    reset(); close_errno = 0; run(0, 0, 0, 0);
    reset(); close_errno = EINTR; run(0, 0, 0, EINTR);
    reset(); poll_interrupt = 1; tick_ms = 3; output_size = 8;
    run(5, 10, 0, ETIMEDOUT);
    assert(virtual_ms == 12 && poll_count == 4 && clock_count > 1);
    reset(); virtual_ms = 1250;
    assert(time_left(0, 500000000UL, 1000, &delay) && delay == 250);
    virtual_ms = 1250;
    assert(time_left(ULONG_MAX, 250000000UL, 3000, &delay) && delay == 1000);
    virtual_ms = 0;
    assert(time_left(0, 0, ULONG_MAX, &delay) && delay == INT_MAX);
    assert(time_left(0, 0, (unsigned long)INT_MAX, &delay) && delay == INT_MAX);
    assert(time_left(0, 0, (unsigned long)INT_MAX + 1UL, &delay) && delay == INT_MAX);
    assert(time_left(0, 0, 0, &delay) && delay == -1);
    for (mode = 0; mode < 3; mode++) {
        reset(); fault = WRITE_BAD; fault_errno = EPIPE; drain_mode = mode;
        run(5, 0, 0, EPIPE); assert(drain_count == (mode == 1 ? 2 : 1));
        assert(block_count == 1 && restore_count == 1);
    }
    reset(); fault = WRITE_BAD; fault_errno = EPIPE; prior_pending = 1;
    run(5, 0, 0, EPIPE); assert(drain_count == 0);
    reset(); mask_error = EINVAL; run(5, 0, 0, EINVAL);
    assert(write_count == 0 && restore_count == 0);
    reset(); pending_error = EIO; run(5, 0, 0, EIO);
    assert(write_count == 0 && restore_count == 1);
    reset(); pending_bad = 1; run(5, 0, 0, 0);
    assert(write_count == 0 && restore_count == 1);
    reset(); result = boundary_close(&bundle); assert(result);
    result = boundary_close(&bundle); assert(result);
    for (mode = 0; mode < 3; mode++) assert(close_count[mode] == 1);
    for (misuse = 0; misuse < 11; misuse++) expect_abort(death_case);
    return 0;
}
#endif
