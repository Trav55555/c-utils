#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "cu.h"
#include "curun.h"

#if defined(__linux__) && !defined(__cerb__) && !defined(__wasi__)
#define CURUN_NATIVE 1
#include <signal.h>
#include <time.h>
#endif

#define CURUN_MAX_CAPTURE 16777216UL

typedef struct Options {
    unsigned long timeout;
    size_t out_cap;
    size_t err_cap;
    int report;
    int command;
} Options;

static void usage(FILE *stream)
{
    fputs("usage: curun [--timeout-ms N] [--stdout-limit N] [--stderr-limit N]\n"
          "             [--report] -- EXECUTABLE [ARG ...]\n"
          "Defaults: 5000 ms, 1048576 bytes per stream; maximum 16777216 per stream.\n"
          "Explicit paths only; no shell/PATH search. Child stdin is EOF.\n", stream);
}

static int decimal(const char *text, unsigned long max, unsigned long *out)
{
    unsigned long value;
    unsigned long digit;

    if (*text == '\0')
        return 0;
    value = 0;
    while (*text != '\0') {
        if (*text < '0' || *text > '9')
            return 0;
        digit = (unsigned long)(*text - '0');
        if (value > (max - digit) / 10)
            return 0;
        value = value * 10 + digit;
        text++;
    }
    *out = value;
    return 1;
}

static int options(int argc, char *argv[], Options *out)
{
    int i;
    int kind;
    unsigned long value;

    out->timeout = 5000;
    out->out_cap = 1048576;
    out->err_cap = 1048576;
    out->report = 0;
    out->command = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return fflush(stdout) == 0 && !ferror(stdout) ? 0 : -2;
        }
        if (strcmp(argv[i], "--report") == 0) {
            out->report = 1;
            continue;
        }
        if (strcmp(argv[i], "--") == 0) {
            if (i + 1 == argc || strchr(argv[i + 1], '/') == NULL)
                break;
            out->command = i + 1;
            return 1;
        }
        kind = 0;
        if (strcmp(argv[i], "--timeout-ms") == 0)
            kind = 1;
        else if (strcmp(argv[i], "--stdout-limit") == 0)
            kind = 2;
        else if (strcmp(argv[i], "--stderr-limit") == 0)
            kind = 3;
        if (kind == 0 || i + 1 == argc)
            break;
        i++;
        if (!decimal(argv[i], kind == 1 ? ULONG_MAX : CURUN_MAX_CAPTURE, &value))
            break;
        if (kind == 1) {
            if (value == 0)
                break;
            out->timeout = value;
        } else {
            if ((unsigned long)(size_t)value != value)
                break;
            if (kind == 2)
                out->out_cap = (size_t)value;
            else
                out->err_cap = (size_t)value;
        }
    }
    usage(stderr);
    return -1;
}

#ifdef CURUN_NATIVE
static volatile sig_atomic_t cancelled;

static void cancel(int signum)
{
    if (cancelled == 0)
        cancelled = signum;
}

static int handlers(struct sigaction old[], int *count)
{
    struct sigaction action;
    int signals[4];
    int i;

    signals[0] = SIGPIPE;
    signals[1] = SIGHUP;
    signals[2] = SIGINT;
    signals[3] = SIGTERM;
    *count = 0;
    for (i = 0; i < 4; i++) {
        if (sigaction(signals[i], NULL, &old[i]) != 0)
            return 0;
        action.sa_handler = signals[i] == SIGPIPE ? SIG_IGN : cancel;
        if (signals[i] == SIGHUP && old[i].sa_handler == SIG_IGN)
            action.sa_handler = SIG_IGN;
        action.sa_flags = 0;
        if (sigemptyset(&action.sa_mask) != 0 || sigaction(signals[i], &action, NULL) != 0)
            return 0;
        *count = i + 1;
    }
    return 1;
}

