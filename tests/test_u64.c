#include "death.h"
#include <assert.h>
#include "cu.h"

#define M32 0xFFFFFFFFUL

static int is(CuU64 v, unsigned long hi, unsigned long lo)
{
    return v.hi == hi && v.lo == lo;
}

static void test_construct(void)
{
    CuU64 v;

    v = cu_u64(1UL, 2UL);
    assert(is(v, 1UL, 2UL));
    v = cu_u64(M32, M32);
    assert(is(v, M32, M32));
    v = cu_u64_from_ulong(0UL);
    assert(is(v, 0UL, 0UL));
    v = cu_u64_from_ulong(M32);
    assert(is(v, 0UL, M32));
    v = cu_u64_from_size(0);
    assert(is(v, 0UL, 0UL));
    v = cu_u64_from_size((size_t)0xFFFFUL);
    assert(is(v, 0UL, 0xFFFFUL));
}

/* Where long or size_t is wider than 32 bits, the upper bits land in hi. */
static void test_construct_wide(void)
{
    unsigned long wide;
    size_t max;
    CuU64 v;

    wide = ((5UL << 16) << 16) | 7UL;
    if ((wide >> 16 >> 16) != 0) {
        v = cu_u64_from_ulong(wide);
        assert(is(v, 5UL, 7UL));
        v = cu_u64_from_ulong((unsigned long)-1);
        assert(is(v, M32, M32));
    }
    max = (size_t)-1;
    v = cu_u64_from_size(max);
    if (max > (size_t)M32)
        assert(v.hi != 0 && v.lo == M32);
    else
        assert(is(v, 0UL, (unsigned long)max));
}

static void test_to_size(void)
{
    size_t r;
    size_t max;

    r = 7;
    assert(cu_u64_to_size(cu_u64(0UL, 42UL), &r) && r == 42);
    max = (size_t)-1;
    assert(cu_u64_to_size(cu_u64_from_size(max), &r) && r == max);
    r = 7;
    if (max <= (size_t)M32) {
        assert(!cu_u64_to_size(cu_u64(M32, M32), &r) && r == 7);
        assert(!cu_u64_to_size(cu_u64(1UL, 0UL), &r) && r == 7);
    } else {
        assert(cu_u64_to_size(cu_u64(M32, M32), &r) && is(cu_u64_from_size(r), M32, M32));
        assert(cu_u64_to_size(cu_u64(1UL, 0UL), &r) && (r >> 16 >> 16) == 1 && (r & M32) == 0);
    }
}

static void test_cmp(void)
{
    assert(cu_u64_cmp(cu_u64(0UL, 0UL), cu_u64(0UL, 0UL)) == 0);
    assert(cu_u64_cmp(cu_u64(1UL, 0UL), cu_u64(0UL, M32)) > 0);
    assert(cu_u64_cmp(cu_u64(0UL, M32), cu_u64(1UL, 0UL)) < 0);
    assert(cu_u64_cmp(cu_u64(3UL, 4UL), cu_u64(3UL, 5UL)) < 0);
    assert(cu_u64_cmp(cu_u64(3UL, 5UL), cu_u64(3UL, 4UL)) > 0);
    assert(cu_u64_cmp(cu_u64(M32, M32), cu_u64(M32, M32)) == 0);
}

static void test_add(void)
{
    CuU64 r;

    assert(cu_u64_add(cu_u64(0UL, 2UL), cu_u64(0UL, 3UL), &r) && is(r, 0UL, 5UL));
    /* The low half carries into the high half. */
    assert(cu_u64_add(cu_u64(0UL, M32), cu_u64(0UL, 1UL), &r) && is(r, 1UL, 0UL));
    assert(cu_u64_add(cu_u64(1UL, M32), cu_u64(2UL, M32), &r) && is(r, 4UL, M32 - 1UL));
    assert(cu_u64_add(cu_u64(M32, M32 - 1UL), cu_u64(0UL, 1UL), &r) && is(r, M32, M32));
    /* Overflow fails and leaves *out unchanged, whether it comes from the
     * high halves or from the carry. */
    r = cu_u64(9UL, 9UL);
    assert(!cu_u64_add(cu_u64(M32, M32), cu_u64(0UL, 1UL), &r) && is(r, 9UL, 9UL));
    assert(!cu_u64_add(cu_u64(M32, 0UL), cu_u64(1UL, 0UL), &r) && is(r, 9UL, 9UL));
    assert(!cu_u64_add(cu_u64(M32, M32), cu_u64(M32, M32), &r) && is(r, 9UL, 9UL));
}

static void test_sub(void)
{
    CuU64 r;

    assert(cu_u64_sub(cu_u64(0UL, 5UL), cu_u64(0UL, 3UL), &r) && is(r, 0UL, 2UL));
    /* The low half borrows from the high half. */
    assert(cu_u64_sub(cu_u64(1UL, 0UL), cu_u64(0UL, 1UL), &r) && is(r, 0UL, M32));
    assert(cu_u64_sub(cu_u64(M32, M32), cu_u64(M32, M32), &r) && is(r, 0UL, 0UL));
    r = cu_u64(9UL, 9UL);
    assert(!cu_u64_sub(cu_u64(0UL, 3UL), cu_u64(0UL, 5UL), &r) && is(r, 9UL, 9UL));
    assert(!cu_u64_sub(cu_u64(1UL, 0UL), cu_u64(1UL, 1UL), &r) && is(r, 9UL, 9UL));
}

#ifdef CU_DEATH_TESTS
/* A half wider than 32 bits only exists where long is wider than 32 bits. */
static unsigned long two_to_32(void)
{
    return (1UL << 16) << 16;
}

static void hi_too_large(void)
{
    (void)cu_u64(two_to_32(), 0UL);
}

static void lo_too_large(void)
{
    (void)cu_u64(0UL, two_to_32());
}

static void add_bad_half(void)
{
    CuU64 bad;
    CuU64 r;

    bad.hi = 0;
    bad.lo = two_to_32();
    assert(cu_u64_add(bad, cu_u64(0UL, 0UL), &r));
}

static void cmp_bad_half(void)
{
    CuU64 bad;

    bad.hi = two_to_32();
    bad.lo = 0;
    (void)cu_u64_cmp(cu_u64(0UL, 0UL), bad);
}

static void add_null_out(void)
{
    assert(cu_u64_add(cu_u64(0UL, 1UL), cu_u64(0UL, 1UL), NULL));
}

static void sub_null_out(void)
{
    assert(cu_u64_sub(cu_u64(0UL, 1UL), cu_u64(0UL, 1UL), NULL));
}

static void to_size_null_out(void)
{
    assert(cu_u64_to_size(cu_u64(0UL, 1UL), NULL));
}

/* Contract violations abort. */
static void death_tests(void)
{
    if (two_to_32() != 0) {
        expect_abort(hi_too_large);
        expect_abort(lo_too_large);
        expect_abort(add_bad_half);
        expect_abort(cmp_bad_half);
    }
    expect_abort(add_null_out);
    expect_abort(sub_null_out);
    expect_abort(to_size_null_out);
}
#endif

int main(void)
{
    test_construct();
    test_construct_wide();
    test_to_size();
    test_cmp();
    test_add();
    test_sub();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
