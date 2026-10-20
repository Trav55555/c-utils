#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
int main(void)
{
    CuArena a;
    CuProcess *p;
    char arg[] = "program";
    char *args[2];

    args[0] = arg;
    args[1] = NULL;
    cu_arena_init(&a);
    p = NULL;
    errno = 0;
    assert(!cu_process_start(&p, &a, arg, args, NULL));
    assert(p == NULL);
#ifdef ENOSYS
    assert(errno == ENOSYS);
#else
    assert(errno == EINVAL);
#endif
    cu_arena_release(&a);
    return 0;
}
#else
#include <signal.h>
#include <unistd.h>

static char helper[] = "--cu-process-child";
static char literal[] = "one argument; $(not-a-command) 'quoted' *";

static int child(int argc, char **argv)
{
    struct sigaction action;
    sigset_t mask;
    unsigned long fd;
    char ready;

    if (argc == 4 && strcmp(argv[2], "literal") == 0)
        return strcmp(argv[3], literal) == 0 ? 37 : 38;
    if (argc == 3 && strcmp(argv[2], "127") == 0)
        return 127;
    if (argc == 3 && strcmp(argv[2], "zero") == 0)
        return 0;
    if (argc != 4 || strcmp(argv[2], "ready") != 0)
        return 45;
    if (sigaction(SIGTERM, NULL, &action) != 0 || action.sa_handler != SIG_DFL)
        return 46;
    if (sigprocmask(SIG_SETMASK, NULL, &mask) != 0 || sigismember(&mask, SIGTERM) != 0)
        return 47;
    if (!cu_parse_ulong(cu_str_cstr(argv[3]), 10, &fd))
        return 48;
    (void)alarm(8);
    ready = 'R';
    if (write((int)fd, &ready, 1) != 1)
        return 49;
    (void)close((int)fd);
    for (;;)
        (void)pause();
}

static void exit_is(CuProcess *p, int code)
{
    CuProcessExit status;

    assert(cu_process_wait(p, 5000, &status) == CU_PROCESS_DONE);
    assert(status.kind == CU_PROCESS_EXITED && status.code == code);
    status.code = -999;
    assert(cu_process_wait(p, 0, &status) == CU_PROCESS_DONE);
    assert(status.kind == CU_PROCESS_EXITED && status.code == code);
    errno = 0;
    assert(!cu_process_signal(p, CU_PROCESS_KILL) && errno == ESRCH);
}

/* Observe exec's environment through an uninstrumented POSIX utility.
 * A profiled copy of this test changes its own environment before main. */
static void environment_is(CuArena *a, char *const envp[], const char *expected, size_t length)
{
    CuProcess *p;
    char usr_env[] = "/usr/bin/env";
    char bin_env[] = "/bin/env";
    char *path;
    char *args[2];
    char output[128];
    size_t used;
    ssize_t count;
    int pipefd[2];
    CuProcessStdio io;
    int started;
    int overflow;

    path = access(usr_env, X_OK) == 0 ? usr_env : bin_env;
    assert(access(path, X_OK) == 0);
    args[0] = path;
    args[1] = NULL;
    assert(pipe(pipefd) == 0);
    io.input = CU_PROCESS_INHERIT;
    io.output = pipefd[1];
    io.error = CU_PROCESS_INHERIT;
    started = cu_process_start_redirected(&p, a, path, args, envp, &io);
    assert(close(pipefd[1]) == 0);
    assert(started);
    used = 0;
    overflow = 0;
    for (;;) {
        if (used == sizeof(output)) { overflow = 1; break; }
        count = read(pipefd[0], output + used, sizeof(output) - used);
        if (count < 0 && errno == EINTR)
            continue;
        assert(count >= 0);
        if (count == 0)
            break;
        used += (size_t)count;
    }
    assert(close(pipefd[0]) == 0);
    if (overflow) {
        if (!cu_process_signal(p, CU_PROCESS_KILL))
            assert(errno == ESRCH);
    }
    exit_is(p, 0);
    assert(!overflow && used == length);
    assert(memcmp(output, expected, length) == 0);
}

