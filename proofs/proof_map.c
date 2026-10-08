/* CBMC proof: for any contents of two 2-byte keys and a 1-byte key, with
 * any allocation failing, upsert and get agree with key equality: a key
 * maps to one value, distinct keys to distinct values, and a failed
 * insertion leaves the map as it was, marks it failed, and refuses later
 * new keys while existing keys are still found.
 *
 * Key lengths and the schedule are fixed, and the arena is in debug mode:
 * symbolic key lengths copied to symbolic offsets in a shared block exhaust
 * memory even at two keys. Arbitrary lengths and sequences are fuzzed
 * against a reference in fuzz/fuzz_map.c. malloc keeps CBMC's default
 * failure. */
#include "cu.h"

#define KEY_MAX 2

char nondet_char(void);

static CuStr any_key(char *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        buf[i] = nondet_char();
    return cu_str_n(buf, len);
}

int main(void)
{
    char b1[KEY_MAX];
    char b2[KEY_MAX];
    char b3[KEY_MAX];
    CuStr k1;
    CuStr k2;
    CuStr k3;
    CuArena a;
    CuMap m;
    long init;
    long *v1;
    long *v2;
    long *v3;

    k1 = any_key(b1, 2);
    k2 = any_key(b2, 2);
    k3 = any_key(b3, 1);
    cu_arena_init_with(&a, 64, 1);
    CU_MAP_INIT(&m, &a, long);

    init = 1;
    v1 = CU_MAP_UPSERT(&m, long, k1, &init);
    __CPROVER_assume(v1 != NULL);
    __CPROVER_assert(*v1 == 1 && cu_map_len(&m) == 1, "first insert copies init");

    init = 2;
    v2 = CU_MAP_UPSERT(&m, long, k2, &init);
    if (cu_str_eq(k1, k2)) {
        __CPROVER_assert(v2 == v1 && *v2 == 1 && cu_map_len(&m) == 1, "equal key finds the value");
    } else if (v2 != NULL) {
        __CPROVER_assert(v2 != v1 && *v2 == 2 && cu_map_len(&m) == 2, "new key gets its own value");
        __CPROVER_assert(CU_MAP_GET(&m, long, k1) == v1, "first key still found");
    } else {
        __CPROVER_assert(cu_map_failed(&m) && cu_map_len(&m) == 1, "failed insert changes nothing");
        __CPROVER_assert(CU_MAP_GET(&m, long, k2) == NULL, "failed key is absent");
        __CPROVER_assert(CU_MAP_GET(&m, long, k1) == v1, "existing key survives failure");
        init = 3;
        v3 = CU_MAP_UPSERT(&m, long, k3, &init);
        __CPROVER_assert(cu_str_eq(k3, k1) ? v3 == v1 : v3 == NULL, "failure is sticky");
    }
    __CPROVER_assert(CU_MAP_GET(&m, long, k2) == (v2 != NULL ? v2 : (long *)NULL),
                     "get agrees with upsert");
    cu_arena_release(&a);
    return 0;
}
