/* Death tests: expect_abort runs body in a child process and requires it to
 * die with SIGABRT. Where there is no fork (WASI), CU_DEATH_TESTS stays
 * undefined and test files skip their death tests. Include this first, so
 * the feature-test macro precedes every system header. */
#ifndef CU_TESTS_DEATH_H
#define CU_TESTS_DEATH_H

#if defined(__unix__) && !defined(__wasi__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define CU_DEATH_TESTS 1

static void expect_abort(void (*body)(void))
{
    pid_t pid;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        body();
        _exit(0);
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif

#endif