static void test_arguments(const char *path, char *name)
{
    CuArena a;
    CuProcess *p;
    char mode[] = "literal";
    char status_mode[] = "127";
    char zero_mode[] = "zero";
    char environment[] = "CU_PROCESS_TEST=literal";
    char *args[5];
    char *env[2];

    cu_arena_init(&a);
    assert(setenv("CU_PROCESS_PARENT_ONLY", "must-not-inherit", 1) == 0);
    args[0] = name;
    args[1] = helper;
    args[2] = mode;
    args[3] = literal;
    args[4] = NULL;
    assert(cu_process_start(&p, &a, path, args, NULL));
    exit_is(p, 37);
    env[0] = environment;
    env[1] = NULL;
    environment_is(&a, env, "CU_PROCESS_TEST=literal\n", (size_t)24);
    environment_is(&a, NULL, "", 0);
    args[2] = status_mode;
    args[3] = NULL;
    assert(cu_process_start(&p, &a, path, args, NULL));
    exit_is(p, 127);
    args[2] = zero_mode;
    assert(cu_process_start(&p, &a, path, args, NULL));
    exit_is(p, 0);
    assert(unsetenv("CU_PROCESS_PARENT_ONLY") == 0);
    cu_arena_release(&a);
}

static void test_start_errors(const char *path, char *name)
{
    CuArena a;
    CuProcess *p;
    char *args[2];

    args[0] = name;
    args[1] = NULL;
    cu_arena_init(&a);
    cu_arena_fail_at(&a, 0);
    p = NULL;
    errno = 0;
    assert(!cu_process_start(&p, &a, path, args, NULL));
    assert(errno == ENOMEM && p == NULL);
    cu_arena_release(&a);
    cu_arena_init(&a);
    p = NULL;
    errno = 0;
    /* POSIX permits either immediate spawn failure or child exit 127. */
    if (cu_process_start(&p, &a, "/cu-process-nonexistent/program", args, NULL))
        exit_is(p, 127);
    else
        assert(p == NULL && errno != 0);
    cu_arena_release(&a);
}

static void test_signal_and_wait(const char *path, char *name, CuProcessSignal signal)
{
    CuArena a;
    CuBuilder b;
    CuStr fd_text;
    CuProcess *p;
    CuProcessExit status;
    struct sigaction ignored;
    struct sigaction old;
    struct sigaction observed;
    sigset_t blocked;
    sigset_t old_mask;
    sigset_t observed_mask;
    int descriptors[2];
    ssize_t got;
    char mode[] = "ready";
    char *args[5];
    char ready;

    cu_arena_init(&a);
    assert(pipe(descriptors) == 0);
    cu_sb_init(&b, &a);
    cu_sb_ulong(&b, (unsigned long)descriptors[1]);
    cu_sb_char(&b, '\0');
    assert(cu_sb_finish(&b, &fd_text));
    args[0] = name;
    args[1] = helper;
    args[2] = mode;
    args[3] = b.data;
    args[4] = NULL;
    ignored.sa_handler = SIG_IGN;
    ignored.sa_flags = 0;
    assert(sigemptyset(&ignored.sa_mask) == 0);
    assert(sigaction(SIGTERM, &ignored, &old) == 0);
    assert(sigemptyset(&blocked) == 0 && sigaddset(&blocked, SIGTERM) == 0);
    assert(sigprocmask(SIG_BLOCK, &blocked, &old_mask) == 0);
    assert(cu_process_start(&p, &a, path, args, NULL));
    assert(sigaction(SIGTERM, NULL, &observed) == 0 && observed.sa_handler == SIG_IGN);
    assert(sigprocmask(SIG_SETMASK, NULL, &observed_mask) == 0);
    assert(sigismember(&observed_mask, SIGTERM) == 1);
    assert(sigprocmask(SIG_SETMASK, &old_mask, NULL) == 0);
    assert(sigaction(SIGTERM, &old, NULL) == 0);
    assert(close(descriptors[1]) == 0);
    do {
        got = read(descriptors[0], &ready, 1);
    } while (got < 0 && errno == EINTR);
    assert(got == 1 && ready == 'R');
    assert(close(descriptors[0]) == 0);
    status.kind = CU_PROCESS_EXITED;
    status.code = -999;
    assert(cu_process_wait(p, 0, &status) == CU_PROCESS_RUNNING);
    assert(status.kind == CU_PROCESS_EXITED && status.code == -999);
    assert(cu_process_wait(p, 15, &status) == CU_PROCESS_RUNNING);
    assert(status.kind == CU_PROCESS_EXITED && status.code == -999);
    assert(cu_process_signal(p, signal));
    assert(cu_process_wait(p, 5000, &status) == CU_PROCESS_DONE);
    assert(status.kind == CU_PROCESS_SIGNALED);
    assert(status.code == (signal == CU_PROCESS_TERMINATE ? SIGTERM : SIGKILL));
    assert(cu_process_wait(p, 0, &status) == CU_PROCESS_DONE);
    assert(status.kind == CU_PROCESS_SIGNALED);
    assert(status.code == (signal == CU_PROCESS_TERMINATE ? SIGTERM : SIGKILL));
    cu_arena_release(&a);
}

