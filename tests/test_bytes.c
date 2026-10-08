#include <assert.h>
#include <string.h>
#include "cu.h"

/* Asymmetric bytes: a swapped order or offset reads a different value. */
static const char seq[] = "\x01\x02\x03\x04\x05";

static void test_reads_both_orders(void)
{
    CuStr s;
    size_t pos;
    unsigned long v;

    s = cu_str_n(seq, 5);
    pos = 0;
    assert(cu_rd_u16le(s, &pos, &v) && v == 0x0201UL && pos == 2);
    pos = 0;
    assert(cu_rd_u16be(s, &pos, &v) && v == 0x0102UL && pos == 2);
    pos = 0;
    assert(cu_rd_u32le(s, &pos, &v) && v == 0x04030201UL && pos == 4);
    pos = 0;
    assert(cu_rd_u32be(s, &pos, &v) && v == 0x01020304UL && pos == 4);
    assert(cu_rd_u8(s, &pos, &v) && v == 0x05UL && pos == 5);
    pos = 1;
    assert(cu_rd_u32le(s, &pos, &v) && v == 0x05040302UL && pos == 5);
}

/* High bytes must not sign-extend: char may be signed, and unsigned char
 * promotes to int, where 0xFF << 24 overflows. */
static void test_high_bytes_are_unsigned(void)
{
    CuStr ones;
    CuStr top;
    size_t pos;
    unsigned long v;

    ones = cu_str_n("\xff\xff\xff\xff", 4);
    pos = 0;
    assert(cu_rd_u32le(ones, &pos, &v) && v == 0xFFFFFFFFUL);
    pos = 0;
    assert(cu_rd_u32be(ones, &pos, &v) && v == 0xFFFFFFFFUL);
    pos = 0;
    assert(cu_rd_u16le(ones, &pos, &v) && v == 0xFFFFUL);
    pos = 0;
    assert(cu_rd_u8(ones, &pos, &v) && v == 0xFFUL);
    top = cu_str_n("\x00\x00\x00\x80", 4);
    pos = 0;
    assert(cu_rd_u32le(top, &pos, &v) && v == 0x80000000UL);
    pos = 0;
    assert(cu_rd_u32be(top, &pos, &v) && v == 0x80UL);
}

/* A failed read leaves pos and out untouched. */
static void expect_short(int ok, size_t pos, size_t pos_before, unsigned long v)
{
    assert(!ok);
    assert(pos == pos_before);
    assert(v == 12345UL);
}

static void test_short_input_fails_unchanged(void)
{
    CuStr s;
    size_t pos;
    unsigned long v;
    int ok;

    s = cu_str_n(seq, 4);
    v = 12345UL;
    pos = 1;
    ok = cu_rd_u32le(s, &pos, &v);
    expect_short(ok, pos, 1, v);
    pos = 1;
    ok = cu_rd_u32be(s, &pos, &v);
    expect_short(ok, pos, 1, v);
    pos = 3;
    ok = cu_rd_u16le(s, &pos, &v);
    expect_short(ok, pos, 3, v);
    pos = 3;
    ok = cu_rd_u16be(s, &pos, &v);
    expect_short(ok, pos, 3, v);
    pos = 4;
    ok = cu_rd_u8(s, &pos, &v);
    expect_short(ok, pos, 4, v);
    pos = 5;
    ok = cu_rd_u8(s, &pos, &v);
    expect_short(ok, pos, 5, v);
    pos = (size_t)-1;
    ok = cu_rd_u32le(s, &pos, &v);
    expect_short(ok, pos, (size_t)-1, v);
    pos = 0;
    ok = cu_rd_u8(cu_str_empty(), &pos, &v);
    expect_short(ok, pos, 0, v);
    /* Exact fit succeeds. */
    pos = 0;
    assert(cu_rd_u32le(s, &pos, &v) && pos == 4);
}

static void test_read_bytes(void)
{
    CuStr s;
    CuStr out;
    CuStr keep;
    size_t pos;

    s = cu_str_n(seq, 5);
    pos = 1;
    assert(cu_rd_bytes(s, &pos, 3, &out));
    assert(pos == 4 && out.ptr == s.ptr + 1 && out.len == 3);
    assert(cu_rd_bytes(s, &pos, 1, &out) && pos == 5);
    assert(cu_rd_bytes(s, &pos, 0, &out) && pos == 5 && out.len == 0);
    keep = cu_str_n("x", 1);
    out = keep;
    pos = 2;
    assert(!cu_rd_bytes(s, &pos, 4, &out));
    assert(pos == 2 && out.ptr == keep.ptr && out.len == 1);
    assert(!cu_rd_bytes(s, &pos, (size_t)-1, &out) && pos == 2);
    pos = 6;
    assert(!cu_rd_bytes(s, &pos, 0, &out) && pos == 6);
}

static void test_writes_exact_bytes(CuArena *a)
{
    CuBuilder b;
    CuStr out;

    cu_sb_init(&b, a);
    cu_sb_u8(&b, 0xABUL);
    cu_sb_u16le(&b, 0x0102UL);
    cu_sb_u16be(&b, 0x0102UL);
    cu_sb_u32le(&b, 0x01020304UL);
    cu_sb_u32be(&b, 0x01020304UL);
    cu_sb_u32le(&b, 0xFFFFFFFFUL);
    cu_sb_u16be(&b, 0xFFFFUL);
    cu_sb_u8(&b, 0UL);
    assert(cu_sb_finish(&b, &out));
    assert(out.len == 20);
    assert(memcmp(out.ptr,
                  "\xab"
                  "\x02\x01"
                  "\x01\x02"
                  "\x04\x03\x02\x01"
                  "\x01\x02\x03\x04"
                  "\xff\xff\xff\xff"
                  "\xff\xff"
                  "\x00",
                  20) == 0);
}

/* Every value written reads back, at every width and order. */
static void test_round_trip(CuArena *a)
{
    static const unsigned long vals[] = {0UL, 1UL, 0x7FUL, 0x80UL, 0xFFUL, 0x1234UL,
                                         0xFFFFUL, 0x12345678UL, 0x80000000UL, 0xFFFFFFFFUL};
    CuBuilder b;
    CuStr out;
    size_t i;
    size_t pos;
    unsigned long v;

    cu_sb_init(&b, a);
    for (i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        if (vals[i] <= 0xFFUL)
            cu_sb_u8(&b, vals[i]);
        if (vals[i] <= 0xFFFFUL) {
            cu_sb_u16le(&b, vals[i]);
            cu_sb_u16be(&b, vals[i]);
        }
        cu_sb_u32le(&b, vals[i]);
        cu_sb_u32be(&b, vals[i]);
    }
    assert(cu_sb_finish(&b, &out));
    pos = 0;
    for (i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        if (vals[i] <= 0xFFUL)
            assert(cu_rd_u8(out, &pos, &v) && v == vals[i]);
        if (vals[i] <= 0xFFFFUL) {
            assert(cu_rd_u16le(out, &pos, &v) && v == vals[i]);
            assert(cu_rd_u16be(out, &pos, &v) && v == vals[i]);
        }
        assert(cu_rd_u32le(out, &pos, &v) && v == vals[i]);
        assert(cu_rd_u32be(out, &pos, &v) && v == vals[i]);
    }
    assert(pos == out.len);
}

int main(void)
{
    CuArena a;

    cu_arena_init(&a);
    test_reads_both_orders();
    test_high_bytes_are_unsigned();
    test_short_input_fails_unchanged();
    test_read_bytes();
    test_writes_exact_bytes(&a);
    test_round_trip(&a);
    cu_arena_release(&a);
    return 0;
}
