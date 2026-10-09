#include "death.h"
#include <assert.h>
#include <string.h>
#include "cu.h"

/* Runs each arena test in both modes: shared blocks (0) and one malloc per push (1). */

static int is_aligned(const void *p)
{
    return (size_t)p % sizeof(double) == 0 && (size_t)p % sizeof(void *) == 0;
}

static void test_pushes_are_separate_and_aligned(int debug)
{
    CuArena a;
    char *bytes;
    double *ds;
    long *ls;
    size_t i;

    cu_arena_init_with(&a, 64, debug);
    bytes = cu_arena_push_bytes(&a, 3);
    ds = CU_PUSH(&a, double, 4);
    ls = CU_PUSH(&a, long, 100);
    assert(bytes != NULL && ds != NULL && ls != NULL);
    assert(is_aligned(ds) && is_aligned(ls));
    memset(bytes, 'x', 3);
    for (i = 0; i < 4; i++)
        ds[i] = (double)i;
    for (i = 0; i < 100; i++)
        ls[i] = (long)i;
    assert(bytes[0] == 'x' && bytes[2] == 'x');
    assert(ds[3] > 2.5 && ds[3] < 3.5);
    assert(ls[0] == 0 && ls[99] == 99);
    assert(cu_arena_failed(&a) == 0);
    cu_arena_release(&a);
}

static void test_zero_size_push_is_not_null(int debug)
{
    CuArena a;

    cu_arena_init_with(&a, 64, debug);
    assert(cu_arena_push_bytes(&a, 0) != NULL);
    assert(cu_arena_push(&a, 0, sizeof(long)) != NULL);
    assert(cu_arena_failed(&a) == 0);
    cu_arena_release(&a);
}

static void test_overflow_fails_and_is_sticky(int debug)
{
    CuArena a;

    cu_arena_init_with(&a, 64, debug);
    assert(cu_arena_push(&a, (size_t)-1, 2) == NULL);
    assert(cu_arena_failed(&a) == 1);
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    assert(cu_arena_failed(&a) == 1);
    cu_arena_release(&a);
    assert(cu_arena_failed(&a) == 0);
}

static void test_temp_scopes_nest(int debug)
{
    CuArena a;
    CuTemp outer;
    CuTemp inner;
    char *keep;
    char *p;

    cu_arena_init_with(&a, 64, debug);
    keep = cu_arena_push_bytes(&a, 4);
    assert(keep != NULL);
    memcpy(keep, "keep", 4);
    outer = cu_temp_begin(&a);
    p = cu_arena_push_bytes(&a, 200);
    assert(p != NULL);
    inner = cu_temp_begin(&a);
    p = cu_arena_push_bytes(&a, 10);
    assert(p != NULL);
    cu_temp_end(inner);
    p = cu_arena_push_bytes(&a, 10);
    assert(p != NULL);
    cu_temp_end(outer);
    assert(memcmp(keep, "keep", 4) == 0);
    cu_arena_release(&a);
}

static void test_temp_end_reuses_memory(void)
{
    CuArena a;
    CuTemp t;
    char *first;
    char *again;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 1) != NULL);
    t = cu_temp_begin(&a);
    first = cu_arena_push_bytes(&a, 8);
    cu_temp_end(t);
    again = cu_arena_push_bytes(&a, 8);
    assert(first != NULL && first == again);
    cu_arena_release(&a);
}

/* Normal mode packs byte pushes into one block; debug mode gives each push
 * its own allocation, so sanitizers see every object's bounds. */
static void test_debug_mode_separates_pushes(int debug)
{
    CuArena a;
    char *p1;
    char *p2;

    cu_arena_init_with(&a, 64, debug);
    p1 = cu_arena_push_bytes(&a, 1);
    p2 = cu_arena_push_bytes(&a, 1);
    assert(p1 != NULL && p2 != NULL);
    assert((p2 == p1 + 1) == !debug);
    cu_arena_release(&a);
}

static void test_release_then_reuse(int debug)
{
    CuArena a;

    cu_arena_init_with(&a, 64, debug);
    assert(cu_arena_push_bytes(&a, 100) != NULL);
    cu_arena_release(&a);
    assert(cu_arena_push_bytes(&a, 100) != NULL);
    cu_arena_release(&a);
}

static void test_default_init(void)
{
    CuArena a;
    long *p;

    cu_arena_init(&a);
    p = CU_PUSH(&a, long, 1);
    assert(p != NULL);
    *p = 5;
    assert(*p == 5);
    cu_arena_release(&a);
}

