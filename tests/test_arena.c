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
    }
    test_temp_end_reuses_memory();
    test_default_init();
    test_scratch_avoids_conflicts();
    return 0;
}
