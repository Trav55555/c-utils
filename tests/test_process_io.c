#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

#if defined(__cerb__) || defined(__wasi__) || !defined(__linux__)
int main(void)
{
    CuArena a;
    CuProcessIO *io;
    void *sentinel;
    char name[] = "program";
    char *args[2];

    cu_arena_init(&a);
    sentinel = &a;
    io = sentinel;
    args[0] = name;
    args[1] = NULL;
    errno = 0;
    assert(!cu_process_io_start(&io, &a, name, args, NULL));
#ifdef ENOSYS
    assert(errno == ENOSYS);
#else
    assert(errno == EINVAL);
#endif
    assert(io == sentinel);
    cu_arena_release(&a);
    return 0;
}
#else
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define OUT_N ((size_t)131089)
#define ERR_N ((size_t)147487)
static size_t input_n;
#define INPUT_N input_n
static char marker[] = "--io-child";
static volatile sig_atomic_t caught;

static void handler(int sig)
{
    (void)sig;
    ++caught;
}

static void emit(int fd, size_t count, unsigned int mul, unsigned int add)
{
    unsigned char b[4096];
    size_t off;
    size_t i;
    size_t n;
    ssize_t r;

    off = 0;
    while (off < count) {
        n = count - off;
        if (n > sizeof(b)) n = sizeof(b);
        for (i = 0; i < n; ++i)
            b[i] = (unsigned char)(((off + i) * mul + add) % 256);
        i = 0;
        while (i < n) {
            r = write(fd, b + i, n - i);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) _exit(81);
            i += (size_t)r;
        }
        off += n;
    }
}

static int child(const char *mode)
{
    unsigned char b[4096];
    size_t total;
    size_t i;
    size_t expected;
    ssize_t n;
    int flags;
    int fd;
    int capacity;

    (void)alarm(15);
    for (fd = 0; fd < 3; ++fd) {
        flags = fcntl(fd, F_GETFL);
        if (flags < 0 || (flags & O_NONBLOCK) != 0) return 82;
    }
    capacity = fcntl(0, F_GETPIPE_SZ);
    if (capacity <= 0 || (size_t)capacity >= INPUT_N) return 88;
    for (fd = 1; fd < 3; fd++) {
        capacity = fcntl(fd, F_SETPIPE_SZ, 4096);
        if (capacity <= 0 || (size_t)capacity >= (fd == 1 ? OUT_N : ERR_N)) return 89;
    }
    if (strcmp(mode, "silent") == 0) {
        for (;;) pause();
    }
    if (strcmp(mode, "live") == 0) {
        for (fd = 0; fd < 3; ++fd) (void)close(fd);
        for (;;) pause();
    }
    if (strcmp(mode, "epipe") == 0) {
        (void)close(0);
        /* EOF on both outputs proves stdin was closed before pumping ends. */
        (void)close(1);
        (void)close(2);
        for (;;) pause();
    }
    if (strcmp(mode, "bulk") == 0) {
        /* Neither output can fit in a pipe; both precede any input read. */
        emit(1, OUT_N, 17, 3);
        emit(2, ERR_N, 29, 197);
        expected = INPUT_N;
    } else if (strcmp(mode, "err") == 0) {
        emit(2, ERR_N, 29, 197);
        expected = 0;
    } else if (strcmp(mode, "empty") == 0) {
        expected = 0;
    } else return 83;
    total = 0;
    for (;;) {
        n = read(0, b, sizeof(b));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return 84;
        if (n == 0) break;
        for (i = 0; i < (size_t)n; ++i)
            if (b[i] != (unsigned char)(((total + i) * 43 + 91) % 256))
                return 85;
        total += (size_t)n;
        if (total > expected) return 86;
    }
    return total == expected ? 23 : 87;
}

