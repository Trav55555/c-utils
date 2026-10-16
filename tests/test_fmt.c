#include "death.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
#include "cu.h"

static void test_builder_appends(CuArena *a)
{
    CuBuilder b;
    CuStr out;

    cu_sb_init(&b, a);
    cu_sb_str(&b, CU_LIT("n="));
    cu_sb_long(&b, -42);
    cu_sb_char(&b, ' ');
    cu_sb_hex(&b, 255, 4);
    cu_sb_char(&b, ' ');
    cu_sb_hex(&b, 0, 0);
    cu_sb_char(&b, ' ');
    cu_sb_ulong(&b, 0);
    assert(cu_sb_finish(&b, &out));
    assert(cu_str_eq(out, CU_LIT("n=-42 00ff 0 0")));
}

static void test_builder_empty(CuArena *a)
{
    CuBuilder b;
    CuStr out;

    cu_sb_init(&b, a);
    cu_sb_str(&b, cu_str_empty());
    assert(cu_sb_finish(&b, &out));
    assert(out.len == 0 && out.ptr != NULL);
}

static void test_builder_grows(CuArena *a)
{
    CuBuilder b;
    CuStr out;
    size_t i;

    cu_sb_init(&b, a);
    for (i = 0; i < 1000; i++)
        cu_sb_char(&b, (char)('a' + (char)(i % 26)));
    assert(cu_sb_finish(&b, &out));
    assert(out.len == 1000 && out.ptr[0] == 'a' && out.ptr[999] == 'l');
}

static void test_builder_failure_is_sticky(CuArena *a)
{
    CuBuilder b;
    CuStr out;
    char one;

    one = 'x';
    cu_sb_init(&b, a);
    cu_sb_char(&b, 'a');
    /* A length that cannot fit after one byte: the append fails without reading. */
    cu_sb_str(&b, cu_str_n(&one, (size_t)-1));
    cu_sb_char(&b, 'b');
    assert(cu_sb_finish(&b, &out) == 0);
    assert(out.len == 0);
}

/* Padding is reserved in one piece: a width that cannot fit fails at once
 * instead of appending zeros one at a time until memory runs out. */
static void test_hex_padding(int debug)
{
    CuArena a;
    CuBuilder b;
    CuStr out;

    cu_arena_init_with(&a, 64, debug);
    cu_sb_init(&b, &a);
    cu_sb_hex(&b, 0xabcUL, 6);
    cu_sb_hex(&b, 0xabcUL, 2);
    assert(cu_sb_finish(&b, &out) && cu_str_eq(out, CU_LIT("000abcabc")));

    cu_arena_set_limit(&a, a.held + 4096);
    cu_sb_init(&b, &a);
    cu_sb_hex(&b, 0, (size_t)-1);
    assert(!cu_sb_finish(&b, &out));
    cu_sb_init(&b, &a);
    cu_sb_hex(&b, 0, (size_t)1 << 20);
    assert(!cu_sb_finish(&b, &out) && cu_arena_failed(&a));
    cu_arena_release(&a);
}

/* Run n fails allocation n alone, so each allocation fails in turn while
 * the ones after it succeed; the runs end when one makes no more than n
 * allocations, and only that run may succeed. */
static void test_append_fails_at_every_allocation(int debug)
{
    CuArena a;
    CuBuilder b;
    CuStr out;
    size_t at;
    int injected;
    int i;
    int done;

    for (at = 0;; at++) {
        cu_arena_init_with(&a, 64, debug);
        cu_arena_fail_at(&a, at);
        cu_sb_init(&b, &a);
        for (i = 0; i < 40; i++)
            cu_sb_str(&b, CU_LIT("0123456789"));
        done = cu_sb_finish(&b, &out);
        if (done)
            assert(out.len == 400 && cu_str_starts_with(out, CU_LIT("01234567890123")));
        else
            assert(out.len == 0 && cu_arena_failed(&a));
        /* The run succeeds exactly when the failure was never reached. */
        injected = a.attempts > at;
        assert(done == !injected);
        cu_arena_release(&a);
        if (!injected)
            break;
    }
}

/* A view from cu_sb_finish stays valid while the builder grows, and a
 * builder can append its own contents: growth never frees the old buffer.
 * Past its block, the builder takes a new block rather than reallocating. */
static void test_finished_views_survive_growth(int debug)
{
    CuArena a;
    CuBuilder b;
    CuStr first;
    CuStr s;
    size_t blocks;
    size_t i;

    cu_arena_init_with(&a, 64, debug);
    cu_sb_init(&b, &a);
    cu_sb_str(&b, CU_LIT("hello"));
    assert(cu_sb_finish(&b, &first));
    blocks = a.blocks;
    for (i = 0; i < 200; i++)
        cu_sb_char(&b, 'x');
    assert(a.blocks > blocks);
    assert(cu_str_eq(first, CU_LIT("hello")));

    /* Self-append: the source is the builder's own buffer. */
    cu_sb_init(&b, &a);
    for (i = 0; i < 40; i++)
        cu_sb_char(&b, 'y');
    assert(cu_sb_finish(&b, &s));
    cu_sb_str(&b, s);
    cu_sb_str(&b, s);
    assert(cu_sb_finish(&b, &s) && s.len == 120);
    for (i = 0; i < 120; i++)
        assert(s.ptr[i] == 'y');
    cu_arena_release(&a);
}

/* A builder that nothing else pushes after grows in place while its
 * block has room: the same buffer, no new block, nothing left behind. */
