#include "death.h"
#include <assert.h>
#include "cu.h"

/* Constants mixed with size_t are written (size_t)n: Cerberus (deep tier)
 * miscomputes size_t - int near SIZE_MAX, though C converts the int first. */

static void test_add(void)
{
    size_t r;
    size_t max;

    max = (size_t)-1;
    r = 7;
    assert(cu_size_add(2, 3, &r) == 1 && r == 5);
    assert(cu_size_add(max - (size_t)1, 1, &r) == 1 && r == max);
    r = 7;
    assert(cu_size_add(max, 1, &r) == 0 && r == 7);
    assert(cu_size_add(1, max, &r) == 0 && r == 7);
}

static void test_mul(void)
{
    size_t r;
    size_t max;

    max = (size_t)-1;
    assert(cu_size_mul(6, 7, &r) == 1 && r == 42);
    assert(cu_size_mul(0, max, &r) == 1 && r == 0);
    assert(cu_size_mul(max, 1, &r) == 1 && r == max);
    r = 7;
    assert(cu_size_mul(max / (size_t)2 + (size_t)1, 2, &r) == 0 && r == 7);
    assert(cu_size_mul(max, max, &r) == 0 && r == 7);
}

#ifdef CU_DEATH_TESTS
static void add_null_out(void)
{
    assert(cu_size_add(1, 2, NULL));
}

static void mul_null_out(void)
{
    assert(cu_size_mul(1, 2, NULL));
}

static void death_tests(void)
{
    expect_abort(add_null_out);
    expect_abort(mul_null_out);
}
#endif

int main(void)
{
    test_add();
    test_mul();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
