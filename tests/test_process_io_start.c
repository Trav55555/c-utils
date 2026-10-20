#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#if !defined(__linux__) || defined(__cerb__) || defined(__wasi__)
int main(void) { return 0; }
#else
#include <fcntl.h>
#include <stdarg.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include "death.h"

#define cu_process_io_start ctor_start_probe
#define cu_process_io_child ctor_child_probe
#define cu_process_io_pump ctor_pump_probe
#define cu_process_io_close ctor_close_probe
#include "cu.h"

/* Descriptor identities are entirely synthetic. Even 0/1/2 here never name
 * native standard streams. A close failure consumes its identity once. */
static int live[128], cloexec[128], flags[128], endpoint[6];
static int pipes, dups, flag_reads, sets, spawns, closes, created;
static int kind, ordinal, error_value, low, close_fault, close_error;
static int preclose_at, precloses, guard_active;
static CuArena child_token, output_token;
enum { PIPE_FAIL = 1, DUP_FAIL, GET_FAIL, SET_FAIL, SPAWN_FAIL };

static int fails(int k, int n)
{
    if (kind != k || ordinal != n) return 0;
    errno = error_value;
    return 1;
}

static int pipe_probe(int pair[2], int options)
{
    int i, slot, fd;
    if (guard_active) _exit(95);
    assert(options == O_CLOEXEC && spawns == 0);
    pipes++;
    if (fails(PIPE_FAIL, pipes)) return -1;
    for (i = 0; i < 2; i++) {
        slot = 2 * (pipes - 1) + i;
        fd = low && slot < 3 ? slot : 20 + slot;
        assert(!live[fd]);
        live[fd] = cloexec[fd] = 1;
        flags[fd] = O_APPEND; /* Unrelated status bits must survive SETFL. */
        endpoint[slot] = pair[i] = fd;
        created++;
    }
    return 0;
}

static int fcntl_probe(int fd, int command, ...)
{
    va_list ap;
    int arg, n, i;
    assert(fd >= 0 && fd < 128 && live[fd] && spawns == 0);
    if (command == F_GETFL) {
        flag_reads++;
        assert(fd == endpoint[1] || fd == endpoint[2] || fd == endpoint[4]);
        if (fails(GET_FAIL, flag_reads)) return -1;
        return flags[fd];
    }
    va_start(ap, command);
    arg = va_arg(ap, int);
    va_end(ap);
    if (command == F_SETFL) {
        sets++;
        assert(fd == endpoint[1] || fd == endpoint[2] || fd == endpoint[4]);
        assert(arg == (O_APPEND | O_NONBLOCK));
        if (fails(SET_FAIL, sets)) return -1;
        flags[fd] = arg;
        return 0;
    }
    assert(command == F_DUPFD_CLOEXEC && fd < 3 && arg >= 3);
    dups++;
    if (fails(DUP_FAIL, dups)) return -1;
    n = 60 + dups;
    assert(n >= arg && !live[n]);
    live[n] = cloexec[n] = 1;
    flags[n] = flags[fd];
    created++;
    for (i = 0; i < 6; i++) if (endpoint[i] == fd) endpoint[i] = n;
    return n;
}

static int close_probe(int fd)
{
    assert(fd >= 0 && fd < 128 && live[fd]);
    live[fd] = 0;
    closes++;
    if (!spawns && fd < 3 && ++precloses == preclose_at) {
        errno = error_value;
        return -1;
    }
    if (close_fault && spawns &&
        (fd == endpoint[0] || fd == endpoint[3] || fd == endpoint[5])) {
        close_fault = 0;
        errno = close_error;
        return -1;
    }
    /* Cleanup must not overwrite an earlier error, including errno zero. */
    errno = ERANGE;
    return 0;
}

static int spawn_probe(CuProcess **out, CuArena *a, const char *path,
                       char *const argv[], char *const envp[],
                       const CuProcessStdio *stdio)
{
    int i, fd;
    assert(a != NULL && path != NULL && argv != NULL && argv[0] != NULL);
    assert(envp == NULL && pipes == 3 && flag_reads == 3 && sets == 3);
    assert(stdio->input == endpoint[0]);
    assert(stdio->output == endpoint[3]);
    assert(stdio->error == endpoint[5]);
    for (i = 0; i < 6; i++) {
        fd = endpoint[i];
        assert(fd >= 3 && live[fd] && cloexec[fd]);
        assert((flags[fd] & O_NONBLOCK) ==
               ((i == 1 || i == 2 || i == 4) ? O_NONBLOCK : 0));
    }
    spawns++;
    if (fails(SPAWN_FAIL, spawns)) return 0;
    *out = (void *)&child_token;
    return 1;
}

static int poll_probe(struct pollfd *fds, nfds_t count, int timeout)
{
    (void)fds; (void)count; (void)timeout;
    abort(); /* A latched constructor failure must never enter the poll loop. */
    return -1;
}