static int restore(struct sigaction old[], int count)
{
    int signals[4];
    int ok;

    signals[0] = SIGPIPE;
    signals[1] = SIGHUP;
    signals[2] = SIGINT;
    signals[3] = SIGTERM;
    ok = 1;
    while (count > 0) {
        count--;
        if (sigaction(signals[count], &old[count], NULL) != 0)
            ok = 0;
    }
    return ok;
}

/* Conservative whole-millisecond remainder; never restart the wait budget. */
static int remaining(const struct timespec *start, unsigned long limit, unsigned long *left)
{
    struct timespec now;
    unsigned long sec;
    unsigned long nsec;
    unsigned long used;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    sec = (unsigned long)now.tv_sec - (unsigned long)start->tv_sec;
    nsec = (unsigned long)now.tv_nsec;
    if (nsec < (unsigned long)start->tv_nsec) {
        sec--;
        nsec += 1000000000UL;
    }
    nsec -= (unsigned long)start->tv_nsec;
    if (sec > limit / 1000)
        return 0;
    used = (nsec + 999999UL) / 1000000UL;
    *left = limit - sec * 1000;
    if (used >= *left)
        return 0;
    *left -= used;
    return 1;
}

static void diagnostic(const char *operation, int saved)
{
    fprintf(stderr, "curun: %s failed (errno %d)\n", operation, saved);
}

/* Clock-independent polls keep ownership even if a timed wait's clock fails.
 * Cleanup has no hard return bound. Do not release the arena before DONE. */
static int reap(CuProcess *child, CuProcessExit *status)
{
    CuProcessWait waited;
    struct timespec nap;
    int killed;
    int warned;
    int saved;
    int ok;

    killed = 0;
    warned = 0;
    ok = 1;
    nap.tv_sec = 0;
    nap.tv_nsec = 10000000L;
    for (;;) {
        waited = cu_process_wait(child, 0, status);
        if (waited == CU_PROCESS_DONE)
            return ok;
        if (waited == CU_PROCESS_ERROR) {
            saved = errno;
            ok = 0;
            if (!warned)
                diagnostic("reap", saved);
            warned = 1;
        }
        if (!killed) {
            if (cu_process_signal(child, CU_PROCESS_KILL))
                killed = 1;
            else if (errno == ESRCH)
                killed = 1;
            else {
                saved = errno;
                ok = 0;
                if (!warned)
                    diagnostic("kill", saved);
                warned = 1;
            }
        }
        (void)nanosleep(&nap, NULL);
    }
}

