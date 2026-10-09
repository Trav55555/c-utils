/* libFuzzer harness: random adds and removes over two node kinds of
 * different sizes match a reference model. After every step each link ever
 * issued gets its node under its own kind while live, NULL once removed,
 * and NULL under the other kind. */
#include <stddef.h>
#include <stdint.h>
#include "cu.h"

#define REF_MAX 256

typedef struct Big {
    unsigned long v;
    CuLink other;
} Big;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuArena a;
    CuPlex px;
    unsigned kinds[2];
    CuLink links[REF_MAX];
    unsigned long vals[REF_MAX];
    int live[REF_MAX];
    size_t n;
    size_t i;
    size_t k;
    unsigned long small;
    Big big;
    unsigned long got;
    int found;
    int which;

    cu_arena_init_with(&a, 256, size > 0 && (data[0] & 1));
    cu_plex_init(&px, &a);
    kinds[0] = CU_PLEX_KIND(&px, unsigned long);
    kinds[1] = CU_PLEX_KIND(&px, Big);
    if (kinds[0] != 1 || kinds[1] != 2)
        __builtin_trap();
    n = 0;
    for (i = 1; i < size; i++) {
        which = data[i] & 1;
        if (data[i] % 3 != 0 && n < REF_MAX) {
            if (which == 0) {
                small = i;
                links[n] = CU_PLEX_ADD(&px, kinds[0], &small);
            } else {
                big.v = i;
                big.other = n > 0 ? links[n - 1] : cu_link_null();
                links[n] = CU_PLEX_ADD(&px, kinds[1], &big);
            }
            if (cu_link_is_null(links[n]) || links[n].kind != kinds[which])
                __builtin_trap();
            vals[n] = i;
            live[n] = 1;
            n++;
        } else if (n > 0) {
            k = data[i] % n;
            if (cu_plex_remove(&px, links[k]) != live[k])
                __builtin_trap();
            live[k] = 0;
        }
        if (cu_plex_failed(&px))
            __builtin_trap();
        for (k = 0; k < n; k++) {
            if (links[k].kind == kinds[0]) {
                found = CU_PLEX_GET(&px, unsigned long, kinds[0], links[k]) != NULL;
                got = found ? *CU_PLEX_GET(&px, unsigned long, kinds[0], links[k]) : 0;
                if (CU_PLEX_GET(&px, Big, kinds[1], links[k]) != NULL)
                    __builtin_trap();
            } else {
                found = CU_PLEX_GET(&px, Big, kinds[1], links[k]) != NULL;
                got = found ? CU_PLEX_GET(&px, Big, kinds[1], links[k])->v : 0;
                if (CU_PLEX_GET(&px, unsigned long, kinds[0], links[k]) != NULL)
                    __builtin_trap();
            }
            if (found != live[k] || (found && got != vals[k]))
                __builtin_trap();
        }
    }
    cu_arena_release(&a);
    return 0;
}