#define pipe2 pipe_probe
#define fcntl fcntl_probe
#define close close_probe
#define poll poll_probe
#define cu_process_start_redirected spawn_probe
#include "../src/cu_process_io.c"
#undef pipe2
#undef fcntl
#undef close
#undef poll
#undef cu_process_start_redirected

static void reset(void)
{
    int i;
    for (i = 0; i < 128; i++) {
        live[i] = 0;
        cloexec[i] = 0;
        flags[i] = 0;
    }
    for (i = 0; i < 6; i++) endpoint[i] = -1;
    pipes = dups = flag_reads = sets = spawns = closes = created = 0;
    kind = ordinal = close_fault = preclose_at = precloses = guard_active = 0;
    low = 1;
    error_value = EIO;
    close_error = EINTR;
}

static void run(int refusal, int pump_first)
{
    CuArena arena;
    CuProcessIO *io, *sentinel;
    CuProcessCapture capture;
    CuStr input;
    char name[] = "never-executed";
    char *args[2];
    int ok, saved, i, before, expected;

    cu_arena_init(&arena);
    if (refusal) cu_arena_fail_at(&arena, 0);
    sentinel = (void *)&output_token;
    io = sentinel;
    args[0] = name; args[1] = NULL;
    expected = close_fault;
    errno = EDOM;
    ok = ctor_start_probe(&io, &arena, name, args, NULL);
    saved = errno;
    if (refusal || kind || preclose_at) {
        assert(!ok && io == sentinel);
        assert(saved == (refusal ? ENOMEM : error_value));
        assert(spawns == (kind == SPAWN_FAIL ? 1 : 0));
    } else {
        assert(ok && io != sentinel && spawns == 1);
        assert(ctor_child_probe(io) == (void *)&child_token);
        if (pump_first) {
            /* Only post-spawn-failure cases call pump, never real OS IO. */
            capture.out = NULL;
            capture.err = NULL;
            capture.out_cap = capture.err_cap = 0;
            capture.out_len = capture.err_len = capture.input_sent = 99;
            input = cu_str_n(NULL, 0);
            errno = EDOM;
            ok = ctor_pump_probe(io, input, &capture, 1);
            saved = errno;
            assert(!ok && saved == close_error);
            assert(capture.out_len == 0 && capture.err_len == 0 && capture.input_sent == 0);
        }
        errno = EDOM;
        ok = ctor_close_probe(io);
        saved = errno;
        if (pump_first || expected) assert(!ok && saved == close_error);
        else assert(ok);
        before = closes;
        errno = EDOM;
        ok = ctor_close_probe(io);
        saved = errno;
        assert(closes == before);
        if (pump_first || expected) assert(!ok && saved == close_error);
        else assert(ok);
        assert(ctor_child_probe(io) == (void *)&child_token);
    }
    for (i = 0; i < 128; i++) assert(!live[i]);
    assert(closes == created);
    cu_arena_release(&arena);
}

#ifdef CU_DEATH_TESTS
static int invalid_start;
static void invalid(void)
{
    CuArena a;
    CuProcessIO *io;
    char name[] = "never-executed";
    char *args[2];
    int result;

    reset();
    guard_active = 1;
    cu_arena_init(&a);
    io = NULL;
    args[0] = invalid_start == 4 ? NULL : name;
    args[1] = NULL;
    result = ctor_start_probe(invalid_start == 0 ? NULL : &io,
        invalid_start == 1 ? NULL : &a, invalid_start == 2 ? NULL : name,
        invalid_start == 3 ? NULL : args, NULL);
    (void)result;
}
#endif

int main(void)
{
    int k, n, zero, mode;
    /* Keep strict builds warning-free while the module is still a stub. */
    (void)pipe_probe; (void)fcntl_probe; (void)close_probe;
    (void)spawn_probe; (void)poll_probe;
    reset(); run(0, 0);
    reset(); low = 0; run(0, 0);
    for (k = PIPE_FAIL; k <= SPAWN_FAIL; k++) {
        for (n = 1; n <= (k == SPAWN_FAIL ? 1 : 3); n++) {
            for (zero = 0; zero < 2; zero++) {
                reset(); kind = k; ordinal = n;
                error_value = zero ? 0 : EIO;
                run(0, 0);
            }
        }
    }
    reset(); run(1, 0);
    for (n = 1; n <= 3; n++) {
        for (zero = 0; zero < 2; zero++) {
            reset(); preclose_at = n; error_value = zero ? 0 : EINTR;
            run(0, 0);
        }
    }
    for (mode = 0; mode < 2; mode++) {
        for (zero = 0; zero < 3; zero++) {
            reset(); close_fault = 1;
            close_error = zero == 0 ? 0 : (zero == 1 ? EINTR : EIO);
            run(0, mode);
        }
    }
#ifdef CU_DEATH_TESTS
    for (invalid_start = 0; invalid_start < 5; invalid_start++) expect_abort(invalid);
#endif
    return 0;
}
#endif
