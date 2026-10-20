#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdlib.h>
#include "cu.h"
#ifndef ENOSYS
#define ENOSYS EINVAL
#endif

#if defined(__cerb__) || defined(__wasi__) || !defined(__linux__)

struct CuProcessIO { int unused; };
int cu_process_io_start(CuProcessIO **io, CuArena *a, const char *path,
                        char *const argv[], char *const envp[])
{
    (void)envp;
    if (io == NULL || a == NULL || path == NULL || argv == NULL || argv[0] == NULL)
        abort();
    errno = ENOSYS;
    return 0;
}
CuProcess *cu_process_io_child(CuProcessIO *io) { (void)io; abort(); }
int cu_process_io_pump(CuProcessIO *io, CuStr input, CuProcessCapture *c,
                       unsigned long timeout_ms)
{
    (void)io; (void)input; (void)c; (void)timeout_ms; abort();
}
int cu_process_io_close(CuProcessIO *io) { (void)io; abort(); }

#else
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

struct CuProcessIO {
    CuProcess *child;
    int fd[3];
    int used;
    int failed;
    int saved_error;
};

static void io_fail(CuProcessIO *io, int error)
{
    if (!io->failed) {
        io->failed = 1;
        io->saved_error = error;
    }
}

static void close_fd(CuProcessIO *io, int *fd)
{
    int old;

    old = *fd;
    *fd = -1;
    if (old >= 0 && close(old) != 0)
        io_fail(io, errno);
}

static void close_ends(CuProcessIO *io)
{
    int i;

    for (i = 2; i >= 0; i--)
        close_fd(io, &io->fd[i]);
}

int cu_process_io_close(CuProcessIO *io)
{
    if (io == NULL)
        abort();
    io->used = 1;
    close_ends(io);
    if (io->failed) {
        errno = io->saved_error;
        return 0;
    }
    return 1;
}

CuProcess *cu_process_io_child(CuProcessIO *io)
{
    if (io == NULL)
        abort();
    return io->child;
}

/* Own only created FDs, never replace a pre-existing standard stream. */
static int move_fd(int *fd)
{
    int replacement;
    int old;

    if (*fd >= 3)
        return 1;
    replacement = fcntl(*fd, F_DUPFD_CLOEXEC, 3);
    if (replacement < 0)
        return 0;
    old = *fd;
    *fd = replacement;
    return close(old) == 0;
}

static int nonblocking(int fd)
{
    int status_flags;

    status_flags = fcntl(fd, F_GETFL);
    return status_flags >= 0 && fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) == 0;
}

int cu_process_io_start(CuProcessIO **out, CuArena *a, const char *path,
                        char *const argv[], char *const envp[])
{
    CuProcessIO *record;
    CuProcessStdio streams;
    int ends[3][2];
    int i;
    int j;

    if (out == NULL || a == NULL || path == NULL || argv == NULL || argv[0] == NULL)
        abort();
    record = CU_PUSH(a, CuProcessIO, 1);
    if (record == NULL) {
        errno = ENOMEM;
        return 0;
    }
    record->child = NULL;
    record->used = 0;
    record->failed = 0;
    record->saved_error = 0;
    for (i = 0; i < 3; i++) {
        record->fd[i] = -1;
        ends[i][0] = -1;
        ends[i][1] = -1;
    }
    for (i = 0; i < 3; i++) {
        if (pipe2(ends[i], O_CLOEXEC) != 0)
            goto setup_error;
        for (j = 0; j < 2; j++) {
            if (!move_fd(&ends[i][j]))
                goto setup_error;
        }
    }
    if (!nonblocking(ends[0][1]) || !nonblocking(ends[1][0]) ||
        !nonblocking(ends[2][0]))
        goto setup_error;
    streams.input = ends[0][0];
    streams.output = ends[1][1];
    streams.error = ends[2][1];
    if (!cu_process_start_redirected(&record->child, a, path, argv, envp, &streams))
        goto setup_error;
    /* From here onward success means child ownership. Any cleanup failure
     * is an IO failure observed by pump/close, never a lost child handle. */
    record->fd[0] = ends[0][1]; ends[0][1] = -1;
    record->fd[1] = ends[1][0]; ends[1][0] = -1;
    record->fd[2] = ends[2][0]; ends[2][0] = -1;
    for (i = 2; i >= 0; i--) {
        for (j = 1; j >= 0; j--)
            close_fd(record, &ends[i][j]);
    }
    if (record->failed)
        close_ends(record);
    *out = record;
    return 1;
setup_error:
    io_fail(record, errno);
    for (i = 2; i >= 0; i--) {
        for (j = 1; j >= 0; j--)
            close_fd(record, &ends[i][j]);
    }
    errno = record->saved_error;
    return 0;
}

static int io_stamp(unsigned long *sec, unsigned long *nsec)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    *sec = (unsigned long)now.tv_sec;
    *nsec = (unsigned long)now.tv_nsec;
    return 1;
}

/* Unsigned duration subtraction, not signed absolute-deadline addition. */
static int time_left(unsigned long sec, unsigned long nsec, unsigned long ms, int *delay)
{
    unsigned long now_sec;
    unsigned long now_nsec;
    unsigned long elapsed;
    unsigned long left;
    unsigned long used;

    if (ms == 0) {
        *delay = -1;
        return 1;
    }
    if (!io_stamp(&now_sec, &now_nsec))
        return 0;
    elapsed = now_sec - sec;
    if (now_nsec < nsec) {
        elapsed--;
        now_nsec = 1000000000UL - nsec + now_nsec;
    } else {
        now_nsec -= nsec;
    }
    if (elapsed > ms / 1000)
        goto expired;
    left = ms - elapsed * 1000;
    used = now_nsec / 1000000UL;
    if (used >= left)
        goto expired;
    left -= used;
    *delay = left > (unsigned long)INT_MAX ? INT_MAX : (int)left;
    return 1;
expired:
    errno = ETIMEDOUT;
    return 0;
}

