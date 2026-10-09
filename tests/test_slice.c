#include "death.h"
#include <assert.h>
#include <stddef.h>
#include "cu.h"

static void test_views_an_array(void)
{
    long arr[5];
    CuSlice s;
    size_t i;

    for (i = 0; i < 5; i++)
        arr[i] = (long)(i + 1) * 10;
    s = CU_SLICE(arr, 5);
    assert(s.len == 5 && s.elem_size == sizeof(long));
    assert(*CU_SLICE_AT(s, long, 0) == 10);
    assert(*CU_SLICE_AT(s, long, 4) == 50);
    assert(CU_SLICE_DATA(s, long) == arr);
    /* A view, not a copy: writes reach the array. */
    *CU_SLICE_AT(s, long, 2) = -3;
    assert(arr[2] == -3);
}

/* One-byte elements are an ordinary size, not a special case. */
static void test_byte_elements(void)
{
    char text[3] = {'a', 'b', 'c'};
    CuSlice s;

    s = CU_SLICE(text, 3);
    assert(s.len == 3 && s.elem_size == 1);
    assert(*CU_SLICE_AT(s, char, 2) == 'c');
    s = cu_slice_sub(s, 1, 3);
    assert(*CU_SLICE_AT(s, char, 0) == 'b');
}

static void test_sub_clamps(void)
{
    long arr[5] = {10, 20, 30, 40, 50};
    CuSlice s;
    CuSlice t;

    s = CU_SLICE(arr, 5);
    t = cu_slice_sub(s, 1, 3);
    assert(t.len == 2 && t.elem_size == sizeof(long));
    assert(CU_SLICE_DATA(t, long) == arr + 1);
    assert(*CU_SLICE_AT(t, long, 1) == 30);
    t = cu_slice_sub(s, 3, 100);
    assert(t.len == 2 && CU_SLICE_DATA(t, long) == arr + 3);
    t = cu_slice_sub(s, 4, 2);
    assert(t.len == 0 && CU_SLICE_DATA(t, long) == arr + 2);
    t = cu_slice_sub(s, 100, (size_t)-1);
    assert(t.len == 0 && CU_SLICE_DATA(t, long) == arr + 5);
    t = cu_slice_sub(s, 0, 5);
    assert(t.len == 5 && CU_SLICE_DATA(t, long) == arr);
}

static void test_empty_is_never_null(void)
{
    long *none;
    CuSlice s;

    none = NULL;
    s = CU_SLICE(none, 0);
    assert(s.len == 0 && s.ptr != NULL);
    assert(CU_SLICE_DATA(s, long) != NULL);
    s = cu_slice_sub(s, 0, 3);
    assert(s.len == 0 && s.ptr != NULL);
}

static void test_slices_a_vec(void)
{
    CuArena a;
    CuVec v;
    CuSlice s;
    long x;

    cu_arena_init(&a);
    CU_VEC_INIT(&v, &a, long);
    s = cu_vec_slice(&v);
    assert(s.len == 0 && s.elem_size == sizeof(long));
    assert(CU_SLICE_DATA(s, long) != NULL);
    for (x = 1; x <= 6; x++)
        CU_VEC_PUSH(&v, &x);
    s = cu_vec_slice(&v);
    assert(s.len == 6);
    assert(CU_SLICE_DATA(s, long) == CU_VEC_DATA(&v, long));
    assert(*CU_SLICE_AT(s, long, 5) == 6);
    s = cu_slice_sub(s, 2, 4);
    assert(*CU_SLICE_AT(s, long, 0) == 3 && *CU_SLICE_AT(s, long, 1) == 4);
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
static void slice_at_len(void)
{
    long arr[3] = {1, 2, 3};
    CuSlice s;

    s = CU_SLICE(arr, 3);
    (void)CU_SLICE_AT(s, long, 3);
}

static void slice_at_wrong_type(void)
{
    long arr[3] = {1, 2, 3};
    CuSlice s;

    s = CU_SLICE(arr, 3);
    (void)CU_SLICE_AT(s, char, 0);
}

static void slice_data_wrong_type(void)
{
    long arr[3] = {1, 2, 3};
    CuSlice s;

    s = CU_SLICE(arr, 3);
    (void)CU_SLICE_DATA(s, char);
}

static void slice_null_with_length(void)
{
    long *none;

    none = NULL;
    (void)CU_SLICE(none, 1);
}

/* No real array is this big, so the size must not wrap to a small one. */
static void slice_size_overflows(void)
{
    long arr[1];

    arr[0] = 0;
    (void)CU_SLICE(arr, (size_t)-1 / sizeof(long) + 1);
}

/* Bypasses the macro, which always passes a nonzero sizeof. */
static void slice_zero_size(void)
{
    long arr[1];

    arr[0] = 0;
    (void)cu_slice_n_(arr, 1, 0);
}

/* Contract violations abort. */
static void death_tests(void)
{
    expect_abort(slice_at_len);
    expect_abort(slice_at_wrong_type);
    expect_abort(slice_data_wrong_type);
    expect_abort(slice_null_with_length);
    expect_abort(slice_size_overflows);
    expect_abort(slice_zero_size);
}
#endif

int main(void)
{
    test_views_an_array();
    test_byte_elements();
    test_sub_clamps();
    test_empty_is_never_null();
    test_slices_a_vec();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