static CuProcessIO *start(CuArena *a, const char *path, char *mode)
{
    CuProcessIO *io;
    char *args[4];
    char *env[1];

    args[0] = (char *)path;
    args[1] = marker;
    args[2] = mode;
    args[3] = NULL;
    env[0] = NULL;
    io = NULL;
    assert(cu_process_io_start(&io, a, path, args, env));
    assert(io != NULL);
    assert(cu_process_io_child(io) != NULL);
    return io;
}

static void finish(CuProcessIO *io, int natural)
{
    CuProcess *p;
    CuProcessExit status;
    CuProcessWait w;
    int ok;

    p = cu_process_io_child(io);
    w = cu_process_wait(p, natural ? 5000 : 0, &status);
    if (w == CU_PROCESS_RUNNING) {
        ok = cu_process_signal(p, CU_PROCESS_KILL);
        assert(ok);
        w = cu_process_wait(p, 5000, &status);
    }
    assert(w == CU_PROCESS_DONE);
    if (natural) assert(status.kind == CU_PROCESS_EXITED && status.code == 23);
}

static void prefix(const char *p, size_t n, int which)
{
    size_t i;
    /* Independent oracle, intentionally not shared with emit(). */
    for (i = 0; i < n; ++i) {
        if (which == 1)
            assert((unsigned char)p[i] == (unsigned char)((i * 17 + 3) & 255));
        else
            assert((unsigned char)p[i] == (unsigned char)((i * 29 + 197) & 255));
    }
}

static void capture_case(const char *path, char *mode, size_t oc, size_t ec,
                         int expected_error)
{
    CuArena a;
    CuProcessIO *io;
    CuProcessCapture c;
    char *input;
    size_t in;
    size_t i;
    int ok;
    int saved;
    int close_ok;
    int close_error;

    in = strcmp(mode, "bulk") == 0 ? INPUT_N : 0;
    input = in ? (char *)malloc(in) : NULL;
    assert(!in || input != NULL);
    for (i = 0; i < in; ++i) input[i] = (char)((i * 43 + 91) & 255);
    c.out = oc ? (char *)malloc(oc) : NULL;
    c.err = ec ? (char *)malloc(ec) : NULL;
    assert(!oc || c.out != NULL);
    assert(!ec || c.err != NULL);
    c.out_cap = oc;
    c.err_cap = ec;
    c.out_len = c.err_len = c.input_sent = (size_t)999999;
    cu_arena_init(&a);
    io = start(&a, path, mode);
    ok = cu_process_io_pump(io, cu_str_n(input, in), &c, 5000);
    saved = errno;
    close_ok = cu_process_io_close(io);
    close_error = errno;
    finish(io, expected_error == 0);
    assert(ok == (expected_error == 0));
    if (expected_error) {
        assert(saved == expected_error);
        assert(!close_ok && close_error == saved);
        errno = 123;
        assert(!cu_process_io_close(io) && errno == saved);
    } else {
        assert(close_ok);
        assert(cu_process_io_close(io));
    }
    assert(c.out_len <= oc && c.err_len <= ec && c.input_sent <= in);
    prefix(c.out, c.out_len, 1);
    prefix(c.err, c.err_len, 2);
    if (!expected_error) {
        assert(c.input_sent == in);
        assert(c.out_len == (in ? OUT_N : 0));
        assert(c.err_len == (strcmp(mode, "empty") == 0 ? 0 : ERR_N));
    } else if (oc < OUT_N && in) {
        assert(c.out_len == oc);
    } else {
        assert(c.err_len == ec);
    }
    cu_arena_release(&a);
    free(input);
    free(c.out);
    free(c.err);
}

