#include "death.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

#ifdef __cerb__
/* Cerberus models no file descriptors, so the platform reports no POSIX
 * files; the output tests run natively and under wasm. */
int main(void)
{
    CuAppend f;

    assert(!cu_append_open(&f, "x", CU_APPEND_CREATE));
    assert(cu_append_errno(&f) == ENOSYS);
    assert(!cu_append_write(&f, cu_str_cstr("a")));
    assert(!cu_append_close(&f));
    errno = 0;
    assert(!cu_path_rename("x", "y") && errno == ENOSYS);
    errno = 0;
    assert(!cu_path_remove("x") && errno == ENOSYS);
    errno = 0;
    assert(!cu_dir_make("x") && errno == ENOSYS);
    errno = 0;
    assert(!cu_dir_sync("x") && errno == ENOSYS);
    return 0;
}
#else

#include <sys/types.h>

static char root[512];

/* Test files go in TMPDIR (dcheck gives wasm tests one too), else /tmp. */
static void path_of(char *out, size_t cap, const char *rel)
{
    assert(strlen(root) + 1 + strlen(rel) < cap);
    sprintf(out, "%s/%s", root, rel);
}

/* The file's bytes, which must equal want. */
static void expect_contents(const char *path, const char *want, size_t n)
{
    CuArena a;
    CuStr s;

    cu_arena_init(&a);
    assert(cu_file_read(&a, path, &s));
    assert(s.len == n && (n == 0 || memcmp(s.ptr, want, n) == 0));
    cu_arena_release(&a);
}

/* Writes append, a reopened file keeps its bytes and appends after them,
 * and TRUNCATE empties it. NUL bytes go through. */
static void test_write_and_reopen(void)
{
    char p[600];
    CuAppend f;

    path_of(p, sizeof p, "log");
    assert(cu_append_open(&f, p, CU_APPEND_CREATE));
    assert(cu_append_write(&f, cu_str_n("ab\0c", 4)));
    assert(cu_append_write(&f, cu_str_empty()));
    assert(cu_append_write(&f, cu_str_cstr("de")));
    assert(cu_append_sync(&f));
    assert(cu_append_close(&f));
    assert(cu_append_close(&f));
    expect_contents(p, "ab\0cde", 6);

    assert(cu_append_open(&f, p, 0));
    assert(cu_append_write(&f, cu_str_cstr("f")));
    assert(cu_append_close(&f));
    expect_contents(p, "ab\0cdef", 7);

    assert(cu_append_open(&f, p, CU_APPEND_TRUNCATE));
    assert(cu_append_write(&f, cu_str_cstr("g")));
    assert(cu_append_close(&f));
    expect_contents(p, "g", 1);
    assert(cu_path_remove(p));
}

/* A large write goes out whole. */
static void test_large_write(void)
{
    char p[600];
    CuAppend f;
    char *buf;
    size_t n;
    size_t i;

    n = (size_t)1 << 20;
    buf = (char *)malloc(n);
    assert(buf != NULL);
    for (i = 0; i < n; i++)
        buf[i] = (char)(unsigned char)(i * 7 + 3);
    path_of(p, sizeof p, "large");
    assert(cu_append_open(&f, p, CU_APPEND_CREATE | CU_APPEND_EXCLUSIVE));
    assert(cu_append_write(&f, cu_str_n(buf, n)));
    assert(cu_append_sync(&f));
    assert(cu_append_close(&f));
    expect_contents(p, buf, n);
    assert(cu_path_remove(p));
    free(buf);
}

/* Truncating cuts the tail; the next write lands at the new end, not at
 * the old offset, and extending fills with zeros. */
static void test_truncate(void)
{
    char p[600];
    CuAppend f;

    path_of(p, sizeof p, "trunc");
    assert(cu_append_open(&f, p, CU_APPEND_CREATE));
    assert(cu_append_write(&f, cu_str_cstr("abcdef")));
    assert(cu_append_truncate(&f, 2));
    assert(cu_append_write(&f, cu_str_cstr("X")));
    assert(cu_append_truncate(&f, 5));
    assert(cu_append_write(&f, cu_str_cstr("Y")));
    assert(cu_append_close(&f));
    expect_contents(p, "abX\0\0Y", 6);
    assert(cu_path_remove(p));
}