static void test_growth_in_place(void)
{
    CuArena a;
    CuBuilder b;
    CuStr out;
    char *first;
    size_t blocks;
    size_t held;
    size_t i;
    size_t n;

    n = 1000;
    cu_arena_init_with(&a, 4096, 0);
    cu_sb_init(&b, &a);
    cu_sb_char(&b, 'a');
    first = b.data;
    blocks = a.blocks;
    held = a.held;
    for (i = 1; i < n; i++)
        cu_sb_char(&b, (char)('a' + (char)(i % 26)));
    assert(cu_sb_finish(&b, &out) && out.len == n && out.ptr == first);
    assert(a.blocks == blocks && a.held == held);
    for (i = 0; i < n; i++)
        assert(out.ptr[i] == (char)('a' + (char)(i % 26)));
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
static void char_null_builder(void)
{
    cu_sb_char(NULL, 'x');
}

static void empty_str_null_builder(void)
{
    cu_sb_str(NULL, cu_str_empty());
}

static void hex_null_builder(void)
{
    cu_sb_hex(NULL, 1, 4);
}

static void finish_null_builder(void)
{
    CuStr out;

    assert(cu_sb_finish(NULL, &out));
}

static void finish_null_out(void)
{
    CuArena a;
    CuBuilder b;

    cu_arena_init(&a);
    cu_sb_init(&b, &a);
    assert(cu_sb_finish(&b, NULL));
}

static void death_tests(void)
{
    expect_abort(char_null_builder);
    expect_abort(empty_str_null_builder);
    expect_abort(hex_null_builder);
    expect_abort(finish_null_builder);
    expect_abort(finish_null_out);
}
#endif

/* Until cu_sb_finish hands out a view, nothing can point into the
 * builder's buffer, so growth past its block reallocates instead of
 * leaving every outgrown copy behind: the arena holds less than twice the
 * contents (doubling) plus a block, not about 3.4 times. */
static void test_unshared_growth_leaves_nothing_behind(void)
{
    CuArena a;
    CuBuilder b;
    CuStr out;
    size_t n;
    size_t i;
    size_t block_size;
    size_t step;

    /* Cerberus models each byte. A small block still forces repeated
     * reallocations, and the same bound rejects retained old buffers.
     * Native checks keep the one-megabyte workload. Both end just past
     * a power of two, the worst case for doubling. */
#ifdef __cerb__
    n = 257;
    block_size = 64;
    step = 1;
#else
    n = (1u << 20) + 1;
    block_size = 65536;
    step = 4099;
#endif
    cu_arena_init_with(&a, block_size, 0);
    cu_sb_init(&b, &a);
    for (i = 0; i < n; i++)
        cu_sb_char(&b, (char)('a' + i % 26));
    assert(cu_sb_finish(&b, &out) && out.len == n);
    for (i = 0; i < n; i += step)
        assert(out.ptr[i] == (char)('a' + i % 26));
    assert(a.held < 2 * n + block_size);
    cu_arena_release(&a);
}

/* After a finish, the next growth copies, so the view survives; the copy
 * has no views yet, so later growth reallocates again. */
static void test_growth_after_finish(int debug)
{
    CuArena a;
    CuBuilder b;
    CuStr first;
    CuStr out;
    size_t i;
    size_t n;

    /* From 100 bytes, 157 more cross both the shared 128-byte buffer and
     * its unshared 256-byte replacement. Cerberus checks every byte at
     * those boundaries; native checks retain the larger workload. */
#ifdef __cerb__
    n = 157;
#else
    n = 5000;
#endif
    cu_arena_init_with(&a, 64, debug);
    cu_sb_init(&b, &a);
    for (i = 0; i < 100; i++)
        cu_sb_char(&b, 'p');
    assert(cu_sb_finish(&b, &first) && first.len == 100);
    for (i = 0; i < n; i++)
        cu_sb_char(&b, (char)('0' + i % 10));
    assert(cu_sb_finish(&b, &out) && out.len == 100 + n);
    for (i = 0; i < 100; i++)
        assert(first.ptr[i] == 'p' && out.ptr[i] == 'p');
    for (i = 0; i < n; i++)
        assert(out.ptr[100 + i] == (char)('0' + i % 10));
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);
}

/* Appending the builder's own bytes, read straight from its data field
 * with no finish, survives a growth that reallocates the buffer. */
static void test_unshared_self_append(int debug)
{
    CuArena a;
    CuBuilder b;
    CuStr out;
    size_t i;
    int round;

    cu_arena_init_with(&a, 64, debug);
    cu_sb_init(&b, &a);
    cu_sb_str(&b, CU_LIT("ab"));
    for (round = 0; round < 12; round++)
        cu_sb_str(&b, cu_str_n(b.data, b.len));
    assert(cu_sb_finish(&b, &out) && out.len == (size_t)2 << 12);
    for (i = 0; i < out.len; i++)
        assert(out.ptr[i] == (i % 2 ? 'b' : 'a'));
    cu_arena_release(&a);
}

int main(void)
{
    CuArena a;
    int debug;

    for (debug = 0; debug <= 1; debug++) {
        cu_arena_init_with(&a, 64, debug);
        test_builder_appends(&a);
        test_builder_empty(&a);
        test_builder_grows(&a);
        test_builder_failure_is_sticky(&a);
        test_append_fails_at_every_allocation(debug);
        test_hex_padding(debug);
        test_finished_views_survive_growth(debug);
        test_growth_after_finish(debug);
        test_unshared_self_append(debug);
        assert(cu_arena_failed(&a) == 0);
        cu_arena_release(&a);
    }
    test_growth_in_place();
    test_unshared_growth_leaves_nothing_behind();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
