#include <assert.h>
#include <string.h>
#include "cu.h"

/* Published FNV-1a 32-bit test vectors. */
static void test_hash_vectors(void)
{
    assert(cu_hash_fnv1a(CU_LIT("")) == 0x811c9dc5UL);
    assert(cu_hash_fnv1a(CU_LIT("a")) == 0xe40c292cUL);
    assert(cu_hash_fnv1a(CU_LIT("foobar")) == 0xbf9cf968UL);
}

static void test_upsert_inserts_once(CuArena *a)
{
    CuMap m;
    long init;
    long *v;
    long *again;

    CU_MAP_INIT(&m, a, long);
    assert(cu_map_len(&m) == 0);
    assert(CU_MAP_GET(&m, long, CU_LIT("x")) == NULL);
    init = 7;
    v = CU_MAP_UPSERT(&m, long, CU_LIT("x"), &init);
    assert(v != NULL && *v == 7 && cu_map_len(&m) == 1);
    *v = 8;
    init = 100;
    again = CU_MAP_UPSERT(&m, long, CU_LIT("x"), &init);
    assert(again == v && *again == 8 && cu_map_len(&m) == 1);
    assert(CU_MAP_GET(&m, long, CU_LIT("x")) == v);
    assert(CU_MAP_GET(&m, long, CU_LIT("y")) == NULL);
    assert(!cu_map_failed(&m));
}

/* The map keeps its own copy of each key. */
static void test_keys_are_copied(CuArena *a)
{
    CuMap m;
    char buf[3];
    long init;

    CU_MAP_INIT(&m, a, long);
    memcpy(buf, "abc", 3);
    init = 1;
    assert(CU_MAP_UPSERT(&m, long, cu_str_n(buf, 3), &init) != NULL);
    memcpy(buf, "zzz", 3);
    assert(CU_MAP_GET(&m, long, CU_LIT("abc")) != NULL);
    assert(CU_MAP_GET(&m, long, CU_LIT("zzz")) == NULL);
}

/* Equal 32-bit hashes share the whole trie path and end in a list. */
static void test_colliding_keys_stay_distinct(CuArena *a)
{
    static const char *const pairs[4][2] = {
        {"costarring", "liquid"}, {"declinate", "macallums"},
        {"altarage", "zinke"}, {"altarages", "zinkes"}};
    CuMap m;
    long i;
    long k;

    for (i = 0; i < 4; i++) {
        assert(cu_hash_fnv1a(cu_str_cstr(pairs[i][0])) == cu_hash_fnv1a(cu_str_cstr(pairs[i][1])));
    }
    CU_MAP_INIT(&m, a, long);
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 2; k++) {
            long init;

            init = i * 2 + k;
            assert(CU_MAP_UPSERT(&m, long, cu_str_cstr(pairs[i][k]), &init) != NULL);
        }
    }
    assert(cu_map_len(&m) == 8);
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 2; k++)
            assert(*CU_MAP_GET(&m, long, cu_str_cstr(pairs[i][k])) == i * 2 + k);
    }
}

static CuStr key_for(CuArena *a, unsigned long i)
{
    CuBuilder b;
    CuStr out;

    cu_sb_init(&b, a);
    cu_sb_char(&b, 'k');
    cu_sb_ulong(&b, i);
    assert(cu_sb_finish(&b, &out));
    return out;
}

/* Enough keys to fill several trie levels. Cerberus interprets this test
 * and needs ~50 s at 200 keys; fuzz/fuzz_map.c covers larger maps. */
#define MANY 200UL

/* Many keys: all found, and the first value pointer never moves. */
static void test_many_keys_and_stable_pointers(CuArena *a)
{
    CuMap m;
    unsigned long i;
    unsigned long init;
    unsigned long *first;
    CuTemp t;

    CU_MAP_INIT(&m, a, unsigned long);
    init = 0;
    first = CU_MAP_UPSERT(&m, unsigned long, key_for(a, 0), &init);
    assert(first != NULL);
    for (i = 1; i < MANY; i++) {
        init = i;
        assert(CU_MAP_UPSERT(&m, unsigned long, key_for(a, i), &init) != NULL);
    }
    assert(cu_map_len(&m) == MANY);
    t = cu_temp_begin(a);
    for (i = 0; i < MANY; i++)
        assert(*CU_MAP_GET(&m, unsigned long, key_for(a, i)) == i);
    cu_temp_end(t);
    assert(CU_MAP_GET(&m, unsigned long, key_for(a, 0)) == first);
    assert(CU_MAP_GET(&m, unsigned long, key_for(a, MANY)) == NULL);
}

static void test_iterates_in_insertion_order(CuArena *a)
{
    static const char *const keys[] = {"pear", "", "apple", "fig", "liquid", "costarring"};
    CuMap m;
    CuMapIter it;
    CuStr key;
    long *v;
    long i;
    long init;

    CU_MAP_INIT(&m, a, long);
    it = cu_map_iter(&m);
    assert(CU_MAP_NEXT(&it, long, &key) == NULL);
    for (i = 0; i < 6; i++) {
        init = i;
        assert(CU_MAP_UPSERT(&m, long, cu_str_cstr(keys[i]), &init) != NULL);
    }
    /* Re-upserting does not move a key in the order. */
    init = 99;
    assert(*CU_MAP_UPSERT(&m, long, CU_LIT("pear"), &init) == 0);
    it = cu_map_iter(&m);
    for (i = 0; i < 6; i++) {
        v = CU_MAP_NEXT(&it, long, &key);
        assert(v != NULL && *v == i);
        assert(cu_str_eq(key, cu_str_cstr(keys[i])));
    }
    assert(CU_MAP_NEXT(&it, long, &key) == NULL);
    assert(CU_MAP_NEXT(&it, long, &key) == NULL);
}

int main(void)
{
    CuArena a;

    cu_arena_init(&a);
    test_hash_vectors();
    test_upsert_inserts_once(&a);
    test_keys_are_copied(&a);
    test_colliding_keys_stay_distinct(&a);
    test_many_keys_and_stable_pointers(&a);
    test_iterates_in_insertion_order(&a);
    cu_arena_release(&a);
    return 0;
}
