/* Boundary failures must not release storage while a child is still owned. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "cu.h"
#include "curun.h"

#if defined(__linux__) && !defined(__cerb__) && !defined(__wasi__)
#include <signal.h>
#include <time.h>

#define MODEL_ALLOC 1
#define MODEL_START 2
#define MODEL_CLOCK 3
#define MODEL_PUMP 4
#define MODEL_TIMEOUT 5
#define MODEL_CLOSE 6
#define MODEL_WAIT 7
#define MODEL_WAIT_CLOCK 8
#define MODEL_EXPIRED 9
#define MODEL_REAP 10
#define MODEL_KILL 11
#define MODEL_ESRCH 12
#define MODEL_CANCEL 13
#define MODEL_GET_SIGNAL 14
#define MODEL_SET_SIGNAL 15
#define MODEL_EMPTY_SIGNAL 16
#define MODEL_RESTORE 17
#define MODEL_OUTPUT 18
#define MODEL_ERROR_OUTPUT 19
#define MODEL_FLUSH_OUT 20
#define MODEL_FLUSH_ERR 21
#define MODEL_REPORT 22

static int model_mode;
static int model_live;
static int model_done;
static int model_kills;
static int model_polls;
static int model_closes;
static int model_stamps;
static int model_signal_sets;
static int model_signal_failed;
static int model_io_token;
static int model_child_token;
static char model_out[8];
static char model_err[8];
static int model_pushes;
static struct timespec model_now;

static void model_release(CuArena *a)
{
    assert(!model_live);
    cu_arena_release(a);
}

static char *model_push(CuArena *a, size_t count)
{
    (void)a;
    assert(count <= 8);
    if (model_mode == MODEL_ALLOC)
        return NULL;
    model_pushes++;
    return model_pushes == 1 ? model_out : model_err;
}

static int model_start(CuProcessIO **io, CuArena *a, const char *path,
                       char *const argv[], char *const envp[])
{
    (void)a;
    (void)path;
    (void)argv;
    (void)envp;
    if (model_mode == MODEL_START) {
        errno = 0;
        return 0;
    }
    assert(!model_live);
    model_live = 1;
    *io = (void *)&model_io_token;
    return 1;
}

static CuProcess *model_child(CuProcessIO *io)
{
    assert(io == (void *)&model_io_token);
    return (void *)&model_child_token;
}

static int model_pump(CuProcessIO *io, CuStr input, CuProcessCapture *capture,
                      unsigned long timeout);

static int model_close(CuProcessIO *io)
{
    assert(io == (void *)&model_io_token);
    model_closes++;
    if (model_mode == MODEL_CLOSE) {
        errno = 0;
        return 0;
    }
    return 1;
}

static CuProcessWait model_wait(CuProcess *child, unsigned long timeout, CuProcessExit *out)
{
    assert(child == (void *)&model_child_token);
    assert(model_live);
    if (timeout != 0 && model_mode == MODEL_WAIT) {
        errno = 0;
        return CU_PROCESS_ERROR;
    }
    if (timeout == 0) {
        model_polls++;
        if (model_polls == 1 && model_mode == MODEL_REAP) {
            errno = 0;
            return CU_PROCESS_ERROR;
        }
        if (model_kills == 0 || (model_mode == MODEL_KILL && model_kills == 1))
            return CU_PROCESS_RUNNING;
    }
    out->kind = model_kills == 0 ? CU_PROCESS_EXITED : CU_PROCESS_SIGNALED;
    out->code = model_kills == 0 ? 7 : SIGKILL;
    model_live = 0;
    model_done = 1;
    return CU_PROCESS_DONE;
}

static int model_signal(CuProcess *child, CuProcessSignal what)
{
    assert(model_live && !model_done);
    assert(child == (void *)&model_child_token && what == CU_PROCESS_KILL);
    model_kills++;
    if (model_mode == MODEL_KILL && model_kills == 1) {
        errno = EPERM;
        return 0;
    }
    if (model_mode == MODEL_ESRCH) {
        errno = ESRCH;
        return 0;
    }
    return 1;
}

static int model_clock(clockid_t id, struct timespec *out)
{
    assert(id == CLOCK_MONOTONIC);
    model_stamps++;
    if (model_mode == MODEL_CLOCK || (model_mode == MODEL_WAIT_CLOCK && model_stamps == 2)) {
        errno = 0;
        return -1;
    }
    *out = model_now;
    if (model_mode == MODEL_EXPIRED && model_stamps == 2)
        out->tv_sec += 2;
    return 0;
}

static int model_sleep(const struct timespec *requested, struct timespec *left)
{
    (void)left;
    assert(requested->tv_sec == 0 && requested->tv_nsec == 10000000L);
    return 0;
}

static int model_empty(sigset_t *mask)
{
    if (model_mode == MODEL_EMPTY_SIGNAL && !model_signal_failed) {
        model_signal_failed = 1;
        errno = 0;
        return -1;
    }
    return sigemptyset(mask);
}

static int model_action(int sig, const struct sigaction *action, struct sigaction *old)
{
    assert(sig == SIGHUP || sig == SIGINT || sig == SIGTERM || sig == SIGPIPE);
    if (old != NULL) {
        if (model_mode == MODEL_GET_SIGNAL) {
            errno = 0;
            return -1;
        }
        old->sa_handler = SIG_IGN;
        old->sa_flags = 0;
        assert(sigemptyset(&old->sa_mask) == 0);
    }
    if (action != NULL) {
        model_signal_sets++;
        if (model_mode == MODEL_SET_SIGNAL && model_signal_sets == 2) {
            errno = 0;
            return -1;
        }
        if (model_mode == MODEL_RESTORE && model_done && !model_signal_failed) {
            model_signal_failed = 1;
            errno = 0;
            return -1;
        }
    }
    return 0;
}

static size_t model_write(const void *data, size_t size, size_t count, FILE *stream)
{
    assert(model_done && !model_live);
    if ((model_mode == MODEL_OUTPUT && stream == stdout) ||
        (model_mode == MODEL_ERROR_OUTPUT && stream == stderr)) {
        errno = 0;
        return 0;
    }
    return fwrite(data, size, count, stream);
}

static int model_flush(FILE *stream)
{
    if ((model_mode == MODEL_FLUSH_OUT && stream == stdout) ||
        (model_mode == MODEL_FLUSH_ERR && stream == stderr)) {
        assert(model_done && !model_live);
        errno = 0;
        return EOF;
    }
    return fflush(stream);
}

static int model_print(FILE *stream, const char *format, ...)
{
    va_list args;
    int result;

    if (model_mode == MODEL_REPORT && strcmp(format, "curun: child exited %d\n") == 0) {
        assert(model_done && !model_live);
        errno = 0;
        return -1;
    }
    va_start(args, format);
    result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int model_main(int argc, char *argv[], char *envp[]);
#define curun_main model_main
#define cu_arena_release model_release
#define cu_arena_push_bytes model_push
#define cu_process_io_start model_start
#define cu_process_io_child model_child
#define cu_process_io_pump model_pump
#define cu_process_io_close model_close
#define cu_process_wait model_wait
#define cu_process_signal model_signal
#define clock_gettime model_clock
#define nanosleep model_sleep
#define sigemptyset model_empty
#define sigaction(a, b, c) model_action((a), (b), (c))
#define fwrite model_write
#define fflush model_flush
#define fprintf model_print
#include "../src/curun.c"
#undef fprintf
#undef fflush
#undef fwrite
#undef sigaction
#undef sigemptyset
#undef nanosleep
#undef clock_gettime
#undef cu_process_signal
#undef cu_process_wait
#undef cu_process_io_close
#undef cu_process_io_pump
#undef cu_process_io_child
#undef cu_process_io_start
#undef cu_arena_push_bytes
#undef cu_arena_release
#undef curun_main

static int model_pump(CuProcessIO *io, CuStr input, CuProcessCapture *capture,
                      unsigned long timeout)
{
    assert(io == (void *)&model_io_token && input.len == 0 && timeout == 1000);
    capture->out_len = 0;
    capture->err_len = 0;
    capture->input_sent = 0;
    if (model_mode >= MODEL_OUTPUT && model_mode <= MODEL_FLUSH_ERR) {
        model_out[0] = 'a';
        model_err[0] = 'b';
        capture->out_len = 1;
        capture->err_len = 1;
    }
    if (model_mode == MODEL_CANCEL)
        cancelled = SIGTERM;
    if (model_mode == MODEL_TIMEOUT) {
        errno = ETIMEDOUT;
        return 0;
    }
    if (model_mode == MODEL_PUMP || model_mode == MODEL_REAP ||
        model_mode == MODEL_KILL || model_mode == MODEL_ESRCH) {
        errno = 0;
        return 0;
    }
    return 1;
}

int main(void)
{
    char *argv[11];
    int mode;
    int expected;
    struct timespec start;
    unsigned long left;

    argv[0] = "curun";
    argv[1] = "--stdout-limit";
    argv[2] = "8";
    argv[3] = "--stderr-limit";
    argv[4] = "8";
    argv[5] = "--timeout-ms";
    argv[6] = "1000";
    argv[7] = "--";
    argv[8] = "/model-child";
    argv[9] = NULL;
    for (mode = 0; mode <= MODEL_REPORT; mode++) {
        model_mode = mode;
        model_live = 0;
        model_done = 0;
        model_kills = 0;
        model_polls = 0;
        model_closes = 0;
        model_stamps = 0;
        model_signal_sets = 0;
        model_signal_failed = 0;
        model_pushes = 0;
        model_now.tv_sec = 0;
        model_now.tv_nsec = 0;
        expected = mode == 0 ? 7 : 125;
        if (mode == MODEL_TIMEOUT || mode == MODEL_EXPIRED)
            expected = 124;
        if (mode == MODEL_CANCEL)
            expected = 128 + SIGTERM;
        if (mode == MODEL_REPORT) {
            argv[7] = "--report";
            argv[8] = "--";
            argv[9] = "/model-child";
            argv[10] = NULL;
        }
        assert(model_main(mode == MODEL_REPORT ? 10 : 9, argv, NULL) == expected);
        assert(!model_live);
        if (mode >= MODEL_CLOCK && mode <= MODEL_CANCEL)
            assert(model_done && model_closes == 1);
        if (mode == MODEL_KILL)
            assert(model_kills == 2);
    }
    model_mode = 0;
    start.tv_sec = 10;
    start.tv_nsec = 900000000L;
    model_now.tv_sec = 11;
    model_now.tv_nsec = 100000000L;
    assert(remaining(&start, 500, &left) == 1 && left == 300);
    start.tv_sec = 11;
    start.tv_nsec = 100000000L;
    model_now.tv_nsec++;
    assert(remaining(&start, 2, &left) == 1 && left == 1);
    assert(remaining(&start, 1, &left) == 0);
    return 0;
}
#else
int main(void)
{
    return 0;
}
#endif
