#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
int main(void) { return 0; }
#else
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "death.h"

#define cu_process_start process_start_probe
#define cu_process_start_redirected process_start_redirected_probe
#define cu_process_wait process_wait_probe
#define cu_process_signal process_signal_probe
#include "cu.h"

static int fail_step;
static int attr_live;
static int destroys;
static int spawns;
static int waits;
static int kills;
static int last_signal;
static int native_exit;
static int terminal;
static int wait_error;
static int wait_interruptions;
static int clock_calls;
static int clock_failure;
static int clock_error;
static int sleeps;
static int sleep_error;
static unsigned long seconds;
static unsigned long nanoseconds;
static unsigned long clock_step;
static int clock_jump;
static unsigned long jump_seconds;
static unsigned long jump_nanoseconds;
static long first_nap;

/* Kernel-produced status is the oracle, not a guessed status-bit layout. */
static void capture_exit_status(void)
{
    pid_t pid;
    pid_t result;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0)
        _exit(57);
    do {
        result = waitpid(pid, &native_exit, 0);
    } while (result < 0 && errno == EINTR);
    assert(result == pid && WIFEXITED(native_exit) && WEXITSTATUS(native_exit) == 57);
}

static void advance(unsigned long amount)
{
    nanoseconds += amount;
    if (nanoseconds >= 1000000000UL) {
        nanoseconds -= 1000000000UL;
        seconds++;
    }
}

static int action_probe(int signal, const struct sigaction *action, struct sigaction *out)
{
    assert(signal == SIGCHLD && action == NULL);
    if (fail_step == 1) { errno = EIO; return -1; }
    out->sa_handler = SIG_DFL;
    out->sa_flags = 0;
    return sigemptyset(&out->sa_mask);
}

static int empty_probe(sigset_t *set)
{
    if (fail_step == 2) { errno = EIO; return -1; }
    return sigemptyset(set);
}

static int fill_probe(sigset_t *set)
{
    if (fail_step == 3) { errno = EIO; return -1; }
    return sigfillset(set);
}

static int delete_probe(sigset_t *set, int signal)
{
    if ((fail_step == 4 && signal == SIGKILL) || (fail_step == 5 && signal == SIGSTOP)) {
        errno = EIO;
        return -1;
    }
    return sigdelset(set, signal);
}

static int init_probe(posix_spawnattr_t *attr)
{
    int code;

    if (fail_step == 6) { errno = EBUSY; return ENOMEM; }
    code = posix_spawnattr_init(attr);
    if (code == 0) attr_live = 1;
    return code;
}

static int mask_probe(posix_spawnattr_t *attr, const sigset_t *mask)
{
    assert(attr_live);
    if (fail_step == 7) { errno = EBUSY; return EINVAL; }
    return posix_spawnattr_setsigmask(attr, mask);
}

static int default_probe(posix_spawnattr_t *attr, const sigset_t *defaults)
{
    assert(attr_live);
    if (fail_step == 8) { errno = EBUSY; return EINVAL; }
    return posix_spawnattr_setsigdefault(attr, defaults);
}

static int flags_probe(posix_spawnattr_t *attr, short flags)
{
    assert(attr_live);
    if (fail_step == 9) { errno = EBUSY; return EINVAL; }
    return posix_spawnattr_setflags(attr, flags);
}

static int destroy_probe(posix_spawnattr_t *attr)
{
    assert(attr_live);
    assert(posix_spawnattr_destroy(attr) == 0);
    attr_live = 0;
    destroys++;
    errno = ERANGE;
    return EIO; /* Must not overwrite the operation's error or lose a child. */
}

