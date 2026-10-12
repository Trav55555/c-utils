#include "death.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

/* Test files go in TMPDIR (dcheck gives wasm tests one too), else /tmp. */
static void temp_path(char *path, size_t cap, const char *name)
{
    const char *dir;

    dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0')
        dir = "/tmp";
    assert(strlen(dir) + strlen(name) + 16 < cap);
    sprintf(path, "%s/cu-file-test-%s", dir, name);
}

static void write_file(const char *path, const char *data, size_t n)
{
    FILE *f;

    f = fopen(path, "wb");
    assert(f != NULL);
    assert(n == 0 || fwrite(data, 1, n, f) == n);
    assert(fclose(f) == 0);
}

/* Bytes 0..255 repeated, so NULs and every other value appear. */
static char *pattern(size_t n)
{
    char *p;
    size_t i;

    p = (char *)malloc(n == 0 ? 1 : n);
    assert(p != NULL);
    for (i = 0; i < n; i++)
        p[i] = (char)(unsigned char)(i * 7 + 3);
    return p;
}

static void test_read_file(int debug)
{
    static const size_t sizes[] = {0, 1, 4095, 4096, 4097, 200000};
    char path[512];
    CuArena a;
    CuStr s;
    char *data;
    size_t i;
    size_t attempts;

    temp_path(path, sizeof path, "read");
    cu_arena_init_with(&a, 1024, debug);
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        data = pattern(sizes[i]);
        write_file(path, data, sizes[i]);
        s = CU_LIT("unset");
        attempts = a.attempts;
        assert(cu_file_read(&a, path, &s));
        assert(s.len == sizes[i] && s.ptr != NULL);
        assert(sizes[i] == 0 || memcmp(s.ptr, data, sizes[i]) == 0);
        if (debug)
            assert(a.attempts - attempts == (sizes[i] == 0 ? 0 : 1));
        free(data);
    }
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);
    assert(remove(path) == 0);
}

static void test_missing_file(void)
{
    char path[512];
    CuArena a;
    CuStr s;

    temp_path(path, sizeof path, "missing");
    (void)remove(path);
    cu_arena_init(&a);
    s = CU_LIT("unset");
    errno = 0;
    assert(!cu_file_read(&a, path, &s));
    assert(errno == ENOENT);
    assert(s.len == 0 && s.ptr != NULL);
    /* Not a memory failure: the arena is fine. */
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);
}

/* A directory opens on some systems and fails to read; either way it is
 * not a file's contents, and not a memory failure, though on ext4 seeking
 * to a directory's end reports a huge size. "." is the directory the
 * tests run in (the project, on its own file system); TMPDIR is often
 * tmpfs. */
static void check_directory(const char *dir)
{
    CuArena a;
    CuStr s;

    cu_arena_init(&a);
    s = CU_LIT("unset");
    assert(!cu_file_read(&a, dir, &s));
    assert(s.len == 0);
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);
}

static void test_directory(void)
{
    const char *dir;

    dir = getenv("TMPDIR");
    check_directory(dir == NULL || dir[0] == '\0' ? "/tmp" : dir);
    check_directory(".");
}

/* A stream of unknown length grows its buffer as it reads. (The stream is
 * a file opened here: WASI has no tmpfile.) */
static void test_read_stream(int debug)
{
    static const size_t sizes[] = {0, 5, 4096, 4097, 70000};
    static const size_t allocations[] = {1, 1, 2, 2, 6};
    char path[512];
    CuArena a;
    CuStr s;
    FILE *f;
    char *data;
    size_t i;
    size_t attempts;

    temp_path(path, sizeof path, "stream");
    cu_arena_init_with(&a, 1024, debug);
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        data = pattern(sizes[i]);
        write_file(path, data, sizes[i]);
        f = fopen(path, "rb");
        assert(f != NULL);
        s = CU_LIT("unset");
        attempts = a.attempts;
        assert(cu_stream_read(&a, f, &s));
        assert(s.len == sizes[i]);
        assert(sizes[i] == 0 || memcmp(s.ptr, data, sizes[i]) == 0);
        assert(feof(f) && !ferror(f));
        assert(a.attempts - attempts == allocations[i]);
        assert(fclose(f) == 0);
        free(data);
    }
    cu_arena_release(&a);
    assert(remove(path) == 0);
}

