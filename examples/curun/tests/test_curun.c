#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "curun.h"

#if defined(__linux__) && !defined(__cerb__) && !defined(__wasi__)
#include <signal.h>
#include <time.h>
#include <unistd.h>
#define TEST_NATIVE 1
#endif

static int invoke(char *args[], int count)
{
    return curun_main(count, args, NULL);
}

int main(int argc, char *argv[])
{
    char *args[12];
    int i;
    int missing_result;
    char *bad[] = {"", "-1", "+1", "1x", " 1", "18446744073709551616000000000000000000"};
#ifdef TEST_NATIVE
    struct timespec nap;
    if (argc == 2 && strcmp(argv[1], "--child-exit") == 0)
        return 7;
    if (argc == 2 && strcmp(argv[1], "--child-signal") == 0) {
        (void)raise(SIGTERM);
        return 99;
    }
    if (argc == 2 && strcmp(argv[1], "--child-wait") == 0) {
        (void)close(1);
        (void)close(2);
        nap.tv_sec = 2;
        nap.tv_nsec = 0;
        (void)nanosleep(&nap, NULL);
        return 99;
    }
    if (argc == 2 && strcmp(argv[1], "--child-output") == 0) {
        fputs("abcde", stdout);
        return 0;
    }
#else
    (void)argc;
    (void)argv;
#endif
    args[0] = "curun";
    args[1] = NULL;
    assert(invoke(args, 1) == 2);
    args[1] = "--help";
    args[2] = NULL;
    assert(invoke(args, 2) == 0);
    args[1] = "--unknown";
    assert(invoke(args, 2) == 2);
    args[1] = "--";
    assert(invoke(args, 2) == 2);
    args[2] = "bare-command";
    args[3] = NULL;
    assert(invoke(args, 3) == 2);
    for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
        args[1] = "--timeout-ms";
        args[2] = bad[i];
        args[3] = "--";
        args[4] = "/missing/not-executed";
        args[5] = NULL;
        assert(invoke(args, 5) == 2);
    }
    args[2] = "0";
    assert(invoke(args, 5) == 2);
    args[1] = "--stdout-limit";
    args[2] = "16777217";
    assert(invoke(args, 5) == 2);
    args[1] = "--stderr-limit";
    assert(invoke(args, 5) == 2);
    args[1] = "--stderr-limit";
    args[2] = NULL;
    assert(invoke(args, 2) == 2);
    args[1] = "--";
    args[2] = "/missing/curun-executable";
    args[3] = NULL;
    missing_result = invoke(args, 3);
    /* POSIX may report child-side exec failure as an owned exit127. */
    assert(missing_result == 125 || missing_result == 127);
#ifdef TEST_NATIVE
    args[1] = "--stdout-limit";
    args[2] = "0";
    args[3] = "--stderr-limit";
    args[4] = "0";
    args[5] = "--report";
    args[6] = "--";
    args[7] = argv[0];
    args[8] = "--child-exit";
    args[9] = NULL;
    assert(invoke(args, 9) == 7);
    args[8] = "--child-signal";
    assert(invoke(args, 9) == 128 + SIGTERM);
    args[1] = "--timeout-ms";
    args[2] = "50";
    args[3] = "--";
    args[4] = argv[0];
    args[5] = "--child-wait";
    args[6] = NULL;
    assert(invoke(args, 6) == 124);
    args[1] = "--stdout-limit";
    args[2] = "4";
    args[5] = "--child-output";
    assert(invoke(args, 6) == 125);
    args[2] = "5";
    assert(invoke(args, 6) == 0);
#endif
    return 0;
}