static int spawn_probe(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                       const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    short flags;
    sigset_t mask;
    sigset_t defaults;

    assert(attr_live && path != NULL && actions == NULL && argv[0] != NULL);
    assert(envp != NULL && envp[0] == NULL);
    /* This libc getter is not tracked by MSan. Zero cannot satisfy the
     * nonzero expected flags, so initialization does not weaken the oracle. */
    flags = 0;
    assert(posix_spawnattr_getflags(attr, &flags) == 0);
    assert((flags & (POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF)) ==
           (POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
    assert(posix_spawnattr_getsigmask(attr, &mask) == 0 && sigismember(&mask, SIGTERM) == 0);
    assert(posix_spawnattr_getsigdefault(attr, &defaults) == 0);
    assert(sigismember(&defaults, SIGTERM) == 1 && sigismember(&defaults, SIGPIPE) == 1);
    spawns++;
    if (fail_step == 10) { errno = EBUSY; return EAGAIN; }
    *pid = (pid_t)4242;
    return 0;
}

static pid_t wait_probe(pid_t pid, int *status, int options)
{
    assert(pid == (pid_t)4242 && options == WNOHANG);
    waits++;
    assert(waits < 100);
    if (wait_error != 0) { errno = wait_error; return (pid_t)-1; }
    if (wait_interruptions != 0) {
        wait_interruptions--;
        advance(1000000UL);
        errno = EINTR;
        return (pid_t)-1;
    }
    if (terminal) { *status = native_exit; return pid; }
    return 0;
}

static int clock_probe(clockid_t clock, struct timespec *out)
{
    assert(clock == CLOCK_MONOTONIC);
    clock_calls++;
    if (clock_calls == clock_failure) { errno = clock_error; return -1; }
    if (clock_calls == 2 && clock_jump) {
        seconds = jump_seconds;
        nanoseconds = jump_nanoseconds;
    } else if (clock_calls > 1) {
        advance(clock_step);
    }
    out->tv_sec = (time_t)seconds;
    out->tv_nsec = (long)nanoseconds;
    return 0;
}

static int sleep_probe(const struct timespec *nap, struct timespec *remainder)
{
    assert(remainder == NULL && nap->tv_sec == 0);
    assert(nap->tv_nsec > 0 && nap->tv_nsec <= 10000000L);
    sleeps++;
    assert(sleeps < 100);
    if (sleeps == 1) first_nap = nap->tv_nsec;
    if (sleep_error != 0) {
        if (sleep_error == EINTR) advance(1000000UL);
        errno = sleep_error;
        return -1;
    }
    advance((unsigned long)nap->tv_nsec);
    return 0;
}

static int kill_probe(pid_t pid, int signal)
{
    assert(pid == (pid_t)4242);
    kills++;
    last_signal = signal;
    if (fail_step == 11) { errno = EPERM; return -1; }
    return 0;
}

#define sigaction(s, a, o) action_probe((s), (a), (o))
#define sigemptyset empty_probe
#define sigfillset fill_probe
#define sigdelset delete_probe
#define posix_spawnattr_init init_probe
#define posix_spawnattr_setsigmask mask_probe
#define posix_spawnattr_setsigdefault default_probe
#define posix_spawnattr_setflags flags_probe
#define posix_spawnattr_destroy destroy_probe
#define posix_spawn spawn_probe
#define waitpid wait_probe
#define clock_gettime clock_probe
#define nanosleep sleep_probe
#define kill kill_probe
#include "../src/cu_process.c"
#undef sigaction
#undef sigemptyset
#undef sigfillset
#undef sigdelset
#undef posix_spawnattr_init
#undef posix_spawnattr_setsigmask
#undef posix_spawnattr_setsigdefault
#undef posix_spawnattr_setflags
#undef posix_spawnattr_destroy
#undef posix_spawn
#undef waitpid
#undef clock_gettime
#undef nanosleep
#undef kill

static void reset(void)
{
    fail_step = 0; attr_live = 0; destroys = 0; spawns = 0;
    waits = 0; kills = 0; last_signal = 0; terminal = 0;
    wait_error = 0; wait_interruptions = 0; clock_calls = 0;
    clock_failure = 0; clock_error = EIO; sleeps = 0; sleep_error = 0;
    seconds = 10; nanoseconds = 0; clock_step = 0; first_nap = 0;
    clock_jump = 0; jump_seconds = 0; jump_nanoseconds = 0;
}

static CuProcess *start(CuArena *a)
{
    CuProcess *p;
    char name[] = "not-executed";
    char *args[2];

    args[0] = name; args[1] = NULL;
    assert(process_start_probe(&p, a, name, args, NULL));
    return p;
}

static void reap(CuProcess *p)
{
    CuProcessExit status;
    int count;

    terminal = 1;
    assert(process_wait_probe(p, 0, &status) == CU_PROCESS_DONE);
    assert(status.kind == CU_PROCESS_EXITED && status.code == 57);
    count = waits;
    assert(process_wait_probe(p, ULONG_MAX, &status) == CU_PROCESS_DONE);
    assert(waits == count);
    count = kills;
    errno = 0;
    assert(!process_signal_probe(p, CU_PROCESS_KILL) && errno == ESRCH);
    assert(kills == count);
}

static void setup_failures(void)
{
    CuArena a;
    CuProcess sentinel;
    CuProcess *p;
    char name[] = "not-executed";
    char *args[2];
    int step;
    int expected;

    args[0] = name; args[1] = NULL;
    cu_arena_init(&a);
    for (step = 1; step <= 10; step++) {
        reset(); fail_step = step; p = &sentinel; errno = 0;
        assert(!process_start_probe(&p, &a, name, args, NULL));
        expected = step <= 5 ? EIO : step == 6 ? ENOMEM : step == 10 ? EAGAIN : EINVAL;
        assert(p == &sentinel && errno == expected);
        assert(!attr_live && destroys == (step > 6 ? 1 : 0));
        assert(spawns == (step == 10 ? 1 : 0));
    }
    reset(); p = start(&a);
    assert(destroys == 1 && spawns == 1 && !attr_live);
    assert(process_signal_probe(p, CU_PROCESS_TERMINATE) && last_signal == SIGTERM);
    assert(process_signal_probe(p, CU_PROCESS_KILL) && last_signal == SIGKILL);
    fail_step = 11; errno = 0;
    assert(!process_signal_probe(p, CU_PROCESS_KILL) && errno == EPERM);
    reap(p);
    cu_arena_release(&a);
}

static void deadlines(void)
{
    CuArena a;
    CuProcess *p;
    CuProcessExit out;

    cu_arena_init(&a);
    reset(); p = start(&a);
    out.kind = CU_PROCESS_SIGNALED; out.code = -77;
    assert(process_wait_probe(p, 0, &out) == CU_PROCESS_RUNNING);
    assert(clock_calls == 0 && sleeps == 0);
    assert(out.kind == CU_PROCESS_SIGNALED && out.code == -77);
    waits = 0; wait_interruptions = 50;
    assert(process_wait_probe(p, 3, &out) == CU_PROCESS_RUNNING);
    assert(waits == 3 && sleeps == 0 && wait_interruptions == 47);
    assert(out.kind == CU_PROCESS_SIGNALED && out.code == -77);
    wait_interruptions = 0; reap(p);
    reset(); p = start(&a); sleep_error = EINTR;
    assert(process_wait_probe(p, 3, &out) == CU_PROCESS_RUNNING);
    assert(sleeps == 3 && out.code == -77);
    sleep_error = 0; reap(p);
    reset(); p = start(&a); nanoseconds = 999500000UL; clock_step = 250000UL;
    assert(process_wait_probe(p, 1, &out) == CU_PROCESS_RUNNING);
    assert(first_nap == 750000L && sleeps == 1);
    reap(p);
    reset(); p = start(&a); seconds = (unsigned long)LONG_MAX - 1;
    assert(process_wait_probe(p, 12, &out) == CU_PROCESS_RUNNING);
    assert(sleeps == 2 && first_nap == 10000000L);
    reap(p);
    reset(); p = start(&a); sleep_error = EIO;
    assert(process_wait_probe(p, ULONG_MAX, &out) == CU_PROCESS_ERROR && errno == EIO);
    assert(first_nap == 10000000L && out.code == -77);
    sleep_error = 0; reap(p);
    reset(); p = start(&a); clock_jump = 1;
    jump_seconds = seconds + ULONG_MAX / 1000;
    jump_nanoseconds = (ULONG_MAX % 1000 - 1) * 1000000UL + 250000UL;
    assert(process_wait_probe(p, ULONG_MAX, &out) == CU_PROCESS_RUNNING);
    assert(sleeps == 1 && first_nap == 750000L);
    reap(p);
    reset(); p = start(&a); clock_jump = 1; jump_seconds = seconds + 2;
    assert(process_wait_probe(p, 1000, &out) == CU_PROCESS_RUNNING && sleeps == 0);
    reap(p);
    /* On 32-bit hosts with wider time_t this is a valid positive clock
     * crossing the unsigned-seconds conversion wrap. */
    if (sizeof(time_t) > sizeof(unsigned long)) {
        reset(); p = start(&a); seconds = ULONG_MAX;
        nanoseconds = 999500000UL; clock_step = 250000UL;
        assert(process_wait_probe(p, 1, &out) == CU_PROCESS_RUNNING);
        assert(sleeps == 1 && first_nap == 750000L);
        reap(p);
    }
    cu_arena_release(&a);
}

static void wait_failures(void)
{
    CuArena a;
    CuProcess *p;
    CuProcessExit out;
    int call;

    cu_arena_init(&a);
    for (call = 1; call <= 2; call++) {
        reset(); p = start(&a); clock_failure = call; clock_error = call == 1 ? EIO : 0;
        out.kind = CU_PROCESS_SIGNALED; out.code = -77; errno = EBUSY;
        assert(process_wait_probe(p, 20, &out) == CU_PROCESS_ERROR);
        assert(errno == clock_error && out.kind == CU_PROCESS_SIGNALED && out.code == -77);
        clock_failure = 0; reap(p);
    }
    reset(); p = start(&a); wait_error = EIO;
    out.kind = CU_PROCESS_SIGNALED; out.code = -77;
    assert(process_wait_probe(p, 10, &out) == CU_PROCESS_ERROR && errno == EIO);
    assert(out.kind == CU_PROCESS_SIGNALED && out.code == -77);
    wait_error = 0; wait_interruptions = 3; terminal = 1;
    assert(process_wait_probe(p, 0, &out) == CU_PROCESS_DONE);
    assert(out.kind == CU_PROCESS_EXITED && out.code == 57);
    assert(clock_calls == 1);
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
static int bad_argument;

static void invalid_argument(void)
{
    CuArena a;
    CuProcess *p;
    CuProcessExit out;
    CuProcessWait waited;
    char name[] = "not-executed";
    char *args[2];
    int ok;

    cu_arena_init(&a); reset(); p = start(&a);
    args[0] = name; args[1] = NULL;
    waited = CU_PROCESS_RUNNING;
    ok = 0;
    switch (bad_argument) {
    case 0: ok = process_start_probe(NULL, &a, name, args, NULL); break;
    case 1: ok = process_start_probe(&p, NULL, name, args, NULL); break;
    case 2: ok = process_start_probe(&p, &a, NULL, args, NULL); break;
    case 3: ok = process_start_probe(&p, &a, name, NULL, NULL); break;
    case 4:
        args[0] = NULL;
        ok = process_start_probe(&p, &a, name, args, NULL);
        break;
    case 5: waited = process_wait_probe(NULL, 0, &out); break;
    case 6: waited = process_wait_probe(p, 0, NULL); break;
    case 7: ok = process_signal_probe(NULL, CU_PROCESS_TERMINATE); break;
    case 8: ok = process_signal_probe(p, (CuProcessSignal)-1); break;
    }
    (void)ok;
    (void)waited;
}

static void bad_reap(void)
{
    CuArena a;
    CuProcess *p;
    CuProcessExit out;

    cu_arena_init(&a); reset(); p = start(&a); wait_error = ECHILD;
    {
        CuProcessWait result;
        result = process_wait_probe(p, 0, &out);
        (void)result;
    }
}

#endif

int main(void)
{
    capture_exit_status();
    setup_failures();
    deadlines();
    wait_failures();
#ifdef CU_DEATH_TESTS
    for (bad_argument = 0; bad_argument < 9; bad_argument++)
        expect_abort(invalid_argument);
    expect_abort(bad_reap);
#endif
    return 0;
}
#endif
