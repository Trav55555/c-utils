/* libFuzzer harness: any sequence of upserts and lookups on short keys
 * matches a linear-search reference: same values, same length, value
 * pointers that never move, and iteration in insertion order. */
#include <stddef.h>
#include <stdint.h>
#include "cu.h"

#define REF_MAX 1024

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuArena a;
    CuMap m;
    CuMapIter it;
    CuStr keys[REF_MAX];
    unsigned long *vals[REF_MAX];
    size_t n;
    size_t i;
    size_t j;
    size_t klen;
    unsigned long init;
    unsigned long *v;
    CuStr key;
    int upsert;

    cu_arena_init_with(&a, 256, size > 0 && (data[0] & 1));
    CU_MAP_INIT(&m, &a, unsigned long);
    n = 0;
    i = 1;
    while (i < size) {
        upsert = data[i] & 1;
        klen = (data[i] >> 1) % 4;
        i++;
        if (klen > size - i)
            klen = size - i;
        key = cu_str_n((const char *)data + i, klen);
        i += klen;
        for (j = 0; j < n && !cu_str_eq(keys[j], key); j++)
            ;
        if (upsert && (j < n || n < REF_MAX)) {
            init = n;
            v = CU_MAP_UPSERT(&m, unsigned long, key, &init);
            if (v == NULL)
                __builtin_trap();
            if (j == n) {
                keys[n] = key;
                vals[n] = v;
                n++;
            }
        } else {
            v = CU_MAP_GET(&m, unsigned long, key);
        }
        if (j < n ? v != vals[j] || *v != j : v != NULL)
            __builtin_trap();
        if (cu_map_len(&m) != n || cu_map_failed(&m))
            __builtin_trap();
    }
    it = cu_map_iter(&m);
    for (j = 0; j < n; j++) {
        v = CU_MAP_NEXT(&it, unsigned long, &key);
        if (v != vals[j] || !cu_str_eq(key, keys[j]))
            __builtin_trap();
    }
    if (CU_MAP_NEXT(&it, unsigned long, &key) != NULL)
        __builtin_trap();
    cu_arena_release(&a);
    return 0;
}
