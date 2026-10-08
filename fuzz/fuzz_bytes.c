/* libFuzzer harness: at every position, every reader agrees with a
 * multiply-and-add reference, fails exactly when too few bytes remain, and
 * leaves pos and out unchanged when it fails; every value read writes back
 * as the same bytes. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cu.h"

typedef int (*Reader)(CuStr, size_t *, unsigned long *);
typedef void (*Writer)(CuBuilder *, unsigned long);

static unsigned long ref(const uint8_t *p, size_t n, int big)
{
    unsigned long v;
    unsigned long scale;
    size_t k;

    v = 0;
    scale = 1;
    for (k = 0; k < n; k++) {
        v += (unsigned long)p[big ? n - 1 - k : k] * scale;
        scale *= 256;
    }
    return v;
}

static void check(CuArena *a, CuStr s, size_t pos, Reader rd, Writer wr, size_t n, int big)
{
    CuBuilder b;
    CuStr back;
    size_t p;
    unsigned long v;
    int ok;

    p = pos;
    v = 777;
    ok = rd(s, &p, &v);
    if (ok != (pos <= s.len && n <= s.len - pos))
        __builtin_trap();
    if (!ok) {
        if (p != pos || v != 777)
            __builtin_trap();
        return;
    }
    if (p != pos + n || v != ref((const uint8_t *)s.ptr + pos, n, big))
        __builtin_trap();
    cu_sb_init(&b, a);
    wr(&b, v);
    if (!cu_sb_finish(&b, &back) || back.len != n || memcmp(back.ptr, s.ptr + pos, n) != 0)
        __builtin_trap();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuArena a;
    CuStr s;
    CuStr view;
    size_t pos;
    size_t p;

    s = cu_str_n((const char *)data, size);
    cu_arena_init(&a);
    for (pos = 0; pos <= size + 1; pos++) {
        check(&a, s, pos, cu_rd_u8, cu_sb_u8, 1, 0);
        check(&a, s, pos, cu_rd_u16le, cu_sb_u16le, 2, 0);
        check(&a, s, pos, cu_rd_u16be, cu_sb_u16be, 2, 1);
        check(&a, s, pos, cu_rd_u32le, cu_sb_u32le, 4, 0);
        check(&a, s, pos, cu_rd_u32be, cu_sb_u32be, 4, 1);
        p = pos;
        view = cu_str_empty();
        if (cu_rd_bytes(s, &p, size / 2, &view) != (pos <= size && size / 2 <= size - pos))
            __builtin_trap();
        if (p != pos && (view.ptr != s.ptr + pos || view.len != size / 2 || p != pos + size / 2))
            __builtin_trap();
    }
    cu_arena_release(&a);
    return 0;
}
