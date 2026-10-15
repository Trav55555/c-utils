#include "death.h"
#include <assert.h>
#include "cu.h"

typedef struct Point {
    long x;
    long y;
} Point;

static void test_push_and_at(int debug)
{
    CuArena a;
    CuVec v;
    Point p;
    Point *q;
    long i;

    cu_arena_init_with(&a, 64, debug);
    CU_VEC_INIT(&v, &a, Point);
    assert(cu_vec_len(&v) == 0);
    for (i = 0; i < 1000; i++) {
        p.x = i;
        p.y = -i;
        CU_VEC_PUSH(&v, &p);
    }
    assert(!cu_vec_failed(&v));
    assert(cu_vec_len(&v) == 1000);
    q = CU_VEC_AT(&v, Point, 0);
    assert(q->x == 0 && q->y == 0);
    q = CU_VEC_AT(&v, Point, 999);
    assert(q->x == 999 && q->y == -999);
    q->x = 7;
    assert(CU_VEC_AT(&v, Point, 999)->x == 7);
    cu_arena_release(&a);
}

static void test_push_copies(void)
{
    CuArena a;
    CuVec v;
    long n;

    cu_arena_init(&a);
    CU_VEC_INIT(&v, &a, long);
    n = 5;
    CU_VEC_PUSH(&v, &n);
    n = 6;
    assert(*CU_VEC_AT(&v, long, 0) == 5);
    cu_arena_release(&a);
}

static void test_pop(void)
{
    CuArena a;
    CuVec v;
    long n;

    cu_arena_init(&a);
    CU_VEC_INIT(&v, &a, long);
    n = 9;
    assert(CU_VEC_POP(&v, &n) == 0 && n == 9);
    n = 1;
    CU_VEC_PUSH(&v, &n);
    n = 2;
    CU_VEC_PUSH(&v, &n);
    assert(CU_VEC_POP(&v, &n) == 1 && n == 2);
    assert(CU_VEC_POP(&v, &n) == 1 && n == 1);
    assert(cu_vec_len(&v) == 0);
    assert(CU_VEC_POP(&v, &n) == 0 && n == 1);
    cu_arena_release(&a);
}

static void test_clear_keeps_storage(void)
{
    CuArena a;
    CuVec v;
    long n;
    long *before;

    cu_arena_init(&a);
    CU_VEC_INIT(&v, &a, long);
    n = 1;
    CU_VEC_PUSH(&v, &n);
    before = CU_VEC_DATA(&v, long);
    cu_vec_clear(&v);
    assert(cu_vec_len(&v) == 0);
    n = 2;
    CU_VEC_PUSH(&v, &n);
    assert(CU_VEC_DATA(&v, long) == before && before[0] == 2);
    cu_arena_release(&a);
}

static void test_reserve_keeps_data_in_place(int debug)
{
    CuArena a;
    CuVec v;
    long n;
    long *data;

    cu_arena_init_with(&a, 64, debug);
    CU_VEC_INIT(&v, &a, long);
    assert(cu_vec_reserve(&v, 100));
    data = CU_VEC_DATA(&v, long);
    for (n = 0; n < 100; n++)
        CU_VEC_PUSH(&v, &n);
    assert(CU_VEC_DATA(&v, long) == data);
    assert(data[0] == 0 && data[99] == 99);
    cu_arena_release(&a);
}

static void test_failure_is_sticky(void)
{
    CuArena a;
    CuVec v;
    long n;
    size_t held;

    cu_arena_init(&a);
    CU_VEC_INIT(&v, &a, long);
    n = 1;
    CU_VEC_PUSH(&v, &n);
    assert(!cu_vec_reserve(&v, (size_t)-1));
    assert(cu_vec_failed(&v));
    /* A failed vector takes no more memory, even for a reserve that fits. */
    held = a.held;
    assert(!cu_vec_reserve(&v, 1000) && a.held == held);
    CU_VEC_PUSH(&v, &n);
    assert(cu_vec_len(&v) == 1);
    assert(*CU_VEC_AT(&v, long, 0) == 1);
    cu_arena_release(&a);

    /* A count that fits but whose size in bytes does not fails the vector
     * and, as an oversized push would, the arena. */
    CU_VEC_INIT(&v, &a, long);
    assert(!cu_vec_reserve(&v, (size_t)-1 / sizeof(long)));
    assert(cu_vec_failed(&v) && cu_arena_failed(&a));
    cu_arena_release(&a);
}

