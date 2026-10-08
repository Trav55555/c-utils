/* CBMC proof: for every pair of size_t values, cu_size_add succeeds exactly
 * when the compiler's overflow builtin says the result fits, and then stores
 * the exact result. cu_size_mul is checked by fuzz/fuzz_size.c instead: a
 * 64-bit divider and multiplier do not finish within the CBMC timeout. */
#include "cu.h"

size_t nondet_size_t(void);

int main(void)
{
    size_t a;
    size_t b;
    size_t got;
    size_t want;
    int ok;
    int overflow;

    a = nondet_size_t();
    b = nondet_size_t();

    got = 0;
    ok = cu_size_add(a, b, &got);
    overflow = __builtin_add_overflow(a, b, &want);
    __CPROVER_assert(ok == !overflow, "add reports overflow exactly");
    __CPROVER_assert(!ok || got == want, "add result is exact");
    __CPROVER_assert(ok || got == 0, "add leaves output unchanged on overflow");
    return 0;
}