/* Suppress only this write's SIGPIPE, never alter a process disposition.
 * A prior pending signal is left alone; concurrent external SIGPIPE can
 * coalesce with the generated one, which the OS cannot distinguish. */
static ssize_t guarded_write(int fd, const char *data, size_t length)
{
    sigset_t set;
    sigset_t old_mask;
    sigset_t pending;
    struct timespec zero;
    ssize_t count;
    int code;
    int prior;
    int saved;
    int received;

    if (sigemptyset(&set) != 0 || sigaddset(&set, SIGPIPE) != 0 ||
        sigemptyset(&pending) != 0)
        return -1;
    code = pthread_sigmask(SIG_BLOCK, &set, &old_mask);
    if (code != 0) {
        errno = code;
        return -1;
    }
    count = -1;
    if (sigpending(&pending) != 0)
        goto restore;
    prior = sigismember(&pending, SIGPIPE);
    if (prior < 0)
        goto restore;
    count = write(fd, data, length);
    saved = errno;
    if (count < 0 && saved == EPIPE && !prior) {
        zero.tv_sec = 0;
        zero.tv_nsec = 0;
        do {
            received = sigtimedwait(&set, NULL, &zero);
        } while (received < 0 && errno == EINTR);
        if (received != SIGPIPE && !(received < 0 && errno == EAGAIN))
            abort();
    }
    if (pthread_sigmask(SIG_SETMASK, &old_mask, NULL) != 0)
        abort();
    errno = saved;
    return count;
restore:
    saved = errno;
    if (pthread_sigmask(SIG_SETMASK, &old_mask, NULL) != 0)
        abort();
    errno = saved;
    return -1;
}

static void read_capture(CuProcessIO *io, int stream, char *buffer,
                         size_t capacity, size_t *length)
{
    char excess;
    size_t amount;
    ssize_t got;

    amount = capacity - *length;
    if (amount > 16384)
        amount = 16384;
    got = read(io->fd[stream], amount == 0 ? &excess : buffer + *length,
               amount == 0 ? (size_t)1 : amount);
    if (got > 0) {
        if (amount == 0)
            io_fail(io, EFBIG);
        else
            *length += (size_t)got;
    } else if (got == 0) {
        close_fd(io, &io->fd[stream]);
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        io_fail(io, errno);
    }
}

int cu_process_io_pump(CuProcessIO *io, CuStr input, CuProcessCapture *c,
                       unsigned long timeout_ms)
{
    struct pollfd fds[3];
    unsigned long sec;
    unsigned long nsec;
    size_t amount;
    ssize_t sent;
    int delay;
    int polled;
    int turn;
    int step;
    int i;

    if (io == NULL || c == NULL || io->used ||
        (input.len != 0 && input.ptr == NULL) ||
        (c->out_cap != 0 && c->out == NULL) || (c->err_cap != 0 && c->err == NULL))
        abort();
    io->used = 1;
    c->out_len = 0;
    c->err_len = 0;
    c->input_sent = 0;
    sec = 0;
    nsec = 0;
    if (!io->failed && timeout_ms != 0 && !io_stamp(&sec, &nsec))
        io_fail(io, errno);
    if (!io->failed && input.len == 0)
        close_fd(io, &io->fd[0]);
    turn = 0;
    while (!io->failed && (io->fd[0] >= 0 || io->fd[1] >= 0 || io->fd[2] >= 0)) {
        if (!time_left(sec, nsec, timeout_ms, &delay)) {
            io_fail(io, errno);
            break;
        }
        for (i = 0; i < 3; i++) {
            fds[i].fd = io->fd[i];
            fds[i].events = i == 0 ? POLLOUT : POLLIN;
            fds[i].revents = 0;
        }
        polled = poll(fds, (nfds_t)3, delay);
        if (polled < 0) {
            if (errno != EINTR)
                io_fail(io, errno);
            continue;
        }
        for (step = 0; step < 3 && !io->failed; step++) {
            i = (turn + step) % 3;
            if (io->fd[i] < 0 || fds[i].revents == 0)
                continue;
            if (!time_left(sec, nsec, timeout_ms, &delay)) {
                io_fail(io, errno);
                break;
            }
            if ((fds[i].revents & POLLNVAL) != 0) {
                io_fail(io, EBADF);
            } else if (i == 0) {
                amount = input.len - c->input_sent;
                if (amount > 16384)
                    amount = 16384;
                sent = guarded_write(io->fd[0], input.ptr + c->input_sent, amount);
                if (sent > 0) {
                    c->input_sent += (size_t)sent;
                    if (c->input_sent == input.len)
                        close_fd(io, &io->fd[0]);
                } else if (sent == 0) {
                    io_fail(io, EIO);
                } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    io_fail(io, errno);
                }
            } else if (i == 1) {
                read_capture(io, i, c->out, c->out_cap, &c->out_len);
            } else {
                read_capture(io, i, c->err, c->err_cap, &c->err_len);
            }
        }
        turn = (turn + 1) % 3;
    }
    if (!io->failed && !time_left(sec, nsec, timeout_ms, &delay))
        io_fail(io, errno);
    return cu_process_io_close(io);
}
#endif