static void quiet_case(const char *path, char *mode, int interrupt)
{
    CuArena a;
    CuProcessIO *io;
    CuProcessCapture c = { NULL, 0, 0, NULL, 0, 0, 0 };
    CuProcessExit status;
    struct sigaction sa;
    int ok;
    int saved;
    int closed;

    cu_arena_init(&a);
    io = start(&a, path, mode);
    sa.sa_flags = 0;
    sa.sa_handler = handler;
    assert(sigemptyset(&sa.sa_mask) == 0);
    if (interrupt) {
        assert(sigaction(SIGALRM, &sa, NULL) == 0);
        (void)alarm(1);
    }
    ok = cu_process_io_pump(io, cu_str_empty(), &c, interrupt ? 1500 : 100);
    saved = errno;
    if (interrupt) (void)alarm(0);
    closed = cu_process_io_close(io);
    assert(cu_process_wait(cu_process_io_child(io), 0, &status) == CU_PROCESS_RUNNING);
    finish(io, 0);
    assert(c.out_len == 0 && c.err_len == 0 && c.input_sent == 0);
    if (strcmp(mode, "live") == 0) assert(ok && closed);
    else assert(!ok && saved == ETIMEDOUT && !closed);
    if (interrupt) assert(caught > 0);
    cu_arena_release(&a);
}

