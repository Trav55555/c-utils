#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(__cerb__) || !(defined(__unix__) || defined(__APPLE__) || defined(__wasi__))
int main(void) { return 0; }
#else
#include <sys/types.h>
#include <sys/stat.h>
#include "death.h"

#define cu_path_info path_info_probe
#define cu_fd_info fd_info_probe
#include "cu.h"

static int operation;
static int calls;
static int fails;
static int failure;
static mode_t mode;
static off_t bytes;

static int fill(struct stat *out, int which)
{
    operation = which;
    calls++;
    assert(calls < 10);
    if (fails) { errno = failure; return -1; }
    out->st_mode = mode;
    /* A nonregular size is intentionally uninitialized. It has no meaning
     * in this API and the implementation must not inspect it. */
    if (S_ISREG(mode)) out->st_size = bytes;
    return 0;
}

static int stat_probe(const char *path, struct stat *out)
{
    assert(strcmp(path, "fixture") == 0);
    return fill(out, 1);
}

static int lstat_probe(const char *path, struct stat *out)
{
    assert(strcmp(path, "fixture") == 0);
    return fill(out, 2);
}

static int fstat_probe(int fd, struct stat *out)
{
    assert(fd == 17);
    return fill(out, 3);
}

#define stat(p, s) stat_probe((p), (s))
#define lstat(p, s) lstat_probe((p), (s))
#define fstat(f, s) fstat_probe((f), (s))
#include "../src/cu_metadata.c"
#undef stat
#undef lstat
#undef fstat

static void reset(void)
{
    operation = 0; calls = 0; fails = 0; failure = 0;
    mode = S_IFREG; bytes = 0;
}

static void sentinel(CuFileInfo *out)
{
    out->kind = CU_DIR_LINK;
    out->size = cu_u64(123, 456);
}

static void same(const CuFileInfo *out)
{
    assert(out->kind == CU_DIR_LINK && out->size.hi == 123 && out->size.lo == 456);
}

static void test_errors(void)
{
    static const int errors[] = {ENOENT, ENOTDIR, ELOOP, EACCES, EIO, EINTR,
#ifdef EOVERFLOW
                                 EOVERFLOW,
#endif
                                 0};
    CuFileInfo out;
    CuInfoResult result;
    size_t i;

    for (i = 0; i < sizeof errors / sizeof errors[0]; i++) {
        reset(); fails = 1; failure = errors[i]; sentinel(&out); errno = EBUSY;
        result = path_info_probe("fixture", CU_PATH_NOFOLLOW, &out);
        assert(result == (errors[i] == ENOENT ? CU_INFO_MISSING : CU_INFO_ERROR));
        assert(errno == errors[i] && operation == 2 && calls == 1); same(&out);
        calls = 0; errno = EBUSY;
        result = path_info_probe("fixture", CU_PATH_FOLLOW, &out);
        assert(result == (errors[i] == ENOENT ? CU_INFO_MISSING : CU_INFO_ERROR));
        assert(errno == errors[i] && operation == 1 && calls == 1); same(&out);
        calls = 0; errno = EBUSY;
        assert(fd_info_probe(17, &out) == CU_INFO_ERROR);
        assert(errno == errors[i] && operation == 3 && calls == 1); same(&out);
    }
}

static void expect_size(unsigned long hi, unsigned long lo)
{
    CuFileInfo out;

    reset(); bytes = (off_t)hi;
    bytes *= 65536;
    bytes *= 65536;
    bytes += (off_t)lo;
    sentinel(&out);
    assert(fd_info_probe(17, &out) == CU_INFO_OK && operation == 3 && calls == 1);
    assert(out.kind == CU_DIR_FILE && out.size.hi == hi && out.size.lo == lo);
}

static void test_values(void)
{
    static const unsigned long small[] = {0, 1, 255, 256, 65535, 65536, 0x7FFFFFFFUL};
    static const mode_t kinds[] = {S_IFDIR, S_IFLNK, S_IFIFO};
    static const CuDirKind expected[] = {CU_DIR_DIR, CU_DIR_LINK, CU_DIR_OTHER};
    CuFileInfo out;
    size_t i;

    for (i = 0; i < sizeof small / sizeof small[0]; i++) expect_size(0, small[i]);
    if (sizeof(off_t) >= (size_t)8) {
        expect_size(0, 0x80000000UL);
        expect_size(0, 0xFFFFFFFFUL);
        expect_size(1, 0);
        expect_size(0x01234567UL, 0x89ABCDEFUL);
        expect_size(0x7FFFFFFFUL, 0xFFFFFFFFUL);
    }
    reset(); bytes = (off_t)-1; sentinel(&out); errno = 0;
    assert(fd_info_probe(17, &out) == CU_INFO_ERROR);
#ifdef EOVERFLOW
    assert(errno == EOVERFLOW);
#else
    assert(errno == ERANGE);
#endif
    same(&out);
    for (i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        reset(); mode = kinds[i]; sentinel(&out);
        assert(path_info_probe("fixture", CU_PATH_NOFOLLOW, &out) == CU_INFO_OK);
        assert(out.kind == expected[i] && out.size.hi == 0 && out.size.lo == 0);
    }
}

#ifdef CU_DEATH_TESTS
static int invalid;
static void invalid_arguments(void)
{
    CuFileInfo out;
    CuInfoResult result;

    reset();
    result = CU_INFO_OK;
    switch (invalid) {
    case 0: result = path_info_probe(NULL, CU_PATH_FOLLOW, &out); break;
    case 1: result = path_info_probe("fixture", CU_PATH_FOLLOW, NULL); break;
    case 2: result = path_info_probe("fixture", (CuPathFollow)-1, &out); break;
    case 3: result = fd_info_probe(17, NULL); break;
    }
    (void)result;
}
#endif

int main(void)
{
    test_errors();
    test_values();
#ifdef CU_DEATH_TESTS
    for (invalid = 0; invalid < 4; invalid++) expect_abort(invalid_arguments);
#endif
    return 0;
}
#endif