/* Opening fails with the system's errno, and the failure is sticky. */
static void test_open_fails(void)
{
    char p[600];
    CuAppend f;

    path_of(p, sizeof p, "missing");
    assert(!cu_append_open(&f, p, 0));
    assert(cu_append_errno(&f) == ENOENT);
    assert(!cu_append_write(&f, cu_str_cstr("a")));
    assert(!cu_append_sync(&f));
    assert(!cu_append_truncate(&f, 0));
    assert(!cu_append_lock(&f));
    assert(cu_append_errno(&f) == ENOENT);
    assert(!cu_append_close(&f));
    assert(!cu_append_close(&f));
    assert(cu_append_errno(&f) == ENOENT);

    path_of(p, sizeof p, "dir/inside");
    assert(!cu_append_open(&f, p, CU_APPEND_CREATE));
    assert(cu_append_errno(&f) == ENOENT);
    assert(!cu_append_close(&f));

    /* EXCLUSIVE refuses an existing file. */
    path_of(p, sizeof p, "exists");
    assert(cu_append_open(&f, p, CU_APPEND_CREATE | CU_APPEND_EXCLUSIVE));
    assert(cu_append_close(&f));
    assert(!cu_append_open(&f, p, CU_APPEND_CREATE | CU_APPEND_EXCLUSIVE));
    assert(cu_append_errno(&f) == EEXIST);
    assert(!cu_append_close(&f));
    assert(cu_path_remove(p));

    /* A directory cannot be opened for writing. */
    assert(!cu_append_open(&f, root, 0));
    assert(cu_append_errno(&f) == EISDIR);
    assert(!cu_append_close(&f));
}

/* Truncating to a length the system cannot hold fails, and that is
 * sticky too. Only reachable where file offsets are no wider than size_t
 * (64-bit systems); i386 with large files and wasm32 have 64-bit offsets,
 * where it would make a 4 GiB sparse file instead. */
static void test_truncate_too_far(void)
{
    char p[600];
    CuAppend f;
    size_t big;

    big = (size_t)-1;
    path_of(p, sizeof p, "far");
    assert(cu_append_open(&f, p, CU_APPEND_CREATE));
    assert(!cu_append_truncate(&f, big));
    assert(cu_append_errno(&f) == EFBIG || cu_append_errno(&f) == EINVAL);
    assert(!cu_append_write(&f, cu_str_cstr("a")));
    assert(!cu_append_close(&f));
    assert(cu_path_remove(p));
}

#if defined(__linux__)
/* A write the device refuses fails with its errno, and everything after
 * it fails the same way, including a sync that would otherwise succeed. */
static void test_write_fails(void)
{
    CuAppend f;

    assert(cu_append_open(&f, "/dev/full", 0));
    assert(!cu_append_write(&f, cu_str_cstr("a")));
    assert(cu_append_errno(&f) == ENOSPC);
    assert(!cu_append_sync(&f));
    assert(!cu_append_write(&f, cu_str_cstr("b")));
    assert(cu_append_errno(&f) == ENOSPC);
    assert(!cu_append_close(&f));
}
#endif

/* A second open of a locked file cannot lock it, even in this process,
 * and can once the first is closed. A failed lock leaves it usable. */
static void test_lock(void)
{
    char p[600];
    CuAppend f;
    CuAppend g;

    path_of(p, sizeof p, "locked");
    assert(cu_append_open(&f, p, CU_APPEND_CREATE));
    assert(cu_append_open(&g, p, 0));
#ifdef __wasi__
    errno = 0;
    assert(!cu_append_lock(&f) && errno == ENOSYS);
    assert(cu_append_errno(&f) == 0);
    assert(cu_append_write(&f, cu_str_cstr("ok")));
    assert(cu_append_close(&f));
#else
    assert(cu_append_lock(&f));
    errno = 0;
    assert(!cu_append_lock(&g));
    assert(errno == EWOULDBLOCK || errno == EAGAIN);
    assert(cu_append_errno(&g) == 0);
    assert(cu_append_write(&g, cu_str_cstr("ok")));
    assert(cu_append_close(&f));
    assert(cu_append_lock(&g));
#endif
    assert(cu_append_close(&g));
    expect_contents(p, "ok", 2);
    assert(cu_path_remove(p));
}

