#include "death.h"
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

/* Each entry is one allocation (debug mode counts them), and its value is
 * aligned for any type whatever the key lengths before it. */
static void test_one_allocation_per_entry(void)
{
    CuArena a;
    CuMap m;
    char key[8];
    double init;
    double *v;
    size_t i;
    size_t attempts;

    cu_arena_init_with(&a, 64, 1);
    CU_MAP_INIT(&m, &a, double);
    init = 0.5;
    for (i = 0; i < 7; i++) {
        memset(key, 'k', i + 1);
        attempts = a.attempts;
        v = CU_MAP_UPSERT(&m, double, cu_str_n(key, i + 1), &init);
        assert(v != NULL && a.attempts == attempts + 1);
        assert((size_t)v % sizeof(double) == 0 && (size_t)v % sizeof(void *) == 0);
        *v = (double)i;
    }
    cu_arena_release(&a);

    cu_arena_init_with(&a, 4096, 0);
    CU_MAP_INIT(&m, &a, double);
    for (i = 0; i < 7; i++) {
        memset(key, 'k', i + 1);
        v = CU_MAP_UPSERT(&m, double, cu_str_n(key, i + 1), &init);
        assert(v != NULL && (size_t)v % sizeof(double) == 0 && (size_t)v % sizeof(void *) == 0);
    }
    assert(*CU_MAP_GET(&m, double, CU_LIT("kkk")) > 0.25);
    cu_arena_release(&a);
}

/* An entry whose size overflows fails like any allocation: the map and the
 * arena are marked, and init is never read. */
static void test_entry_size_overflow_fails(void)
{
    CuArena a;
    CuMap m;
    long init;
    size_t huge;

    /* Shifts, not (size_t)-1 - 15: Cerberus (git-b3e11ea) truncates that
     * subtraction to 32 bits, which would make this value small. */
    huge = (size_t)-1 >> 4 << 4;
    cu_arena_init_with(&a, 64, 0);
    cu_map_init(&m, &a, huge);
    init = 1;
    assert(cu_map_upsert_(&m, CU_LIT("k"), &init, huge, huge) == NULL);
    assert(cu_map_failed(&m) && cu_arena_failed(&a) && cu_map_len(&m) == 0);
    cu_arena_release(&a);
}

/* Run n fails allocation n alone, so each allocation fails in turn while
 * the ones after it succeed; the runs end when one makes no more than n
 * allocations, and only that run may succeed. */
static void test_upsert_fails_at_every_allocation(int debug)
{
    static const char *const keys[] = {"one", "two", "", "four", "liquid", "costarring"};
    CuArena a;
    CuMap m;
    CuMapIter it;
    CuStr key;
    size_t at;
    int injected;
    long i;
    long k;
    long inserted;
    long *v;
    int done;

    for (at = 0;; at++) {
        cu_arena_init_with(&a, 64, debug);
        cu_arena_fail_at(&a, at);
        CU_MAP_INIT(&m, &a, long);
        inserted = 0;
        for (i = 0; i < 6; i++) {
            v = CU_MAP_UPSERT(&m, long, cu_str_cstr(keys[i]), &i);
            if (v == NULL)
                break;
            inserted++;
        }
        done = inserted == 6;
        if (!done) {
            /* The failed key is absent, the map is marked, later new keys
             * are refused, and every earlier key is intact and in order. */
            assert(cu_map_failed(&m) && cu_map_len(&m) == (size_t)inserted);
            assert(CU_MAP_GET(&m, long, cu_str_cstr(keys[inserted])) == NULL);
            assert(CU_MAP_UPSERT(&m, long, CU_LIT("later"), &i) == NULL);
        }
        it = cu_map_iter(&m);
        for (k = 0; k < inserted; k++) {
            assert(*CU_MAP_GET(&m, long, cu_str_cstr(keys[k])) == k);
            assert(*CU_MAP_NEXT(&it, long, &key) == k);
        }
        assert(CU_MAP_NEXT(&it, long, &key) == NULL);
        /* The run succeeds exactly when the failure was never reached. */
        injected = a.attempts > at;
        assert(done == !injected);
        cu_arena_release(&a);
        if (!injected)
            break;
    }
}

#ifdef CU_DEATH_TESTS
/* Runs one map call on a map of longs holding "k". */
static void with_long_map(void (*call)(CuMap *))
{
    CuArena a;
    CuMap m;
    long init;

    cu_arena_init(&a);
    CU_MAP_INIT(&m, &a, long);
    init = 1;
    if (CU_MAP_UPSERT(&m, long, CU_LIT("k"), &init) != NULL)
        call(&m);
    cu_arena_release(&a);
}

static void get_wrong_type(CuMap *m)
{
    (void)CU_MAP_GET(m, char, CU_LIT("k"));
}

static void upsert_wrong_type(CuMap *m)
{
    long init;

    init = 1;
    (void)CU_MAP_UPSERT(m, char, CU_LIT("k"), &init);
}

static void upsert_wrong_init(CuMap *m)
{
    char init;

    init = 1;
    (void)CU_MAP_UPSERT(m, long, CU_LIT("new"), &init);
}

static void next_wrong_type(CuMap *m)
{
    CuMapIter it;
    CuStr key;

    it = cu_map_iter(m);
    (void)CU_MAP_NEXT(&it, char, &key);
}

static void map_get_wrong_type(void)
{
    with_long_map(get_wrong_type);
}

static void map_upsert_wrong_type(void)
{
    with_long_map(upsert_wrong_type);
}

static void map_upsert_wrong_init(void)
{
    with_long_map(upsert_wrong_init);
}

static void map_next_wrong_type(void)
{
    with_long_map(next_wrong_type);
}

static void upsert_null_init(CuMap *m)
{
    (void)cu_map_upsert_(m, CU_LIT("new"), NULL, sizeof(long), sizeof(long));
}

static void next_null_key(CuMap *m)
{
    CuMapIter it;

    it = cu_map_iter(m);
    (void)cu_map_next_(&it, NULL, sizeof(long));
}

static void map_upsert_null_init(void)
{
    with_long_map(upsert_null_init);
}

static void map_next_null_key(void)
{
    with_long_map(next_null_key);
}

static void map_null_arena(void)
{
    CuMap m;

    cu_map_init(&m, NULL, sizeof(long));
}

static void map_zero_size(void)
{
    CuArena a;
    CuMap m;

    cu_arena_init(&a);
    cu_map_init(&m, &a, 0);
}

/* Contract violations abort. */
static void death_tests(void)
{
    expect_abort(map_get_wrong_type);
    expect_abort(map_upsert_wrong_type);
    expect_abort(map_upsert_wrong_init);
    expect_abort(map_next_wrong_type);
    expect_abort(map_zero_size);
    expect_abort(map_upsert_null_init);
    expect_abort(map_next_null_key);
    expect_abort(map_null_arena);
}
#endif

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
    test_upsert_fails_at_every_allocation(0);
    test_upsert_fails_at_every_allocation(1);
    test_one_allocation_per_entry();
    test_entry_size_overflow_fails();
    cu_arena_release(&a);
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
