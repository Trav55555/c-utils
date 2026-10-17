/* libFuzzer harness: CuU64 compare, add and subtract agree with uint64_t on
 * values read from the input in both byte orders, overflow fails exactly
 * when uint64_t wraps, and every value read writes back as the same bytes. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cu.h"

static uint64_t value(CuU64 v)
{
    return (uint64_t)v.hi << 32 | (uint64_t)v.lo;
}

static void check_pair(CuU64 a, CuU64 b)
{
    CuU64 r;
    int c;
    int ok;

    c = cu_u64_cmp(a, b);
    if ((c < 0) != (value(a) < value(b)) || (c == 0) != (value(a) == value(b)))
        __builtin_trap();
    r = cu_u64(7, 7);
    ok = cu_u64_add(a, b, &r);
    if (ok != (value(a) + value(b) >= value(a)))
        __builtin_trap();
    if (ok ? value(r) != value(a) + value(b) : (r.hi != 7 || r.lo != 7))
        __builtin_trap();
    r = cu_u64(7, 7);
    ok = cu_u64_sub(a, b, &r);
    if (ok != (value(a) >= value(b)))
        __builtin_trap();
    if (ok ? value(r) != value(a) - value(b) : (r.hi != 7 || r.lo != 7))
        __builtin_trap();
}

static void check_write(CuArena *a, CuStr src, CuU64 v, int big)
{
    CuBuilder b;
    CuStr back;

    cu_sb_init(&b, a);
    if (big)
        cu_sb_u64be(&b, v);
    else
        cu_sb_u64le(&b, v);
    if (!cu_sb_finish(&b, &back) || back.len != 8 || memcmp(back.ptr, src.ptr, 8) != 0)
        __builtin_trap();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuArena arena;
    CuStr s;
    size_t pos;
    CuU64 a;
    CuU64 b;
    int big;

    if (size < 17)
        return 0;
    s = cu_str_n((const char *)data + 1, size - 1);
    big = data[0] & 1;
    pos = 0;
    if (!(big ? cu_rd_u64be(s, &pos, &a) : cu_rd_u64le(s, &pos, &a)))
        __builtin_trap();
    if (!(big ? cu_rd_u64be(s, &pos, &b) : cu_rd_u64le(s, &pos, &b)))
        __builtin_trap();
    check_pair(a, b);
    check_pair(b, a);
    check_pair(a, a);
    cu_arena_init(&arena);
    check_write(&arena, s, a, big);
    cu_arena_release(&arena);
    return 0;
}