static void test_scratch_avoids_conflicts(void)
{
    CuTemp s1;
    CuTemp s2;
    CuTemp s3;
    CuArena *conflicts[1];
    char *p;

    s1 = cu_scratch_begin1(NULL);
    assert(s1.arena != NULL);
    s2 = cu_scratch_begin1(s1.arena);
    assert(s2.arena != NULL && s2.arena != s1.arena);
    conflicts[0] = s2.arena;
    s3 = cu_scratch_begin(conflicts, 1);
    assert(s3.arena == s1.arena);
    p = cu_arena_push_bytes(s2.arena, 16);
    assert(p != NULL);
    cu_scratch_end(s3);
    cu_scratch_end(s2);
    cu_scratch_end(s1);
    cu_scratch_release();
}

/* A push that exactly fills the rest of a block stays in it. */
static void test_exact_fit_stays_in_block(void)
{
    CuArena a;
    size_t one;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 32) != NULL);
    one = a.held;
    assert(cu_arena_push_bytes(&a, 32) != NULL);
    assert(a.held == one);
    /* Nothing more fits, but a zero-size push still needs no new block. */
    assert(cu_arena_push_bytes(&a, 0) != NULL);
    assert(a.held == one);
    assert(cu_arena_push_bytes(&a, 1) != NULL);
    assert(a.held > one);
    cu_arena_release(&a);
}

/* A size that overflows once the block header is added fails cleanly. */
static void test_header_overflow_fails(int debug)
{
    CuArena a;

    cu_arena_init_with(&a, 64, debug);
    assert(cu_arena_push_bytes(&a, (size_t)-1) == NULL);
    assert(cu_arena_failed(&a) && a.held == 0);
    cu_arena_release(&a);
}

/* The limit counts whole blocks: a push fails when its block would pass
 * it, ending a scope or releasing gives the bytes back, and the arena is
 * marked failed exactly as for running out of memory. */
static void test_limit(int debug)
{
    CuArena a;
    CuTemp t;
    size_t one;

    /* Measure one block's cost with no limit. */
    cu_arena_init_with(&a, 64, debug);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    one = a.held;
    assert(one > 64);
    cu_arena_release(&a);
    assert(a.held == 0);

    cu_arena_set_limit(&a, one - 1);
    assert(cu_arena_push_bytes(&a, 64) == NULL);
    assert(cu_arena_failed(&a) && a.held == 0);
    /* refused names the exact cap that lets the push through. */
    assert(a.refused == one && a.held + a.refused == one);
    cu_arena_release(&a);

    cu_arena_set_limit(&a, one);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    assert(a.held == one);
    t = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 64) == NULL);
    assert(cu_arena_failed(&a));
    cu_temp_end(t);
    cu_arena_release(&a);
    assert(!cu_arena_failed(&a));

    /* A scope that ends returns its blocks' bytes. */
    cu_arena_set_limit(&a, 2 * one);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    t = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    assert(a.held == 2 * one);
    cu_temp_end(t);
    assert(a.held == one);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);

    /* A limit set below what is already held refuses new blocks. */
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    cu_arena_set_limit(&a, 1);
    assert(cu_arena_push_bytes(&a, 64) == NULL);
    assert(a.refused != 0);
    /* Releasing clears the refusal along with the failure. */
    cu_arena_release(&a);
    assert(a.refused == 0 && !cu_arena_failed(&a));

    /* No limit by default, and (size_t)-1 lifts one. */
    cu_arena_init_with(&a, 64, debug);
    assert(a.limit == (size_t)-1 && a.refused == 0);
    cu_arena_set_limit(&a, 0);
    assert(cu_arena_push_bytes(&a, 1) == NULL);
    cu_arena_set_limit(&a, (size_t)-1);
    assert(cu_arena_push_bytes(&a, 1) != NULL);
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
/* Ending the outer scope frees the inner scope's block, and the next push
 * usually gets a block at the same address. The stale inner scope must still
 * be caught, not matched to the new block by address. */
static void end_stale_scope_after_reuse(void)
{
    CuArena a;
    CuTemp outer;
    CuTemp inner;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    outer = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 200) != NULL);
    inner = cu_temp_begin(&a);
    cu_temp_end(outer);
    assert(cu_arena_push_bytes(&a, 200) != NULL);
    cu_temp_end(inner);
}

static void end_scope_after_release(void)
{
    CuArena a;
    CuTemp t;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    t = cu_temp_begin(&a);
    cu_arena_release(&a);
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    cu_temp_end(t);
}

/* Contract violations abort. */
static void death_tests(void)
{
    expect_abort(end_stale_scope_after_reuse);
    expect_abort(end_scope_after_release);
}
#endif

int main(void)
{
    int debug;

    for (debug = 0; debug <= 1; debug++) {
        test_pushes_are_separate_and_aligned(debug);
        test_zero_size_push_is_not_null(debug);
        test_overflow_fails_and_is_sticky(debug);
        test_temp_scopes_nest(debug);
        test_release_then_reuse(debug);
        test_debug_mode_separates_pushes(debug);
        test_limit(debug);
        test_header_overflow_fails(debug);
    }
    test_temp_end_reuses_memory();
    test_exact_fit_stays_in_block();
    test_default_init();
    test_scratch_avoids_conflicts();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