static int execute(const Options *opts, char *argv[], char *envp[])
{
    CuArena arena;
    CuProcessIO *io;
    CuProcess *child;
    CuProcessCapture capture;
    CuProcessExit status;
    CuProcessWait waited;
    struct timespec start;
    unsigned long left;
    int result;
    int saved;
    int budget;
    int ok;
    int output_failed;
    int printed;

    cu_arena_init(&arena);
    io = NULL;
    capture.out = NULL;
    capture.err = NULL;
    capture.out_cap = opts->out_cap;
    capture.err_cap = opts->err_cap;
    capture.out_len = 0;
    capture.err_len = 0;
    capture.input_sent = 0;
    if (opts->out_cap != 0)
        capture.out = cu_arena_push_bytes(&arena, opts->out_cap);
    if (opts->err_cap != 0)
        capture.err = cu_arena_push_bytes(&arena, opts->err_cap);
    result = 125;
    if ((opts->out_cap != 0 && capture.out == NULL) ||
        (opts->err_cap != 0 && capture.err == NULL)) {
        diagnostic("allocation", ENOMEM);
        goto release;
    }
    if (cancelled != 0)
        goto release;
    if (!cu_process_io_start(&io, &arena, argv[opts->command], argv + opts->command, envp)) {
        saved = errno;
        diagnostic("start", saved);
        goto release;
    }
    child = cu_process_io_child(io);
    waited = CU_PROCESS_RUNNING;
    ok = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        diagnostic("clock", errno);
    else if (cancelled == 0) {
        ok = cu_process_io_pump(io, cu_str_n(NULL, 0), &capture, opts->timeout);
        saved = errno;
        if (!ok && cancelled == 0) {
            if (saved == ETIMEDOUT) {
                fputs("curun: deadline exceeded\n", stderr);
                result = 124;
            } else if (saved == EFBIG)
                fputs("curun: capture limit exceeded\n", stderr);
            else
                diagnostic("communication", saved);
        }
    }
    if (!cu_process_io_close(io) && ok) {
        diagnostic("close", errno);
        ok = 0;
    }
    while (ok && cancelled == 0) {
        budget = remaining(&start, opts->timeout, &left);
        if (budget <= 0) {
            if (budget == 0) {
                fputs("curun: deadline exceeded\n", stderr);
                result = 124;
            } else
                diagnostic("clock", errno);
            ok = 0;
            break;
        }
        waited = cu_process_wait(child, left < 50 ? left : 50, &status);
        if (waited == CU_PROCESS_DONE)
            break;
        if (waited == CU_PROCESS_ERROR) {
            diagnostic("wait", errno);
            ok = 0;
        }
    }
    if (waited != CU_PROCESS_DONE && !reap(child, &status))
        ok = 0;
    if (ok && cancelled == 0) {
        if (status.kind == CU_PROCESS_EXITED)
            result = status.code;
        else
            result = status.code < 128 ? 128 + status.code : 255;
    }
    output_failed = 0;
    saved = 0;
    if (capture.out_len != 0 && fwrite(capture.out, 1, capture.out_len, stdout) != capture.out_len) {
        saved = errno;
        output_failed = 1;
    }
    if (capture.err_len != 0 && fwrite(capture.err, 1, capture.err_len, stderr) != capture.err_len) {
        if (!output_failed)
            saved = errno;
        output_failed = 1;
    }
    if (fflush(stdout) != 0) {
        if (!output_failed)
            saved = errno;
        output_failed = 1;
    }
    if (fflush(stderr) != 0) {
        if (!output_failed)
            saved = errno;
        output_failed = 1;
    }
    if (output_failed) {
        diagnostic("output", saved);
        (void)fflush(stderr);
        result = 125;
    }
    if (opts->report) {
        if (status.kind == CU_PROCESS_EXITED)
            printed = fprintf(stderr, "curun: child exited %d\n", status.code);
        else
            printed = fprintf(stderr, "curun: child signal %d\n", status.code);
        if (printed < 0)
            result = 125;
        if (fflush(stderr) != 0 || ferror(stderr))
            result = 125;
    }
release:
    cu_arena_release(&arena);
    return result;
}
#endif

int curun_main(int argc, char *argv[], char *envp[])
{
    Options opts;
    int parsed;
    int result;
#ifdef CURUN_NATIVE
    struct sigaction old[4];
    int count;
#endif

#ifdef CURUN_NATIVE
    cancelled = 0;
    if (!handlers(old, &count)) {
        diagnostic("signal setup", errno);
        (void)fflush(stderr);
        (void)restore(old, count);
        return 125;
    }
#endif
    parsed = options(argc, argv, &opts);
    if (parsed <= 0)
        result = parsed == 0 ? 0 : parsed == -2 ? 125 : 2;
    else {
#ifdef CURUN_NATIVE
        result = execute(&opts, argv, envp);
#else
        (void)envp;
        fputs("curun: process communication is unavailable on this platform\n", stderr);
        result = 125;
#endif
    }
    if (fflush(stdout) != 0 || ferror(stdout))
        result = 125;
    if (fflush(stderr) != 0 || ferror(stderr))
        result = 125;
#ifdef CURUN_NATIVE
    if (cancelled != 0) {
        fprintf(stderr, "curun: interrupted by signal %d\n", (int)cancelled);
        (void)fflush(stderr);
        result = cancelled < 128 ? 128 + (int)cancelled : 255;
    }
    if (!restore(old, count))
        result = 125;
#endif
    return result;
}
