/* CBMC proof: in either arena mode, with arbitrary element values, a CuVec
 * keeps what is pushed. Five pushes cross the initial capacity of 4, and the
 * 16-byte block size forces new blocks. Pops return elements last-first,
 * pop on empty reports empty, and clear empties the vector while later
 * pushes still work.
 *
 * The schedule is fixed: with a nondeterministic push or pop at each step,
 * CBMC runs out of memory at 3 steps. fuzz/fuzz_vec.c checks arbitrary
 * sequences against a reference array instead. */
#include "cu.h"

#define N 5

int nondet_int(void);
long nondet_long(void);

/* Allocation never fails here. CBMC's default malloc may fail, which forks
 * every push and also exhausts memory; the failure paths are covered by
 * tests/test_vec.c. */
void *malloc(size_t n)
{
    return __CPROVER_allocate(n, 0);
}

int main(void)
{
    CuArena a;
    CuVec v;
    long ref[N];
    long x;
    int i;

    cu_arena_init_with(&a, 16, nondet_int() != 0);
    CU_VEC_INIT(&v, &a, long);
    x = 0;
    __CPROVER_assert(CU_VEC_POP(&v, &x) == 0, "pop on new vector reports empty");
    for (i = 0; i < N; i++) {
        ref[i] = nondet_long();
        CU_VEC_PUSH(&v, &ref[i]);
    }
    __CPROVER_assert(!cu_vec_failed(&v), "pushes succeed");
    __CPROVER_assert(cu_vec_len(&v) == N, "length counts pushes");
    for (i = 0; i < N; i++)
        __CPROVER_assert(*CU_VEC_AT(&v, long, i) == ref[i], "elements match");
    for (i = N - 1; i >= 2; i--) {
        __CPROVER_assert(CU_VEC_POP(&v, &x) == 1, "pop on non-empty succeeds");
        __CPROVER_assert(x == ref[i], "pop returns the last push");
    }
    __CPROVER_assert(cu_vec_len(&v) == 2, "pops shrink length");
    cu_vec_clear(&v);
    __CPROVER_assert(cu_vec_len(&v) == 0, "clear empties");
    __CPROVER_assert(CU_VEC_POP(&v, &x) == 0, "pop after clear reports empty");
    x = nondet_long();
    CU_VEC_PUSH(&v, &x);
    __CPROVER_assert(cu_vec_len(&v) == 1 && *CU_VEC_AT(&v, long, 0) == x,
                     "push after clear");
    cu_arena_release(&a);
    return 0;
}
