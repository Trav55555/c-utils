/* libFuzzer harness: parsing arbitrary bytes never crashes, anything that
 * parses formats back to an equal value, and formatting any value parses
 * back to it. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cu.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuArena a;
    CuBuilder b;
    CuStr s;
    CuStr out;
    unsigned long u;
    unsigned long u2;
    long l;
    long l2;
    unsigned base;

    if (size < 1)
        return 0;
    base = 2 + data[0] % 35;
    s = cu_str_n((const char *)data + 1, size - 1);
    cu_arena_init(&a);

    if (cu_parse_ulong(s, base, &u)) {
        cu_sb_init(&b, &a);
        cu_sb_ulong(&b, u);
        if (!cu_sb_finish(&b, &out) || !cu_parse_ulong(out, 10, &u2) || u2 != u)
            __builtin_trap();
    }
    if (cu_parse_long(s, base, &l)) {
        cu_sb_init(&b, &a);
        cu_sb_long(&b, l);
        if (!cu_sb_finish(&b, &out) || !cu_parse_long(out, 10, &l2) || l2 != l)
            __builtin_trap();
    }
    if (size - 1 >= sizeof l) {
        memcpy(&l, data + 1, sizeof l);
        cu_sb_init(&b, &a);
        cu_sb_long(&b, l);
        cu_sb_char(&b, ' ');
        cu_sb_hex(&b, (unsigned long)l, 0);
        if (!cu_sb_finish(&b, &out))
            __builtin_trap();
        if (!cu_str_cut(out, CU_LIT(" "), &s, &out))
            __builtin_trap();
        if (!cu_parse_long(s, 10, &l2) || l2 != l)
            __builtin_trap();
        if (!cu_parse_ulong(out, 16, &u2) || u2 != (unsigned long)l)
            __builtin_trap();
    }
    cu_arena_release(&a);
    return 0;
}
