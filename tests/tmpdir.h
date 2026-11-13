/* Per-run scratch directory for tests that touch the file system. The
 * directory is created fresh under TMPDIR (else /tmp), trying numbered names
 * until mkdir succeeds, so a leftover from an interrupted run or a
 * concurrent run never collides. The test puts its files inside and calls
 * tmpdir_remove at the end of a passing run. Include this after death.h (or
 * first, if the file does not use death.h), so the feature-test macro
 * precedes every system header. Cerberus models no mkdir: there the
 * "directory" is TMPDIR itself and removal does nothing, so a test keeps
 * unique-enough file names of its own. */
#ifndef CU_TESTS_TMPDIR_H
#define CU_TESTS_TMPDIR_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __cerb__
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* Creates the directory and writes its path to out, which holds cap bytes. */
static void tmpdir_make(char *out, size_t cap, const char *prefix)
{
    const char *tmp;
#ifndef __cerb__
    unsigned i;
#endif

    tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    assert(strlen(tmp) + strlen(prefix) + 16 < cap);
#ifdef __cerb__
    (void)prefix;
    strcpy(out, tmp);
#else
    for (i = 0; i < 1000; i++) {
        sprintf(out, "%s/%s-%u", tmp, prefix, i);
        if (mkdir(out, 0700) == 0)
            break;
        assert(errno == EEXIST);
    }
    assert(i < 1000);
#endif
}

/* Removes the directory, which must be empty. */
static void tmpdir_remove(const char *dir)
{
#ifdef __cerb__
    (void)dir;
#else
    assert(rmdir(dir) == 0);
#endif
}

#endif
