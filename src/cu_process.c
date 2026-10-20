/* posix_spawn and monotonic waiting need the POSIX interfaces. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdlib.h>
#include "cu.h"

#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
#define CU_NO_PROCESSES 1
#endif
#ifndef ENOSYS
#define ENOSYS EINVAL
#endif

#ifdef CU_NO_PROCESSES

struct CuProcess {
    int unused;
};

int cu_process_start(CuProcess **p, CuArena *a, const char *path,
                     char *const argv[], char *const envp[])
{
    (void)envp;
    if (p == NULL || a == NULL || path == NULL || argv == NULL || argv[0] == NULL)
        abort();
    errno = ENOSYS;
    return 0;
}

int cu_process_start_redirected(CuProcess **p, CuArena *a, const char *path,
                                char *const argv[], char *const envp[],
                                const CuProcessStdio *stdio)
{
    (void)envp;
    if (p == NULL || a == NULL || path == NULL || argv == NULL || argv[0] == NULL ||
        stdio == NULL)
        abort();
    errno = ENOSYS;
    return 0;
}

/* No process object can exist on this backend. */
CuProcessWait cu_process_wait(CuProcess *p, unsigned long timeout_ms, CuProcessExit *out)
{
    (void)p;
    (void)timeout_ms;
    (void)out;
    abort();
}

int cu_process_signal(CuProcess *p, CuProcessSignal signal)
{
    (void)p;
    (void)signal;
    abort();
}

#else

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct CuProcess {
    pid_t id;
    int done;
    CuProcessExit status;
};

static int start_child(CuProcess **p, CuArena *a, const char *path,
                       char *const argv[], char *const envp[],
                       const posix_spawn_file_actions_t *actions)
{
    CuProcess *record;
    struct sigaction action;
    sigset_t mask;
    sigset_t defaults;
    posix_spawnattr_t attr;
    char *empty_env[1];
    pid_t id;
    int code;

    if (p == NULL || a == NULL || path == NULL || argv == NULL || argv[0] == NULL)
        abort();
    if (sigaction(SIGCHLD, NULL, &action) != 0)
        return 0;
    if (action.sa_handler == SIG_IGN) {
        errno = EINVAL;
        return 0;
    }
#ifdef SA_NOCLDWAIT
    if ((action.sa_flags & SA_NOCLDWAIT) != 0) {
        errno = EINVAL;
        return 0;
    }
#endif
    record = CU_PUSH(a, CuProcess, 1);
    if (record == NULL) {
        errno = ENOMEM;
        return 0;
    }
    record->id = 0;
    record->done = 0;
    record->status.kind = CU_PROCESS_EXITED;
    record->status.code = 0;
    if (sigemptyset(&mask) != 0 || sigfillset(&defaults) != 0 ||
        sigdelset(&defaults, SIGKILL) != 0 || sigdelset(&defaults, SIGSTOP) != 0)
        return 0;
    code = posix_spawnattr_init(&attr);
    if (code != 0) {
        errno = code;
        return 0;
    }
    code = posix_spawnattr_setsigmask(&attr, &mask);
    if (code == 0)
        code = posix_spawnattr_setsigdefault(&attr, &defaults);
    if (code == 0)
        code = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    empty_env[0] = NULL;
    if (code == 0)
        code = posix_spawn(&id, path, actions, &attr, argv,
                           envp == NULL ? empty_env : envp);
    /* Cleanup must not replace the setup/spawn error, or discard ownership
     * after a successful spawn. No fallible record setup follows spawn. */
    (void)posix_spawnattr_destroy(&attr);
    if (code != 0) {
        errno = code;
        return 0;
    }
    record->id = id;
    *p = record;
    return 1;
}

int cu_process_start(CuProcess **p, CuArena *a, const char *path,
                     char *const argv[], char *const envp[])
{
    return start_child(p, a, path, argv, envp, NULL);
}

static int copy_source(int fd)
{
#ifdef F_DUPFD_CLOEXEC
    return fcntl(fd, F_DUPFD_CLOEXEC, 3);
#else
    (void)fd;
    errno = ENOSYS;
    return -1;
#endif
}

