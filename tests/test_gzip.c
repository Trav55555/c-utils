#include "death.h"
#include <assert.h>
#include "cu.h"
#include "gzip_vectors.h"

static void check_crcs(const struct crc_vector *v, size_t n)
{
    CuStr d;
    size_t i;
    size_t cut;

    for (i = 0; i < n; i++) {
        d = cu_str_n((const char *)v[i].data, v[i].len);
        assert(cu_crc32(0, d) == v[i].crc);
        /* Continued in two parts, the sum is the same. */
        cut = d.len / 3;
        assert(cu_crc32(cu_crc32(0, cu_str_prefix(d, cut)), cu_str_skip(d, cut)) == v[i].crc);
    }
}

static void test_crc32(void)
{
    check_crcs(small_crcs, sizeof small_crcs / sizeof small_crcs[0]);
#ifndef __cerb__
    check_crcs(large_crcs, sizeof large_crcs / sizeof large_crcs[0]);
#endif
}

/* Each member gets zlib's verdict, and an accepted one its data. */
static void check_members(const struct member *members, size_t n)
{
    const struct member *m;
    CuArena a;
    CuStr out;
    size_t used;
    size_t i;
    int ok;

    cu_arena_init(&a);
    for (i = 0; i < n; i++) {
        m = &members[i];
        used = 12345;
        ok = cu_gzip_inflate(&a, cu_str_n((const char *)m->z, m->len), 1UL << 20, &out, &used);
        assert(ok == m->ok);
        if (ok) {
            assert(used == m->used && out.len == m->data_len);
            assert(cu_crc32(0, out) == m->data_crc);
        } else {
            assert(used == 12345 && out.len == 0);
        }
    }
    cu_arena_release(&a);
}

static void test_members(void)
{
    check_members(small_members, sizeof small_members / sizeof small_members[0]);
#ifndef __cerb__
    check_members(large_members, sizeof large_members / sizeof large_members[0]);
#endif
}

/* Output past the cap fails, as for zlib streams. */
static void test_cap(void)
{
    const struct member *m;
    CuArena a;
    CuStr out;
    CuStr in;
    size_t used;

    m = &small_members[0];
    while (!(m->ok && m->data_len == 6))
        m++;
    in = cu_str_n((const char *)m->z, m->len);
    cu_arena_init(&a);
    assert(cu_gzip_inflate(&a, in, 6, &out, &used) && out.len == 6);
    assert(!cu_gzip_inflate(&a, in, 5, &out, &used) && out.len == 0);
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
static void null_out(void)
{
    CuArena a;
    size_t used;

    cu_arena_init(&a);
    assert(!cu_gzip_inflate(&a, cu_str_empty(), 1, NULL, &used));
}

static void crc_too_big(void)
{
    (void)cu_crc32(0xFFFFFFFFUL + 1UL, cu_str_empty());
}
#endif

int main(void)
{
    test_crc32();
    test_members();
    test_cap();
#ifdef CU_DEATH_TESTS
    expect_abort(null_out);
    if (sizeof(unsigned long) > 4)
        expect_abort(crc_too_big);
#endif
    return 0;
}