static void test_empty_data_is_not_null(void)
{
    CuArena a;
    CuVec v;
    long *data;
    long total;
    size_t i;

    cu_arena_init(&a);
    CU_VEC_INIT(&v, &a, long);
    data = CU_VEC_DATA(&v, long);
    assert(data != NULL);
    total = 0;
    for (i = 0; i < cu_vec_len(&v); i++)
        total += data[i];
    assert(total == 0);
    cu_arena_release(&a);
}

/* Run n fails allocation n alone, so each allocation fails in turn while
 * the ones after it succeed; the runs end when one makes no more than n
 * allocations, and only that run may succeed. */
static void test_push_fails_at_every_allocation(int debug)
{
    CuArena a;
    CuVec v;
    size_t at;
    int injected;
    long i;
    long pushed;
    int done;

    for (at = 0;; at++) {
        cu_arena_init_with(&a, 64, debug);
        cu_arena_fail_at(&a, at);
        CU_VEC_INIT(&v, &a, long);
        pushed = 0;
        for (i = 0; i < 12; i++) {
            CU_VEC_PUSH(&v, &i);
            if (!cu_vec_failed(&v))
                pushed++;
        }
        /* Failure is sticky: nothing lands after the first failed push. */
        assert(cu_vec_len(&v) == (size_t)pushed);
        for (i = 0; i < pushed; i++)
            assert(*CU_VEC_AT(&v, long, i) == i);
        done = pushed == 12;
        /* The run succeeds exactly when the failure was never reached. */
        injected = a.attempts > at;
        assert(done == !injected);
        cu_arena_release(&a);
        if (!injected)
            break;
    }
}

/* Growth reuses the vector's own storage when it is the arena's newest
 * allocation, so a vector that nothing else pushes after holds about its
 * capacity, not every buffer it has outgrown. */
static void test_growth_holds_little(int debug)
{
    CuArena a;
    CuVec v;
    long i;
    long n;
    size_t payload;

    n = 512;
    payload = (size_t)n * sizeof(long);
    cu_arena_init_with(&a, 64, debug);
    CU_VEC_INIT(&v, &a, long);
    for (i = 0; i < n; i++)
        CU_VEC_PUSH(&v, &i);
    assert(!cu_vec_failed(&v) && cu_vec_len(&v) == (size_t)n);
    for (i = 0; i < n; i++)
        assert(*CU_VEC_AT(&v, long, i) == i);
    assert(a.held < payload + payload / 2);
    cu_arena_release(&a);
}

typedef struct Big {
    long v[20];
} Big;

/* Pushing one of the vector's own elements when it is full: growth may
 * free the old buffer, so the element must be read from where it went.
 * Small and large elements take different paths. */
static void test_push_own_element(int debug)
{
    CuArena a;
    CuVec v;
    CuVec big;
    Big b;
    long i;
    size_t k;

    cu_arena_init_with(&a, 64, debug);
    CU_VEC_INIT(&v, &a, long);
    for (i = 0; i < 8; i++)
        CU_VEC_PUSH(&v, &i);
    for (i = 0; i < 40; i++)
        CU_VEC_PUSH(&v, CU_VEC_AT(&v, long, (size_t)i % 8));
    assert(!cu_vec_failed(&v) && cu_vec_len(&v) == 48);
    for (k = 0; k < 48; k++)
        assert(*CU_VEC_AT(&v, long, k) == (long)(k % 8));

    CU_VEC_INIT(&big, &a, Big);
    for (i = 0; i < 20; i++)
        b.v[i] = i;
    CU_VEC_PUSH(&big, &b);
    for (i = 0; i < 9; i++)
        CU_VEC_PUSH(&big, CU_VEC_AT(&big, Big, 0));
    assert(!cu_vec_failed(&big) && cu_vec_len(&big) == 10);
    for (k = 0; k < 10; k++)
        assert(CU_VEC_AT(&big, Big, k)->v[19] == 19 && CU_VEC_AT(&big, Big, k)->v[3] == 3);
    cu_arena_release(&a);
}

/* A scope opened and ended between pushes changes nothing; a vector made
 * inside a scope gives all its memory back when the scope ends. */
static void test_growth_across_scopes(int debug)
{
    CuArena a;
    CuVec v;
    CuVec inner;
    CuTemp t;
    long i;
    size_t held;

    cu_arena_init_with(&a, 64, debug);
    CU_VEC_INIT(&v, &a, long);
    for (i = 0; i < 100; i++) {
        CU_VEC_PUSH(&v, &i);
        if (i % 20 == 0) {
            t = cu_temp_begin(&a);
            assert(cu_arena_push_bytes(&a, 100) != NULL);
            cu_temp_end(t);
        }
    }
    assert(!cu_vec_failed(&v) && cu_vec_len(&v) == 100);
    for (i = 0; i < 100; i++)
        assert(*CU_VEC_AT(&v, long, i) == i);
    held = a.held;
    t = cu_temp_begin(&a);
    CU_VEC_INIT(&inner, &a, long);
    for (i = 0; i < 100; i++)
        CU_VEC_PUSH(&inner, &i);
    assert(!cu_vec_failed(&inner) && *CU_VEC_AT(&inner, long, 99) == 99);
    cu_temp_end(t);
    assert(a.held == held);
    assert(*CU_VEC_AT(&v, long, 99) == 99);
    cu_arena_release(&a);
}

