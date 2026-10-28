/* Death tests: expect_abort runs body in a child process and requires it to
 * die with SIGABRT. A body that does not is named, with how it ended, before
 * the assertion fails: the assertion alone does not say which death test it
 * was. Where there is no fork (WASI), CU_DEATH_TESTS stays undefined and
 * test files skip their death tests. Include this first, so
 * the feature-test macro precedes every system header. */
#ifndef CU_TESTS_DEATH_H
#define CU_TESTS_DEATH_H

#if defined(__unix__) && !defined(__wasi__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#define CU_DEATH_TESTS 1

#define expect_abort(body) expect_abort_named((body), #body)

static void expect_abort_named(void (*body)(void), const char *name)
{
    pid_t pid;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
#if defined(__linux__)
        /* No core dump: a core_pattern pipe (apport, systemd-coredump)
         * ignores ulimit -c and can take seconds per expected abort. */
        prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
        body();
        _exit(0);
    }
    assert(waitpid(pid, &status, 0) == pid);
    if (WIFEXITED(status))
        fprintf(stderr, "expect_abort: %s exited with status %d\n", name, WEXITSTATUS(status));
    else if (WIFSIGNALED(status) && WTERMSIG(status) != SIGABRT)
        fprintf(stderr, "expect_abort: %s died of signal %d\n", name, WTERMSIG(status));
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif

#endif