/* Each allocation the read makes fails in turn: the call fails, *out is
 * empty, the arena is marked, and the file is still closed (no leak). */
static void test_read_fails_at_every_allocation(int debug, int stream)
{
    char path[512];
    CuArena a;
    CuStr s;
    char *data;
    size_t at;
    int ok;
    int injected;
    FILE *f;

    temp_path(path, sizeof path, "fault");
    data = pattern(9000);
    write_file(path, data, 9000);
    for (at = 0;; at++) {
        cu_arena_init_with(&a, 1024, debug);
        cu_arena_fail_at(&a, at);
        s = CU_LIT("unset");
        if (stream) {
            f = fopen(path, "rb");
            assert(f != NULL);
            ok = cu_stream_read(&a, f, &s);
            assert(fclose(f) == 0);
        } else {
            ok = cu_file_read(&a, path, &s);
        }
        injected = a.attempts > at;
        assert(ok == !injected);
        if (ok)
            assert(s.len == 9000 && memcmp(s.ptr, data, 9000) == 0);
        else
            assert(s.len == 0 && cu_arena_failed(&a));
        cu_arena_release(&a);
        if (!injected)
            break;
    }
    free(data);
    assert(remove(path) == 0);
}

/* A caller-owned stream starts at its current position, remains open on
 * failure, and distinguishes an I/O error from an allocation failure. */
static void test_stream_position_and_error(void)
{
    char path[512];
    CuArena a;
    CuStr s;
    FILE *f;

    temp_path(path, sizeof path, "position");
    write_file(path, "before after", 12);
    cu_arena_init(&a);
    f = fopen(path, "rb");
    assert(f != NULL);
    assert(fseek(f, 7, SEEK_SET) == 0);
    assert(cu_stream_read(&a, f, &s));
    assert(cu_str_eq(s, CU_LIT("after")));
    assert(fclose(f) == 0);
    f = fopen(path, "wb");
    assert(f != NULL);
    s = CU_LIT("unset");
    assert(!cu_stream_read(&a, f, &s));
    assert(s.len == 0 && s.ptr != NULL);
    assert(ferror(f) && !cu_arena_failed(&a));
    assert(fclose(f) == 0);
    cu_arena_release(&a);
    assert(remove(path) == 0);
}

/* A one-byte sized file needs a small allocation, even with a tight
 * arena limit. Falling back to a large stream buffer would fail here. */
static void test_small_file_limit(void)
{
    char path[512];
    CuArena a;
    CuStr s;

    temp_path(path, sizeof path, "small");
    write_file(path, "x", 1);
    cu_arena_init_with(&a, 1, 1);
    cu_arena_set_limit(&a, 256);
    assert(cu_file_read(&a, path, &s));
    assert(cu_str_eq(s, CU_LIT("x")));
    cu_arena_release(&a);
    assert(remove(path) == 0);
}

#ifdef CU_DEATH_TESTS
static void read_null_path(void)
{
    CuArena a;
    CuStr s;

    cu_arena_init(&a);
    assert(!cu_file_read(&a, NULL, &s));
}

static void read_null_out(void)
{
    CuArena a;

    cu_arena_init(&a);
    assert(!cu_file_read(&a, "/tmp", NULL));
}

static void stream_null_file(void)
{
    CuArena a;
    CuStr s;

    cu_arena_init(&a);
    assert(!cu_stream_read(&a, NULL, &s));
}

static void death_tests(void)
{
    expect_abort(read_null_path);
    expect_abort(read_null_out);
    expect_abort(stream_null_file);
}
#endif

int main(void)
{
    int debug;

    for (debug = 0; debug <= 1; debug++) {
        test_read_file(debug);
        test_read_stream(debug);
        test_read_fails_at_every_allocation(debug, 0);
        test_read_fails_at_every_allocation(debug, 1);
    }
    test_stream_position_and_error();
    test_small_file_limit();
    test_missing_file();
    test_directory();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
