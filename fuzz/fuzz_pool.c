/* libFuzzer harness: any sequence of adds and removes, with a small
 * max_gen so slots retire, matches a reference model. After every step
 * each handle ever issued gets exactly its object while live and NULL once
 * removed, and a forged handle gets NULL unless it equals a live one. */
#include <stddef.h>
#include <stdint.h>
#include "cu.h"

#define REF_MAX 512

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuArena a;
    CuPool p;
    CuHandle hs[REF_MAX];
    unsigned long vals[REF_MAX];
    int live[REF_MAX];
    size_t n;
    size_t live_count;
    size_t i;
    size_t k;
    unsigned long v;
    unsigned long *got;
    CuHandle forged;
    int matches;

    if (size < 1)
        return 0;
    cu_arena_init_with(&a, 256, data[0] & 1);
    cu_pool_init_with(&p, &a, sizeof(unsigned long), (unsigned long)(data[0] >> 1) % 4 + 1);
    n = 0;
    live_count = 0;
    for (i = 1; i < size; i++) {
        if (data[i] % 3 != 0 && n < REF_MAX) {
            v = i;
            hs[n] = CU_POOL_ADD(&p, &v);
            if (cu_handle_is_null(hs[n]))
                __builtin_trap();
            vals[n] = v;
            live[n] = 1;
            n++;
            live_count++;
        } else if (n > 0) {
            k = data[i] % n;
            if (cu_pool_remove(&p, hs[k]) != live[k])
                __builtin_trap();
            if (live[k]) {
                live[k] = 0;
                live_count--;
            }
        }
        if (cu_pool_len(&p) != live_count || cu_pool_failed(&p))
            __builtin_trap();
        for (k = 0; k < n; k++) {
            got = CU_POOL_GET(&p, unsigned long, hs[k]);
            if (live[k] ? got == NULL || *got != vals[k] : got != NULL)
                __builtin_trap();
        }
        forged.index = data[i] % 8;
        forged.gen = data[i] / 8 % 4;
        matches = 0;
        for (k = 0; k < n; k++) {
            if (live[k] && hs[k].index == forged.index && hs[k].gen == forged.gen)
                matches = 1;
        }
        if ((CU_POOL_GET(&p, unsigned long, forged) != NULL) != matches)
            __builtin_trap();
    }
    cu_arena_release(&a);
    return 0;
}