static void signal_case(const char *path, int which)
{
    CuArena a;
    CuProcessIO *io;
    CuProcessCapture c = { NULL, 0, 0, NULL, 0, 0, 0 };
    struct sigaction sa;
    struct sigaction after;
    struct sigaction before;
    sigset_t baseline;
    sigset_t set;
    sigset_t old;
    sigset_t now;
    struct timespec zero;
    char *input;
    size_t j;
    int ok;
    int saved;
    int sig;
    int i;

    sa.sa_flags = 0;
    sa.sa_handler = which == 0 ? SIG_DFL : which == 1 ? SIG_IGN : handler;
    assert(sigemptyset(&sa.sa_mask) == 0);
    assert(sigaction(SIGPIPE, &sa, NULL) == 0);
    assert(sigemptyset(&set) == 0);
    assert(sigaddset(&set, SIGPIPE) == 0);
    assert(sigprocmask(SIG_UNBLOCK, &set, &old) == 0);
    if (which == 3) {
        assert(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
        assert(raise(SIGPIPE) == 0);
    }
    assert(sigaction(SIGPIPE, NULL, &before) == 0);
    assert(sigprocmask(SIG_BLOCK, NULL, &baseline) == 0);
    input = (char *)malloc(INPUT_N);
    assert(input != NULL);
    for (j = 0; j < INPUT_N; j++)
        input[j] = '\0';
    cu_arena_init(&a);
    io = start(&a, path, "epipe");
    ok = cu_process_io_pump(io, cu_str_n(input, INPUT_N), &c, 5000);
    saved = errno;
    assert(!cu_process_io_close(io));
    finish(io, 0);
    assert(!ok && saved == EPIPE);
    assert(c.input_sent < INPUT_N && c.out_len == 0 && c.err_len == 0);
    assert(sigaction(SIGPIPE, NULL, &after) == 0);
    assert(after.sa_handler == before.sa_handler);
    assert(after.sa_flags == before.sa_flags);
    for (i = 1; i <= SIGRTMAX; ++i)
        assert(sigismember(&after.sa_mask, i) == sigismember(&before.sa_mask, i));
    assert(caught == 0);
    assert(sigprocmask(SIG_BLOCK, NULL, &now) == 0);
    for (i = 1; i <= SIGRTMAX; ++i)
        assert(sigismember(&now, i) == sigismember(&baseline, i));
    assert(sigismember(&now, SIGPIPE) == (which == 3));
    assert(sigpending(&now) == 0);
    assert(sigismember(&now, SIGPIPE) == (which == 3));
    if (which == 3) {
        zero.tv_sec = 0;
        zero.tv_nsec = 0;
        sig = sigtimedwait(&set, NULL, &zero);
        assert(sig == SIGPIPE);
        assert(sigpending(&now) == 0 && !sigismember(&now, SIGPIPE));
    }
    assert(sigprocmask(SIG_SETMASK, &old, NULL) == 0);
    cu_arena_release(&a);
    free(input);
}

static void isolated(const char *path, int which)
{
    pid_t pid;
    pid_t got;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        (void)alarm(25);
        caught = 0;
        if (which < 4) signal_case(path, which);
        else if (which == 4) {
            assert(close(0) == 0);
            capture_case(path, "empty", 0, 0, 0);
            errno = 0;
            assert(fcntl(0, F_GETFD) == -1 && errno == EBADF);
        } else quiet_case(path, "silent", 1);
        _exit(0);
    }
    do { got = waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
    assert(got == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void allocation_failures(const char *path)
{
    CuArena arena;
    CuProcessIO *io;
    void *sentinel;
    char *args[4];
    size_t fail_at;
    int started;
    int saved;
    int closed;

    args[0] = (char *)path;
    args[1] = marker;
    args[2] = "empty";
    args[3] = NULL;
    for (fail_at = 0; fail_at < 2; fail_at++) {
        cu_arena_init_with(&arena, 1, 1);
        cu_arena_fail_at(&arena, fail_at);
        sentinel = &arena;
        io = sentinel;
        started = cu_process_io_start(&io, &arena, path, args, NULL);
        saved = errno;
        if (started) {
            closed = cu_process_io_close(io);
            (void)closed;
            finish(io, 0);
        }
        assert(!started && io == sentinel && saved == ENOMEM);
        cu_arena_release(&arena);
    }
}

typedef struct IOJob {
    const char *path;
    int complete;
} IOJob;

static void io_job(void *context)
{
    IOJob *job;
    sigset_t blocked;
    sigset_t saved;
    sigset_t observed;

    job = (IOJob *)context;
    assert(sigemptyset(&blocked) == 0 && sigaddset(&blocked, SIGUSR1) == 0);
    assert(pthread_sigmask(SIG_BLOCK, &blocked, &saved) == 0);
    capture_case(job->path, "bulk", OUT_N, ERR_N, 0);
    assert(pthread_sigmask(SIG_SETMASK, NULL, &observed) == 0);
    assert(sigismember(&observed, SIGUSR1) == 1);
    assert(pthread_sigmask(SIG_SETMASK, &saved, NULL) == 0);
    job->complete = 1;
}

static void concurrent(const char *path)
{
    CuArena arena;
    IOJob *jobs;
    CuThread *threads[2];
    size_t i;

    cu_arena_init(&arena);
    jobs = CU_PUSH(&arena, IOJob, 2);
    assert(jobs != NULL);
    for (i = 0; i < 2; i++) {
        jobs[i].path = path;
        jobs[i].complete = 0;
        assert(cu_thread_start(&threads[i], &arena, 0, io_job, &jobs[i]));
    }
    for (i = 0; i < 2; i++) {
        cu_thread_join(threads[i]);
        assert(jobs[i].complete);
    }
    cu_arena_release(&arena);
}

int main(int argc, char **argv)
{
    int i;
    long page;

    page = sysconf(_SC_PAGESIZE);
    assert(page > 0 && (unsigned long)page <= ((size_t)-1 - 33) / 32);
    input_n = (size_t)page * 32 + 33;
    if (input_n < 262177) input_n = 262177;
    if (argc == 3 && strcmp(argv[1], marker) == 0) return child(argv[2]);
    assert(argc == 1);
    (void)alarm(120);
    capture_case(argv[0], "bulk", OUT_N, ERR_N, 0);
    capture_case(argv[0], "empty", 0, 0, 0);
    capture_case(argv[0], "bulk", OUT_N - 1, ERR_N, EFBIG);
    capture_case(argv[0], "bulk", 0, ERR_N, EFBIG);
    capture_case(argv[0], "bulk", OUT_N, ERR_N - 1, EFBIG);
    capture_case(argv[0], "err", 0, ERR_N, 0);
    capture_case(argv[0], "err", 0, ERR_N - 1, EFBIG);
    capture_case(argv[0], "err", 0, 0, EFBIG);
    quiet_case(argv[0], "silent", 0);
    quiet_case(argv[0], "live", 0);
    for (i = 0; i < 6; ++i) isolated(argv[0], i);
    allocation_failures(argv[0]);
    concurrent(argv[0]);
    (void)alarm(0);
    return 0;
}
#endif