/* The header fast paths store and read what the functions do, and fall
 * back to them when the vector is full or failed. */
static void test_fast_macros(int debug)
{
    CuArena a;
    CuVec v;
    long i;
    size_t held;

    cu_arena_init_with(&a, 64, debug);
    CU_VEC_INIT(&v, &a, long);
    for (i = 0; i < 100; i++)
        CU_VEC_PUSH_FAST(&v, &i);
    assert(!cu_vec_failed(&v) && cu_vec_len(&v) == 100);
    for (i = 0; i < 100; i++) {
        assert(*CU_VEC_AT_FAST(&v, long, i) == i);
        assert(CU_VEC_AT_FAST(&v, long, i) == CU_VEC_AT(&v, long, i));
    }
    *CU_VEC_AT_FAST(&v, long, 3) = -3;
    assert(*CU_VEC_AT(&v, long, 3) == -3);
    /* A failed vector skips the push even with room to spare. */
    assert(cu_vec_reserve(&v, 10));
    assert(!cu_vec_reserve(&v, (size_t)-1) && cu_vec_failed(&v));
    held = a.held;
    CU_VEC_PUSH_FAST(&v, &i);
    assert(cu_vec_len(&v) == 100 && a.held == held);
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
/* With room to spare, so only the scope check can stop it. */
static void push_fast_in_inner_scope(void)
{
    CuArena a;
    CuVec v;
    CuTemp t;
    long x;

    cu_arena_init_with(&a, 64, 0);
    CU_VEC_INIT(&v, &a, long);
    assert(cu_vec_reserve(&v, 8));
    t = cu_temp_begin(&a);
    (void)t;
    x = 1;
    CU_VEC_PUSH_FAST(&v, &x);
}

static void push_fast_wrong_size(void)
{
    CuArena a;
    CuVec v;
    char c;

    cu_arena_init_with(&a, 64, 0);
    CU_VEC_INIT(&v, &a, long);
    assert(cu_vec_reserve(&v, 8));
    c = 1;
    CU_VEC_PUSH_FAST(&v, &c);
}

static void push_fast_null(void)
{
    CuArena a;
    CuVec v;
    long *none;

    cu_arena_init_with(&a, 64, 0);
    CU_VEC_INIT(&v, &a, long);
    assert(cu_vec_reserve(&v, 8));
    none = NULL;
    CU_VEC_PUSH_FAST(&v, none);
}

static void push_fast_null_vec(void)
{
    CuVec *none;
    long x;

    none = NULL;
    x = 1;
    CU_VEC_PUSH_FAST(none, &x);
}

static void at_fast_past_end(void)
{
    CuArena a;
    CuVec v;
    long x;

    cu_arena_init_with(&a, 64, 0);
    CU_VEC_INIT(&v, &a, long);
    x = 1;
    CU_VEC_PUSH(&v, &x);
    assert(*CU_VEC_AT_FAST(&v, long, 1) == 0);
}

static void at_fast_wrong_size(void)
{
    CuArena a;
    CuVec v;
    long x;

    cu_arena_init_with(&a, 64, 0);
    CU_VEC_INIT(&v, &a, long);
    x = 1;
    CU_VEC_PUSH(&v, &x);
    assert(*CU_VEC_AT_FAST(&v, char, 0) == 0);
}

static void death_tests(void)
{
    expect_abort(push_fast_in_inner_scope);
    expect_abort(push_fast_wrong_size);
    expect_abort(push_fast_null);
    expect_abort(push_fast_null_vec);
    expect_abort(at_fast_past_end);
    expect_abort(at_fast_wrong_size);
}
#endif

int main(void)
{
    int debug;

    for (debug = 0; debug <= 1; debug++) {
        test_push_and_at(debug);
        test_reserve_keeps_data_in_place(debug);
        test_push_fails_at_every_allocation(debug);
        test_growth_holds_little(debug);
        test_growth_across_scopes(debug);
        test_fast_macros(debug);
        test_push_own_element(debug);
    }
    test_push_copies();
    test_pop();
    test_clear_keeps_storage();
    test_failure_is_sticky();
    test_empty_data_is_not_null();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
