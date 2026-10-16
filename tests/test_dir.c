#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "death.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

#ifdef __cerb__
/* Cerberus models no directory kinds or links, so cu_dir reports the
 * platform as having no directories; the listing tests run natively and
 * under wasm. */
int main(void)
{
    CuArena a;
    CuDir d;
    CuDirEntry e;

    cu_arena_init(&a);
    assert(!cu_dir_open(&d, &a, "."));
    assert(cu_dir_errno(&d) == ENOSYS);
    assert(cu_dir_next(&d, &e) == -1);
    cu_dir_close(&d);
    cu_arena_release(&a);
    return 0;
}
#else

#include <sys/stat.h>
#include <unistd.h>

static char root[512];

/* A name with bytes >= 0x80. Natively it is not UTF-8, since names are any
 * bytes; WASI paths must be UTF-8. */
#ifdef __wasi__
#define HIGH_NAME "\xc3\xa9t\xc3\xa9"
#else
#define HIGH_NAME "\xc3\xa9t\xe9"
#endif

/* Test directories go in TMPDIR (dcheck gives wasm tests one too), else /tmp. */
static void path_of(char *out, size_t cap, const char *rel)
{
    assert(strlen(root) + 1 + strlen(rel) < cap);
    sprintf(out, "%s/%s", root, rel);
}

static void make_dir(const char *rel)
{
    char p[600];

    path_of(p, sizeof p, rel);
    assert(mkdir(p, 0700) == 0);
}

static void make_file(const char *rel)
{
    char p[600];
    FILE *f;

    path_of(p, sizeof p, rel);
    f = fopen(p, "wb");
    assert(f != NULL);
    assert(fclose(f) == 0);
}

static void remove_path(const char *rel, int dir)
{
    char p[600];

    path_of(p, sizeof p, rel);
    assert((dir ? rmdir(p) : unlink(p)) == 0);
}

/* Lists rel into names and kinds; returns the count. Asserts the listing
 * ends cleanly and that names stay valid after close. */
static size_t list(CuArena *a, const char *rel, CuDirEntry *out, size_t cap)
{
    char p[600];
    CuDir d;
    CuDirEntry e;
    size_t n;
    int r;

    path_of(p, sizeof p, rel);
    assert(cu_dir_open(&d, a, p));
    n = 0;
    while ((r = cu_dir_next(&d, &e)) == 1) {
        assert(n < cap);
        assert(e.name.ptr[e.name.len] == '\0');
        out[n++] = e;
    }
    assert(r == 0);
    assert(cu_dir_next(&d, &e) == 0);
    cu_dir_close(&d);
    cu_dir_close(&d);
    return n;
}

/* The kind of the one entry named name, failing if it is absent or
 * listed twice. */
static CuDirKind kind_of(const CuDirEntry *es, size_t n, const char *name, size_t len)
{
    size_t i;
    size_t found;
    CuDirKind kind;

    found = 0;
    kind = CU_DIR_OTHER;
    for (i = 0; i < n; i++) {
        if (es[i].name.len == len && memcmp(es[i].name.ptr, name, len) == 0) {
            found++;
            kind = es[i].kind;
        }
    }
    assert(found == 1);
    return kind;
}

static void test_empty(int debug)
{
    CuArena a;
    CuDirEntry es[4];

    cu_arena_init_with(&a, 1024, debug);
    make_dir("empty");
    assert(list(&a, "empty", es, 4) == 0);
    remove_path("empty", 1);
    cu_arena_release(&a);
}

/* Every kind, with names that hold bytes >= 0x80 and a newline. A link to
 * a directory is a link, and listing does not descend into either. */
static void test_kinds(int debug)
{
    static const char high[] = HIGH_NAME;
    static const char newline[] = "new\nline";
    char p[600];
    CuArena a;
    CuDirEntry es[16];
    size_t n;
    int fifo;

    cu_arena_init_with(&a, 1024, debug);
    make_dir("kinds");
    make_file("kinds/file");
    make_dir("kinds/sub");
    make_file("kinds/sub/inner");
    /* A relative target: WASI refuses absolute ones. */
    path_of(p, sizeof p, "kinds/link");
    assert(symlink("sub", p) == 0);
    make_file("kinds/" HIGH_NAME);
    make_file("kinds/new\nline");
#ifdef __wasi__
    fifo = 0; /* WASI has no FIFOs */
#else
    path_of(p, sizeof p, "kinds/fifo");
    assert(mkfifo(p, 0600) == 0);
    fifo = 1;
#endif
    n = list(&a, "kinds", es, 16);
    assert(n == (size_t)(5 + fifo));
    assert(kind_of(es, n, "file", 4) == CU_DIR_FILE);
    assert(kind_of(es, n, "sub", 3) == CU_DIR_DIR);
    assert(kind_of(es, n, "link", 4) == CU_DIR_LINK);
    assert(kind_of(es, n, high, sizeof high - 1) == CU_DIR_FILE);
    assert(kind_of(es, n, newline, sizeof newline - 1) == CU_DIR_FILE);
    if (fifo)
        assert(kind_of(es, n, "fifo", 4) == CU_DIR_OTHER);
    /* A nested directory lists on its own. */
    n = list(&a, "kinds/sub", es, 16);
    assert(n == 1 && kind_of(es, n, "inner", 5) == CU_DIR_FILE);
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);
    remove_path("kinds/sub/inner", 0);
    remove_path("kinds/link", 0);
    remove_path("kinds/sub", 1);
    remove_path("kinds/file", 0);
    remove_path("kinds/" HIGH_NAME, 0);
    remove_path("kinds/new\nline", 0);
    if (fifo)
        remove_path("kinds/fifo", 0);
    remove_path("kinds", 1);
}