static void test_reaping_policy(const char *path, char *name)
{
    CuArena a;
    CuProcess *p;
    struct sigaction old;
    struct sigaction action;
    char *args[2];

    cu_arena_init(&a);
    args[0] = name;
    args[1] = NULL;
    action.sa_handler = SIG_IGN;
    action.sa_flags = 0;
    assert(sigemptyset(&action.sa_mask) == 0);
    assert(sigaction(SIGCHLD, &action, &old) == 0);
    p = NULL;
    errno = 0;
    assert(!cu_process_start(&p, &a, path, args, NULL));
    assert(p == NULL && errno == EINVAL);
    assert(sigaction(SIGCHLD, &old, NULL) == 0);
#ifdef SA_NOCLDWAIT
    action.sa_handler = SIG_DFL;
    action.sa_flags = SA_NOCLDWAIT;
    assert(sigaction(SIGCHLD, &action, &old) == 0);
    p = NULL;
    errno = 0;
    assert(!cu_process_start(&p, &a, path, args, NULL));
    assert(p == NULL && errno == EINVAL);
    assert(sigaction(SIGCHLD, &old, NULL) == 0);
#endif
    cu_arena_release(&a);
}

typedef struct ProcessJob {
    const char *path;
    char *name;
    int done;
} ProcessJob;

static void process_job(void *context)
{
    ProcessJob *job;
    CuArena a;
    CuProcess *p;
    char mode[] = "zero";
    char *args[4];

    job = (ProcessJob *)context;
    cu_arena_init(&a);
    args[0] = job->name;
    args[1] = helper;
    args[2] = mode;
    args[3] = NULL;
    assert(cu_process_start(&p, &a, job->path, args, NULL));
    exit_is(p, 0);
    cu_arena_release(&a);
    job->done = 1;
}

static void test_concurrent(const char *path, char *name)
{
    CuArena a;
    ProcessJob *jobs;
    CuThread *threads[3];
    size_t i;

    cu_arena_init(&a);
    jobs = CU_PUSH(&a, ProcessJob, 3);
    assert(jobs != NULL);
    for (i = 0; i < 3; i++) {
        jobs[i].path = path;
        jobs[i].name = name;
        jobs[i].done = 0;
        assert(cu_thread_start(&threads[i], &a, 0, process_job, &jobs[i]));
    }
    for (i = 0; i < 3; i++) {
        cu_thread_join(threads[i]);
        assert(jobs[i].done);
    }
    cu_arena_release(&a);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], helper) == 0)
        return child(argc, argv);
    assert(argc > 0 && argv[0] != NULL);
    test_arguments(argv[0], argv[0]);
    test_start_errors(argv[0], argv[0]);
    test_signal_and_wait(argv[0], argv[0], CU_PROCESS_TERMINATE);
    test_signal_and_wait(argv[0], argv[0], CU_PROCESS_KILL);
    test_reaping_policy(argv[0], argv[0]);
    test_concurrent(argv[0], argv[0]);
    return 0;
}
#endif
