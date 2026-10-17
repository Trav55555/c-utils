/* CBMC proof: for every pair of valid CuU64 values, cu_u64_cmp orders them
 * as the 64-bit values they stand for, cu_u64_add and cu_u64_sub succeed
 * exactly when the result fits in 64 bits and then store it, and a failed
 * call leaves *out unchanged. Every size_t converts to a CuU64 and back
 * unchanged. The reference uses unsigned long long, which CBMC provides;
 * the library itself stays C89. */
#include "cu.h"

unsigned long nondet_ulong(void);
size_t nondet_size_t(void);

static CuU64 any_u64(void)
{
    CuU64 v;

    v.hi = nondet_ulong();
    v.lo = nondet_ulong();
    __CPROVER_assume(v.hi <= 0xFFFFFFFFUL && v.lo <= 0xFFFFFFFFUL);
    return v;
}

static unsigned long long value(CuU64 v)
{
    return (unsigned long long)v.hi << 32 | (unsigned long long)v.lo;
}

int main(void)
{
    CuU64 a;
    CuU64 b;
    CuU64 r;
    unsigned long long want;
    int c;
    int ok;
    size_t n;
    size_t back;

    a = any_u64();
    b = any_u64();

    c = cu_u64_cmp(a, b);
    __CPROVER_assert((c < 0) == (value(a) < value(b)), "cmp less");
    __CPROVER_assert((c == 0) == (value(a) == value(b)), "cmp equal");

    r.hi = 7;
    r.lo = 7;
    ok = cu_u64_add(a, b, &r);
    want = value(a) + value(b);
    __CPROVER_assert(ok == (want >= value(a)), "add fails exactly on overflow");
    __CPROVER_assert(!ok || value(r) == want, "add result is exact");
    __CPROVER_assert(ok || (r.hi == 7 && r.lo == 7), "failed add changes nothing");
    __CPROVER_assert(r.hi <= 0xFFFFFFFFUL && r.lo <= 0xFFFFFFFFUL, "add result is valid");

    r.hi = 7;
    r.lo = 7;
    ok = cu_u64_sub(a, b, &r);
    __CPROVER_assert(ok == (value(a) >= value(b)), "sub fails exactly when b > a");
    __CPROVER_assert(!ok || value(r) == value(a) - value(b), "sub result is exact");
    __CPROVER_assert(ok || (r.hi == 7 && r.lo == 7), "failed sub changes nothing");
    __CPROVER_assert(r.hi <= 0xFFFFFFFFUL && r.lo <= 0xFFFFFFFFUL, "sub result is valid");

    n = nondet_size_t();
    r = cu_u64_from_size(n);
    __CPROVER_assert(value(r) == (unsigned long long)n, "from_size keeps the value");
    back = 0;
    __CPROVER_assert(cu_u64_to_size(r, &back) && back == n, "size round trip");
    return 0;
}
