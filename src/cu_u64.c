#include <stdlib.h>
#include "cu.h"

#define M32 0xFFFFFFFFUL

static void check(CuU64 v)
{
    if (v.hi > M32 || v.lo > M32)
        abort();
}

CuU64 cu_u64(unsigned long hi, unsigned long lo)
{
    CuU64 v;

    v.hi = hi;
    v.lo = lo;
    check(v);
    return v;
}

/* Shifts go 16 bits at a time: a single shift by 32 is undefined where
 * long is 32 bits, and a shift by 16 is defined for every unsigned long. */
CuU64 cu_u64_from_ulong(unsigned long v)
{
    unsigned long hi;

    hi = v >> 16 >> 16;
    if (hi >> 16 >> 16 != 0)
        abort();
    return cu_u64(hi, v & M32);
}

/* size_t may be as narrow as 16 bits, so it shifts 8 bits at a time: a
 * shift by 8 is defined for every size_t, where a shift by 16 or 32 is
 * not. Conversions to size_t keep the low bits, as unsigned conversions
 * do. */
static size_t shr32(size_t v)
{
    return v >> 8 >> 8 >> 8 >> 8;
}

CuU64 cu_u64_from_size(size_t v)
{
    size_t hi;

    hi = shr32(v);
    if (shr32(hi) != 0)
        abort();
    return cu_u64((unsigned long)hi & M32, (unsigned long)v & M32);
}

/* Builds the size_t that keeps v's low bits, then checks that it converts
 * back to v: it does exactly when nothing was dropped. */
int cu_u64_to_size(CuU64 v, size_t *out)
{
    size_t r;

    check(v);
    if (out == NULL)
        abort();
    r = (size_t)v.hi << 8 << 8 << 8 << 8 | (size_t)v.lo;
    if (cu_u64_cmp(cu_u64_from_size(r), v) != 0)
        return 0;
    *out = r;
    return 1;
}

int cu_u64_cmp(CuU64 a, CuU64 b)
{
    check(a);
    check(b);
    if (a.hi != b.hi)
        return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo)
        return a.lo < b.lo ? -1 : 1;
    return 0;
}

/* Sums are masked to 32 bits, so a carry shows as a sum smaller than an
 * operand both where long wraps at 32 bits and where it does not. */
int cu_u64_add(CuU64 a, CuU64 b, CuU64 *out)
{
    unsigned long lo;
    unsigned long hi;
    unsigned long carry;

    check(a);
    check(b);
    if (out == NULL)
        abort();
    lo = (a.lo + b.lo) & M32;
    carry = lo < a.lo;
    hi = (a.hi + b.hi) & M32;
    if (hi < a.hi)
        return 0;
    if (carry && hi == M32)
        return 0;
    out->hi = hi + carry;
    out->lo = lo;
    return 1;
}

int cu_u64_sub(CuU64 a, CuU64 b, CuU64 *out)
{
    unsigned long borrow;

    if (out == NULL)
        abort();
    if (cu_u64_cmp(a, b) < 0)
        return 0;
    borrow = a.lo < b.lo;
    out->lo = (a.lo - b.lo) & M32;
    out->hi = a.hi - b.hi - borrow;
    return 1;
}
