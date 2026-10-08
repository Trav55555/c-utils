/* libFuzzer harness: cu_size_add and cu_size_mul agree with the compiler's
 * overflow builtins on every input, including the large values where CBMC
 * cannot finish a proof of the multiply. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cu.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t a;
    size_t b;
    size_t got;
    size_t want;
    int ok;
    int overflow;

    if (size < 2 * sizeof(size_t))
        return 0;
    memcpy(&a, data, sizeof a);
    memcpy(&b, data + sizeof a, sizeof b);

    got = 0;
    ok = cu_size_mul(a, b, &got);
    overflow = __builtin_mul_overflow(a, b, &want);
    if (ok != !overflow || (ok && got != want) || (!ok && got != 0))
        __builtin_trap();

    got = 0;
    ok = cu_size_add(a, b, &got);
    overflow = __builtin_add_overflow(a, b, &want);
    if (ok != !overflow || (ok && got != want) || (!ok && got != 0))
        __builtin_trap();
    return 0;
}
