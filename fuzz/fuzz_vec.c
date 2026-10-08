/* libFuzzer harness: any sequence of push, pop, clear and reserve on a CuVec
 * matches a plain fixed-size reference array, in both arena modes. */
#include <stddef.h>
#include <stdint.h>
#include "cu.h"

#define REF_MAX 4096

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuArena a;
    CuVec v;
    unsigned ref[REF_MAX];
    size_t ref_len;
    size_t i;
    size_t k;
    unsigned x;

    if (size < 1)
        return 0;
    cu_arena_init_with(&a, 64, data[0] & 1);
    CU_VEC_INIT(&v, &a, unsigned);
    ref_len = 0;
    for (i = 1; i < size; i++) {
        switch (data[i] % 4) {
        case 0:
        case 1:
            if (ref_len == REF_MAX)
                break;
            x = data[i];
            CU_VEC_PUSH(&v, &x);
            ref[ref_len++] = x;
            break;
        case 2:
            x = 0;
            if (CU_VEC_POP(&v, &x) != (ref_len != 0))
                __builtin_trap();
            if (ref_len != 0 && x != ref[--ref_len])
                __builtin_trap();
            break;
        default:
            if (data[i] & 4) {
                cu_vec_clear(&v);
                ref_len = 0;
            } else if (!cu_vec_reserve(&v, data[i] >> 3)) {
                __builtin_trap();
            }
            break;
        }
        if (cu_vec_failed(&v) || cu_vec_len(&v) != ref_len)
            __builtin_trap();
    }
    for (k = 0; k < ref_len; k++) {
        if (*CU_VEC_AT(&v, unsigned, k) != ref[k] || CU_VEC_DATA(&v, unsigned)[k] != ref[k])
            __builtin_trap();
    }
    cu_arena_release(&a);
    return 0;
}
