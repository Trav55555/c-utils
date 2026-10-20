#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
int main(void) { return 0; }
#else
#include <fcntl.h>
#ifndef F_DUPFD_CLOEXEC
int main(void) { return 0; }
#else
#include <stdarg.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "death.h"

#define cu_process_start stdio_start_probe
#define cu_process_start_redirected stdio_redirect_probe
#define cu_process_wait stdio_wait_probe
#define cu_process_signal stdio_signal_probe
#include "cu.h"

/* These are model descriptors, never kernel descriptors. Each duplicate has
 * its own identity even when two fields name the same open-file description. */
static int sources[3];
static int live[3];
static int copied[3];
static int child[3];
static int assigned[3];
static int action_closes[3];
static int checks, duplicates, closes, inits, destroys, dup_actions, close_actions;
static int actions_live, spawns, waits, native_status;
static int failure_kind, failure_at, operation_error;
static int explicit_count;

enum { FAIL_DUP = 1, FAIL_INIT, FAIL_DUP2, FAIL_CLOSE_ACTION, FAIL_SPAWN, FAIL_CHECK };

static int fail(int kind, int ordinal)
{
    return failure_kind == kind && failure_at == ordinal;
}

static int duplicate_probe(int fd, int command, ...)
{
    va_list ap;
    int minimum;
    int index;
    int field;
    int n;

    if (command == F_GETFD) {
        checks++;
        assert(duplicates == 0 && checks <= explicit_count);
        if (fail(FAIL_CHECK, checks)) { errno = operation_error; return -1; }
        return FD_CLOEXEC;
    }
    va_start(ap, command);
    minimum = va_arg(ap, int);
    va_end(ap);
    assert(command == F_DUPFD_CLOEXEC && minimum == 3);
    assert(checks == explicit_count);
    assert(dup_actions == 0 && close_actions == 0 && spawns == 0);
    index = duplicates++;
    assert(index < 3);
    if (fail(FAIL_DUP, duplicates)) { errno = operation_error; return -1; }
    n = 0;
    field = 0;
    while (field < 3) {
        if (sources[field] != CU_PROCESS_INHERIT) {
            if (n == index) break;
            n++;
        }
        field++;
    }
    assert(field < 3 && fd == sources[field]);
    copied[index] = fd;
    live[index] = 1;
    return 40 + index;
}

static int close_probe(int fd)
{
    int index;

    index = fd - 40;
    assert(index >= 0 && index < 3 && live[index]);
    live[index] = 0;
    closes++;
    /* A failed close may already have consumed the descriptor. No retries. */
    errno = EINTR;
    return -1;
}

static int actions_init_probe(posix_spawn_file_actions_t *actions)
{
    (void)actions;
    inits++;
    assert(!actions_live);
    if (fail(FAIL_INIT, inits)) { errno = EBUSY; return operation_error; }
    actions_live = 1;
    return 0;
}

static int dup2_probe(posix_spawn_file_actions_t *actions, int fd, int target)
{
    int index;

    (void)actions;
    index = fd - 40;
    assert(actions_live && duplicates == explicit_count);
    assert(index >= 0 && index < explicit_count && live[index]);
    assert(target >= 0 && target <= 2 && !assigned[target]);
    dup_actions++;
    if (fail(FAIL_DUP2, dup_actions)) { errno = EBUSY; return operation_error; }
    assigned[target] = 1;
    child[target] = copied[index];
    return 0;
}

static int addclose_probe(posix_spawn_file_actions_t *actions, int fd)
{
    int index;

    (void)actions;
    index = fd - 40;
    assert(actions_live && index >= 0 && index < explicit_count && live[index]);
    assert(!action_closes[index]);
    close_actions++;
    if (fail(FAIL_CLOSE_ACTION, close_actions)) { errno = EBUSY; return operation_error; }
    action_closes[index] = 1;
    return 0;
}

static int actions_destroy_probe(posix_spawn_file_actions_t *actions)
{
    (void)actions;
    assert(actions_live);
    actions_live = 0;
    destroys++;
    errno = ERANGE;
    return EIO;
}

static int spawn_probe(pid_t *pid, const char *path,
                       const posix_spawn_file_actions_t *actions,
                       const posix_spawnattr_t *attr,
                       char *const argv[], char *const envp[])
{
    int i;
    int n;

    assert(path != NULL && argv[0] != NULL && attr != NULL);
    assert(envp != NULL && envp[0] == NULL);
    assert((actions != NULL) == (explicit_count != 0));
    n = 0;
    for (i = 0; i < 3; i++) {
        if (sources[i] != CU_PROCESS_INHERIT) {
            assert(assigned[i] && child[i] == sources[i]);
            assert(live[n] && action_closes[n]);
            n++;
        } else {
            assert(!assigned[i] && child[i] == i);
        }
    }
    assert(n == explicit_count && dup_actions == n && close_actions == n);
    spawns++;
    if (fail(FAIL_SPAWN, spawns)) { errno = EBUSY; return operation_error; }
    *pid = (pid_t)4242;
    return 0;
}

static pid_t wait_probe(pid_t pid, int *status, int options)
{
    assert(pid == (pid_t)4242 && options == WNOHANG);
    waits++;
    *status = native_status;
    return pid;
}

static int kill_probe(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    abort(); /* Never send a native signal to the model PID. */
    return -1;
}