int cu_process_start_redirected(CuProcess **p, CuArena *a, const char *path,
                                char *const argv[], char *const envp[],
                                const CuProcessStdio *stdio)
{
    posix_spawn_file_actions_t actions;
    int source[3];
    int copy[3];
    int i;
    int count;
    int actions_ready;
    int code;
    int ok;
    int saved;

    if (p == NULL || a == NULL || path == NULL || argv == NULL || argv[0] == NULL ||
        stdio == NULL)
        abort();
    source[0] = stdio->input;
    source[1] = stdio->output;
    source[2] = stdio->error;
    count = 0;
    /* Validate before allocating FDs: a new copy must not accidentally make
     * a later, originally closed source descriptor appear valid. Caller
     * keeps all source descriptor identities stable throughout start. */
    for (i = 0; i < 3; i++) {
        copy[i] = -1;
        if (source[i] < CU_PROCESS_INHERIT) {
            errno = EBADF;
            return 0;
        }
        if (source[i] != CU_PROCESS_INHERIT) {
            if (fcntl(source[i], F_GETFD) < 0)
                return 0;
            count++;
        }
    }
    if (count == 0)
        return start_child(p, a, path, argv, envp, NULL);
    actions_ready = 0;
    ok = 0;
    /* Snapshot every source before child actions can replace 0, 1 or 2. */
    for (i = 0; i < 3; i++) {
        if (source[i] != CU_PROCESS_INHERIT) {
            copy[i] = copy_source(source[i]);
            if (copy[i] < 0)
                goto cleanup;
        }
    }
    code = posix_spawn_file_actions_init(&actions);
    if (code != 0) {
        errno = code;
        goto cleanup;
    }
    actions_ready = 1;
    for (i = 0; i < 3; i++) {
        if (copy[i] >= 0) {
            code = posix_spawn_file_actions_adddup2(&actions, copy[i], i);
            if (code == 0)
                code = posix_spawn_file_actions_addclose(&actions, copy[i]);
            if (code != 0) {
                errno = code;
                goto cleanup;
            }
        }
    }
    ok = start_child(p, a, path, argv, envp, &actions);
cleanup:
    saved = errno;
    if (actions_ready)
        (void)posix_spawn_file_actions_destroy(&actions);
    for (i = 2; i >= 0; i--) {
        if (copy[i] >= 0)
            (void)close(copy[i]);
    }
    errno = saved;
    return ok;
}

static int stamp(unsigned long *sec, unsigned long *nsec)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    *sec = (unsigned long)now.tv_sec;
    *nsec = (unsigned long)now.tv_nsec;
    return 1;
}

/* Subtract unsigned seconds modulo ULONG_MAX+1, never add a signed absolute
 * deadline. Even ULONG_MAX milliseconds spans less than one seconds wrap.
 * Return -1 on clock failure, 0 on expiry, 1 with a clamped sleep duration.
 * Keep the fractional millisecond so a sleep never exceeds the remainder. */
static int budget(unsigned long sec, unsigned long nsec, unsigned long timeout_ms,
                  struct timespec *nap)
{
    unsigned long now_sec;
    unsigned long now_nsec;
    unsigned long elapsed;
    unsigned long left;
    unsigned long used;

    if (!stamp(&now_sec, &now_nsec))
        return -1;
    elapsed = now_sec - sec;
    if (now_nsec < nsec) {
        elapsed--;
        now_nsec = 1000000000UL - nsec + now_nsec;
    } else {
        now_nsec -= nsec;
    }
    if (elapsed > timeout_ms / 1000)
        return 0;
    left = timeout_ms - elapsed * 1000;
    used = now_nsec / 1000000UL;
    if (used >= left)
        return 0;
    left -= used;
    nap->tv_sec = 0;
    if (left > 10)
        nap->tv_nsec = 10000000L;
    else
        nap->tv_nsec = (long)(left * 1000000UL - now_nsec % 1000000UL);
    return 1;
}

CuProcessWait cu_process_wait(CuProcess *p, unsigned long timeout_ms, CuProcessExit *out)
{
    unsigned long sec;
    unsigned long nsec;
    struct timespec nap;
    pid_t result;
    int status;
    int remaining;
    int interrupted;

    if (p == NULL || out == NULL)
        abort();
    if (p->done) {
        *out = p->status;
        return CU_PROCESS_DONE;
    }
    sec = 0;
    nsec = 0;
    if (timeout_ms != 0 && !stamp(&sec, &nsec))
        return CU_PROCESS_ERROR;
    for (;;) {
        result = waitpid(p->id, &status, WNOHANG);
        interrupted = 0;
        if (result < 0) {
            if (errno == ECHILD)
                abort();
            if (errno != EINTR)
                return CU_PROCESS_ERROR;
            interrupted = 1;
        } else if (result > 0) {
            if (WIFEXITED(status)) {
                p->status.kind = CU_PROCESS_EXITED;
                p->status.code = WEXITSTATUS(status);
                p->done = 1;
            } else if (WIFSIGNALED(status)) {
                p->status.kind = CU_PROCESS_SIGNALED;
                p->status.code = WTERMSIG(status);
                p->done = 1;
            }
            if (p->done) {
                *out = p->status;
                return CU_PROCESS_DONE;
            }
        }
        if (timeout_ms == 0) {
            if (interrupted)
                continue;
            return CU_PROCESS_RUNNING;
        }
        remaining = budget(sec, nsec, timeout_ms, &nap);
        if (remaining < 0)
            return CU_PROCESS_ERROR;
        if (remaining == 0)
            return CU_PROCESS_RUNNING;
        if (interrupted)
            continue;
        /* An interrupted sleep restarts at waitpid and the original budget,
         * not at an unbounded retry of the requested sleep interval. */
        if (nanosleep(&nap, NULL) != 0 && errno != EINTR)
            return CU_PROCESS_ERROR;
    }
}

int cu_process_signal(CuProcess *p, CuProcessSignal signal)
{
    int native_signal;

    if (p == NULL || (signal != CU_PROCESS_TERMINATE && signal != CU_PROCESS_KILL))
        abort();
    if (p->done) {
        errno = ESRCH;
        return 0;
    }
    native_signal = signal == CU_PROCESS_TERMINATE ? SIGTERM : SIGKILL;
    return kill(p->id, native_signal) == 0;
}

#endif