/* A failed open saves errno, and the CuDir then reports errors. */
static void test_open_fails(void)
{
    char p[600];
    CuArena a;
    CuDir d;
    CuDirEntry e;

    cu_arena_init(&a);
    path_of(p, sizeof p, "missing");
    assert(!cu_dir_open(&d, &a, p));
    assert(cu_dir_errno(&d) == ENOENT);
    e.name = CU_LIT("unset");
    assert(cu_dir_next(&d, &e) == -1 && cu_dir_next(&d, &e) == -1);
    assert(cu_str_eq(e.name, CU_LIT("unset")));
    cu_dir_close(&d);
    make_file("plain");
    path_of(p, sizeof p, "plain");
    assert(!cu_dir_open(&d, &a, p));
    assert(cu_dir_errno(&d) == ENOTDIR);
    cu_dir_close(&d);
    remove_path("plain", 0);
#ifndef __wasi__
    /* Permissions: WASI has none, and root reads anything. */
    if (geteuid() != 0) {
        make_dir("locked");
        path_of(p, sizeof p, "locked");
        assert(chmod(p, 0) == 0);
        assert(!cu_dir_open(&d, &a, p));
        assert(cu_dir_errno(&d) == EACCES);
        cu_dir_close(&d);
        assert(chmod(p, 0700) == 0);
        remove_path("locked", 1);
    }
#endif
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);
}

/* A directory removed after open lists nothing more, without crashing. */
static void test_removed_while_open(void)
{
    char p[600];
    CuArena a;
    CuDir d;
    CuDirEntry e;

    cu_arena_init(&a);
    make_dir("gone");
    path_of(p, sizeof p, "gone");
    assert(cu_dir_open(&d, &a, p));
    remove_path("gone", 1);
    assert(cu_dir_next(&d, &e) != 1);
    cu_dir_close(&d);
    cu_arena_release(&a);
}

/* Each allocation a listing makes fails in turn: the call returns -1 with
 * errno 0 and the arena marked, and stays failed. */
static void test_fails_at_every_allocation(int debug)
{
    CuArena a;
    CuDir d;
    CuDirEntry e;
    char p[600];
    size_t n;
    size_t got;
    int r;

    make_dir("alloc");
    make_file("alloc/one");
    make_file("alloc/two");
    path_of(p, sizeof p, "alloc");
    for (n = 0;; n++) {
        cu_arena_init_with(&a, 1024, debug);
        cu_arena_fail_at(&a, n);
        assert(cu_dir_open(&d, &a, p));
        got = 0;
        while ((r = cu_dir_next(&d, &e)) == 1)
            got++;
        cu_dir_close(&d);
        if (r == 0) {
            assert(got == 2 && !cu_arena_failed(&a));
            cu_arena_release(&a);
            break;
        }
        assert(r == -1 && cu_arena_failed(&a) && cu_dir_errno(&d) == 0);
        cu_arena_release(&a);
    }
    assert(n > 0);
    remove_path("alloc/one", 0);
    remove_path("alloc/two", 0);
    remove_path("alloc", 1);
}

#ifdef CU_DEATH_TESTS
static void next_after_close(void)
{
    CuArena a;
    CuDir d;
    CuDirEntry e;

    cu_arena_init(&a);
    assert(cu_dir_open(&d, &a, root));
    cu_dir_close(&d);
    (void)cu_dir_next(&d, &e);
}

static void open_null_path(void)
{
    CuArena a;
    CuDir d;
    int ok;

    cu_arena_init(&a);
    ok = cu_dir_open(&d, &a, NULL);
    (void)ok;
}

static void next_null_entry(void)
{
    CuArena a;
    CuDir d;

    cu_arena_init(&a);
    assert(cu_dir_open(&d, &a, root));
    (void)cu_dir_next(&d, NULL);
}
#endif

int main(void)
{
    const char *dir;
    int debug;

    dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0')
        dir = "/tmp";
    assert(strlen(dir) < sizeof root - 32);
    sprintf(root, "%s/cu-dir-test", dir);
    assert(mkdir(root, 0700) == 0);
    for (debug = 0; debug <= 1; debug++) {
        test_empty(debug);
        test_kinds(debug);
        test_fails_at_every_allocation(debug);
    }
    test_open_fails();
    test_removed_while_open();
#ifdef CU_DEATH_TESTS
    expect_abort(next_after_close);
    expect_abort(open_null_path);
    expect_abort(next_null_entry);
#endif
    assert(rmdir(root) == 0);
    return 0;
}
#endif