/* cu_dir_sync(path) succeeds, or on WASI reports that it cannot. */
static int dir_sync_works(const char *path)
{
#ifdef __wasi__
    errno = 0;
    return !cu_dir_sync(path) && errno == ENOSYS;
#else
    return cu_dir_sync(path);
#endif
}

/* Rename replaces its target, remove removes, and directory operations
 * report the system's errno. */
static void test_paths(void)
{
    char p[600];
    char q[600];
    CuAppend f;

    path_of(p, sizeof p, "from");
    path_of(q, sizeof q, "to");
    assert(cu_append_open(&f, p, CU_APPEND_CREATE));
    assert(cu_append_write(&f, cu_str_cstr("new")));
    assert(cu_append_close(&f));
    assert(cu_append_open(&f, q, CU_APPEND_CREATE));
    assert(cu_append_write(&f, cu_str_cstr("old")));
    assert(cu_append_close(&f));
    assert(cu_path_rename(p, q));
    assert(dir_sync_works(root));
    expect_contents(q, "new", 3);
    errno = 0;
    assert(!cu_path_rename(p, q) && errno == ENOENT);
    assert(cu_path_remove(q));
    errno = 0;
    assert(!cu_path_remove(q) && errno == ENOENT);

    path_of(p, sizeof p, "sub");
    assert(cu_dir_make(p));
    errno = 0;
    assert(!cu_dir_make(p) && errno == EEXIST);
    assert(dir_sync_works(p));
    path_of(q, sizeof q, "sub/file");
    assert(cu_append_open(&f, q, CU_APPEND_CREATE));
    assert(cu_append_close(&f));
#ifndef __wasi__
    errno = 0;
    assert(!cu_dir_sync(q) && errno == ENOTDIR);
#endif
    assert(cu_path_remove(q));
    errno = 0;
    assert(!cu_path_remove(p));
    path_of(q, sizeof q, "sub/missing/deeper");
    errno = 0;
    assert(!cu_dir_make(q) && errno == ENOENT);
#ifndef __wasi__
    errno = 0;
    assert(!cu_dir_sync(q) && errno == ENOENT);
#endif
    /* The directory itself goes with the system's rmdir. */
    assert(remove(p) == 0);
}

#ifdef CU_DEATH_TESTS
static void open_null_path(void)
{
    CuAppend f;
    int ok;

    ok = cu_append_open(&f, NULL, 0);
    (void)ok;
}

static void write_after_close(void)
{
    char p[600];
    CuAppend f;
    int ok;

    path_of(p, sizeof p, "closed");
    assert(cu_append_open(&f, p, CU_APPEND_CREATE));
    assert(cu_append_close(&f));
    ok = cu_append_write(&f, cu_str_cstr("a"));
    (void)ok;
}

static void sync_null(void)
{
    int ok;

    ok = cu_append_sync(NULL);
    (void)ok;
}

static void rename_null(void)
{
    int ok;

    ok = cu_path_rename("a", NULL);
    (void)ok;
}

static void dir_sync_null(void)
{
    int ok;

    ok = cu_dir_sync(NULL);
    (void)ok;
}

static void death_tests(void)
{
    char p[600];

    expect_abort(open_null_path);
    expect_abort(write_after_close);
    expect_abort(sync_null);
    expect_abort(rename_null);
    expect_abort(dir_sync_null);
    path_of(p, sizeof p, "closed");
    assert(cu_path_remove(p));
}
#endif

int main(void)
{
    const char *dir;

    dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0')
        dir = "/tmp";
    assert(strlen(dir) < sizeof root - 32);
    sprintf(root, "%s/cu-append-test", dir);
    assert(cu_dir_make(root));
    test_write_and_reopen();
    test_large_write();
    test_truncate();
    test_open_fails();
    if (sizeof(off_t) <= sizeof(size_t))
        test_truncate_too_far();
#if defined(__linux__)
    test_write_fails();
#endif
    test_lock();
    test_paths();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    assert(remove(root) == 0);
    return 0;
}
#endif