#define fcntl duplicate_probe
#define close close_probe
#define posix_spawn_file_actions_init actions_init_probe
#define posix_spawn_file_actions_adddup2 dup2_probe
#define posix_spawn_file_actions_addclose addclose_probe
#define posix_spawn_file_actions_destroy actions_destroy_probe
#define posix_spawn spawn_probe
#define waitpid wait_probe
#define kill kill_probe
#include "../src/cu_process.c"
#undef fcntl
#undef close
#undef posix_spawn_file_actions_init
#undef posix_spawn_file_actions_adddup2
#undef posix_spawn_file_actions_addclose
#undef posix_spawn_file_actions_destroy
#undef posix_spawn
#undef waitpid
#undef kill

static void reset(int input, int output, int error)
{
    int i;

    sources[0] = input; sources[1] = output; sources[2] = error;
    explicit_count = 0;
    for (i = 0; i < 3; i++) {
        live[i] = copied[i] = assigned[i] = action_closes[i] = 0;
        child[i] = i;
        if (sources[i] != CU_PROCESS_INHERIT) explicit_count++;
    }
    checks = duplicates = closes = inits = destroys = dup_actions = close_actions = 0;
    actions_live = spawns = waits = 0;
    failure_kind = failure_at = 0;
    operation_error = EIO;
}

static void run(int refusal)
{
    CuArena arena;
    CuProcess sentinel;
    CuProcess *p;
    CuProcessStdio stdio;
    CuProcessExit status;
    char name[] = "not-executed";
    char *argv[2];
    int ok;
    int saved;
    int i;

    cu_arena_init(&arena);
    if (refusal) cu_arena_fail_at(&arena, 0);
    stdio.input = sources[0]; stdio.output = sources[1]; stdio.error = sources[2];
    argv[0] = name; argv[1] = NULL;
    p = &sentinel;
    errno = EDOM;
    ok = stdio_redirect_probe(&p, &arena, name, argv, NULL, &stdio);
    saved = errno;
    assert(!actions_live && destroys == (inits != 0 && failure_kind != FAIL_INIT));
    for (i = 0; i < 3; i++) assert(!live[i]);
    assert(closes == duplicates - (failure_kind == FAIL_DUP ? 1 : 0));
    if (refusal || failure_kind != 0) {
        assert(!ok && p == &sentinel);
        assert(saved == (refusal ? ENOMEM : operation_error));
        assert(spawns == (failure_kind == FAIL_SPAWN ? 1 : 0));
    } else {
        assert(ok && p != &sentinel && spawns == 1);
        assert(stdio_wait_probe(p, 0, &status) == CU_PROCESS_DONE);
        assert(status.kind == CU_PROCESS_EXITED && status.code == 57 && waits == 1);
    }
    assert(stdio.input == sources[0] && stdio.output == sources[1] && stdio.error == sources[2]);
    cu_arena_release(&arena);
}

#ifdef CU_DEATH_TESTS
static int bad_argument;
static void invalid_argument(void)
{
    CuArena a;
    CuProcess *p;
    CuProcessStdio s;
    char name[] = "not-executed";
    char *args[2];
    int ok;

    cu_arena_init(&a);
    s.input = s.output = s.error = CU_PROCESS_INHERIT;
    args[0] = bad_argument == 4 ? NULL : name; args[1] = NULL;
    ok = stdio_redirect_probe(bad_argument == 0 ? NULL : &p,
                              bad_argument == 1 ? NULL : &a,
                              bad_argument == 2 ? NULL : name,
                              bad_argument == 3 ? NULL : args, NULL,
                              bad_argument == 5 ? NULL : &s);
    (void)ok;
}
#endif

int main(void)
{
    pid_t pid;
    pid_t result;
    int kind;
    int ordinal;
    int zero;
    int field;
    int values[3];

    /* Keep the fixture buildable against the initial ENOSYS stub too. */
    (void)duplicate_probe;
    (void)close_probe;
    (void)actions_init_probe;
    (void)dup2_probe;
    (void)addclose_probe;
    (void)actions_destroy_probe;

    /* Obtain terminal status independently, without guessing wait bit layouts. */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) _exit(57);
    do { result = waitpid(pid, &native_status, 0); } while (result < 0 && errno == EINTR);
    assert(result == pid && WIFEXITED(native_status) && WEXITSTATUS(native_status) == 57);

    reset(1, 2, 0); run(0); /* Simultaneous three-way standard-stream cycle. */
    reset(0, 1, 2); run(0); /* Explicit identity still needs dup2 (CLOEXEC). */
    reset(17, 17, 17); run(0); /* Merge without transferring caller ownership. */
    reset(-1, -1, -1); run(0);
    for (field = 0; field < 3; field++) {
        values[0] = values[1] = values[2] = CU_PROCESS_INHERIT;
        values[field] = 17;
        reset(values[0], values[1], values[2]); run(0);
    }
    for (kind = FAIL_DUP; kind <= FAIL_CHECK; kind++) {
        for (ordinal = 1; ordinal <= ((kind == FAIL_DUP || kind == FAIL_DUP2 || kind == FAIL_CLOSE_ACTION || kind == FAIL_CHECK) ? 3 : 1); ordinal++) {
            /* errno zero is meaningful for -1-returning syscalls, not for
             * posix_spawn APIs whose zero return denotes success. */
            for (zero = 0; zero <= (kind == FAIL_DUP || kind == FAIL_CHECK ? 1 : 0); zero++) {
                reset(1, 2, 0);
                failure_kind = kind; failure_at = ordinal;
                operation_error = zero ? 0 : EIO;
                run(0);
            }
        }
    }
    reset(1, 2, 0); run(1);
    reset(-1, -1, -1); run(1);
#ifdef CU_DEATH_TESTS
    for (bad_argument = 0; bad_argument < 6; bad_argument++) expect_abort(invalid_argument);
#endif
    return 0;
}
#endif
#endif
