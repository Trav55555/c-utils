#include "death.h"
#include <assert.h>
#include "cu.h"

static void test_add_get_remove(CuArena *a)
{
    CuPool p;
    CuHandle h1;
    CuHandle h2;
    long v;

    CU_POOL_INIT(&p, a, long);
    v = 10;
    h1 = CU_POOL_ADD(&p, &v);
    v = 20;
    h2 = CU_POOL_ADD(&p, &v);
    assert(!cu_handle_is_null(h1) && !cu_handle_is_null(h2));
    assert(cu_pool_len(&p) == 2);
    assert(*CU_POOL_GET(&p, long, h1) == 10);
    assert(*CU_POOL_GET(&p, long, h2) == 20);
    assert(CU_POOL_GET(&p, long, h1) != CU_POOL_GET(&p, long, h2));
    assert(cu_pool_remove(&p, h1));
    assert(cu_pool_len(&p) == 1);
    assert(CU_POOL_GET(&p, long, h1) == NULL);
    /* Removing twice is reported, not a double free. */
    assert(!cu_pool_remove(&p, h1));
    assert(cu_pool_len(&p) == 1);
    assert(*CU_POOL_GET(&p, long, h2) == 20);
    assert(!cu_pool_failed(&p));
}

/* A reused slot gets a new generation: the old handle stays stale. */
static void test_reuse_bumps_generation(CuArena *a)
{
    CuPool p;
    CuHandle old;
    CuHandle fresh;
    long v;

    CU_POOL_INIT(&p, a, long);
    v = 1;
    old = CU_POOL_ADD(&p, &v);
    assert(cu_pool_remove(&p, old));
    v = 2;
    fresh = CU_POOL_ADD(&p, &v);
    assert(fresh.index == old.index && fresh.gen != old.gen);
    assert(CU_POOL_GET(&p, long, old) == NULL);
    assert(*CU_POOL_GET(&p, long, fresh) == 2);
    assert(!cu_pool_remove(&p, old));
    assert(*CU_POOL_GET(&p, long, fresh) == 2);
}

static void test_bad_handles(CuArena *a)
{
    CuPool p;
    CuHandle h;
    CuHandle forged;
    long v;

    CU_POOL_INIT(&p, a, long);
    assert(cu_handle_is_null(cu_handle_null()));
    assert(CU_POOL_GET(&p, long, cu_handle_null()) == NULL);
    assert(!cu_pool_remove(&p, cu_handle_null()));
    v = 5;
    h = CU_POOL_ADD(&p, &v);
    forged = h;
    forged.index = h.index + 1;
    assert(CU_POOL_GET(&p, long, forged) == NULL);
    forged.index = (size_t)-1;
    assert(CU_POOL_GET(&p, long, forged) == NULL);
    assert(!cu_pool_remove(&p, forged));
    forged = h;
    forged.gen = h.gen + 1;
    assert(CU_POOL_GET(&p, long, forged) == NULL);
    forged.gen = 0;
    assert(CU_POOL_GET(&p, long, forged) == NULL);
    assert(cu_pool_len(&p) == 1);
}

/* Across several pages, objects stay put while others come and go. */
static void test_many_objects_stay_put(CuArena *a)
{
    CuPool p;
    CuHandle hs[300];
    long *first;
    long v;
    long i;

    CU_POOL_INIT(&p, a, long);
    for (i = 0; i < 300; i++) {
        v = i;
        hs[i] = CU_POOL_ADD(&p, &v);
        assert(!cu_handle_is_null(hs[i]));
    }
    first = CU_POOL_GET(&p, long, hs[0]);
    for (i = 1; i < 300; i += 2)
        assert(cu_pool_remove(&p, hs[i]));
    for (i = 1; i < 300; i += 2) {
        v = -i;
        hs[i] = CU_POOL_ADD(&p, &v);
    }
    assert(cu_pool_len(&p) == 300);
    assert(CU_POOL_GET(&p, long, hs[0]) == first);
    for (i = 0; i < 300; i++)
        assert(*CU_POOL_GET(&p, long, hs[i]) == (i % 2 ? -i : i));
}

/* With max_gen 2 a slot serves two generations, then retires. */
static void test_retires_at_max_generation(CuArena *a)
{
    CuPool p;
    CuHandle g1;
    CuHandle g2;
    CuHandle next;
    long v;

    cu_pool_init_with(&p, a, sizeof(long), 2);
    v = 1;
    g1 = CU_POOL_ADD(&p, &v);
    assert(g1.gen == 1);
    assert(cu_pool_remove(&p, g1));
    g2 = CU_POOL_ADD(&p, &v);
    assert(g2.index == g1.index && g2.gen == 2);
    assert(cu_pool_remove(&p, g2));
    next = CU_POOL_ADD(&p, &v);
    assert(next.index != g1.index);
    assert(CU_POOL_GET(&p, long, g1) == NULL);
    assert(CU_POOL_GET(&p, long, g2) == NULL);
    assert(!cu_pool_remove(&p, g2));
    assert(CU_POOL_GET(&p, long, next) != NULL);
}

