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

    cu_arena_init(&a);
    CU_VEC_INIT(&v, &a, long);
    n = 1;
    CU_VEC_PUSH(&v, &n);
    assert(!cu_vec_reserve(&v, (size_t)-1));
    assert(cu_vec_failed(&v));
    CU_VEC_PUSH(&v, &n);
    assert(cu_vec_len(&v) == 1);
    assert(*CU_VEC_AT(&v, long, 0) == 1);
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

int main(void)
{
    int debug;

    for (debug = 0; debug <= 1; debug++) {
        test_push_and_at(debug);
        test_reserve_keeps_data_in_place(debug);
    }
    test_push_copies();
    test_pop();
    test_clear_keeps_storage();
    test_failure_is_sticky();
    test_empty_data_is_not_null();
    return 0;
}