/* A value size whose 64-slot page overflows size_t makes the first add
 * fail without touching malloc. The failure returns before copying, so a
 * small value is enough. */
static void test_failed_add_is_sticky(CuArena *a)
{
    CuPool p;
    long v;
    size_t huge;

    /* A shift, not / 2: Cerberus (git-b3e11ea) truncates unsigned
     * quotients of 2^31 or more, which would make this value small. */
    huge = (size_t)-1 >> 1;
    cu_pool_init(&p, a, huge);
    v = 1;
    assert(cu_handle_is_null(cu_pool_add_(&p, &v, huge)));
    assert(cu_pool_failed(&p) && cu_pool_len(&p) == 0);
    assert(cu_handle_is_null(cu_pool_add_(&p, &v, huge)));
    assert(cu_pool_get_(&p, cu_handle_null(), huge) == NULL);
}

/* Run n fails allocation n alone, so each allocation fails in turn while
 * the ones after it succeed; the runs end when one makes no more than n
 * allocations, and only that run may succeed. */
static void test_add_fails_at_every_allocation(int debug)
{
    CuArena a;
    CuPool p;
    CuHandle h1;
    CuHandle h2;
    size_t at;
    int injected;
    long v;
    int done;

    for (at = 0;; at++) {
        cu_arena_init_with(&a, 64, debug);
        cu_arena_fail_at(&a, at);
        CU_POOL_INIT(&p, &a, long);
        v = 1;
        /* The first add makes the page's slots, its values and the page
         * table: three allocations. */
        h1 = CU_POOL_ADD(&p, &v);
        v = 2;
        h2 = CU_POOL_ADD(&p, &v);
        done = !cu_handle_is_null(h1);
        if (!done) {
            assert(cu_pool_failed(&p) && cu_pool_len(&p) == 0);
            assert(cu_handle_is_null(h2));
        } else {
            assert(*CU_POOL_GET(&p, long, h1) == 1 && *CU_POOL_GET(&p, long, h2) == 2);
        }
        /* The run succeeds exactly when the failure was never reached. */
        injected = a.attempts > at;
        assert(done == !injected);
        cu_arena_release(&a);
        if (!injected)
            break;
    }
}

#ifdef CU_DEATH_TESTS
/* Must-use results land here: asserting success inside a death test would
 * let an assert, not the contract check, supply the abort. */
static CuHandle handle_sink;

static void pool_get_wrong_type(void)
{
    CuArena a;
    CuPool p;
    CuHandle h;
    long v;

    cu_arena_init(&a);
    CU_POOL_INIT(&p, &a, long);
    v = 1;
    h = CU_POOL_ADD(&p, &v);
    (void)CU_POOL_GET(&p, char, h);
}

static void pool_add_wrong_type(void)
{
    CuArena a;
    CuPool p;
    char v;

    cu_arena_init(&a);
    CU_POOL_INIT(&p, &a, long);
    v = 1;
    handle_sink = CU_POOL_ADD(&p, &v);
}

static void pool_null_arena(void)
{
    CuPool p;

    cu_pool_init(&p, NULL, sizeof(long));
}

static void pool_add_null_value(void)
{
    CuArena a;
    CuPool p;

    cu_arena_init(&a);
    CU_POOL_INIT(&p, &a, long);
    handle_sink = cu_pool_add_(&p, NULL, sizeof(long));
}

static void pool_zero_size(void)
{
    CuArena a;
    CuPool p;

    cu_arena_init(&a);
    cu_pool_init(&p, &a, 0);
}

static void pool_zero_max_gen(void)
{
    CuArena a;
    CuPool p;

    cu_arena_init(&a);
    cu_pool_init_with(&p, &a, sizeof(long), 0);
}

/* Contract violations abort. */
static void death_tests(void)
{
    (void)handle_sink;
    expect_abort(pool_get_wrong_type);
    expect_abort(pool_add_wrong_type);
    expect_abort(pool_zero_size);
    expect_abort(pool_zero_max_gen);
    expect_abort(pool_null_arena);
    expect_abort(pool_add_null_value);
}
#endif

int main(void)
{
    CuArena a;

    cu_arena_init(&a);
    test_add_get_remove(&a);
    test_reuse_bumps_generation(&a);
    test_bad_handles(&a);
    test_many_objects_stay_put(&a);
    test_retires_at_max_generation(&a);
    test_failed_add_is_sticky(&a);
    test_add_fails_at_every_allocation(0);
    test_add_fails_at_every_allocation(1);
    cu_arena_release(&a);
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
