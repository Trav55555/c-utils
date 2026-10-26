/* libFuzzer harness: inflating any input either fails, or gives data that
 * the stream's own bytes alone inflate to again and that survives the
 * writer and inflating back. Writing the input and inflating it gives the
 * input. Agreement with zlib itself is pinned by the vectors in
 * tests/test_zlib.c; the seeds are streams zlib wrote. */
#include <stddef.h>
#include <stdint.h>
#include "cu.h"

static void round_trip(CuArena *a, CuStr data)
{
    CuBuilder b;
    CuStr z;
    CuStr back;
    size_t used;

    cu_sb_init(&b, a);
    cu_sb_zlib(&b, data);
    if (!cu_sb_finish(&b, &z))
        __builtin_trap();
    if (!cu_zlib_inflate(a, z, data.len, &back, &used) || used != z.len ||
        !cu_str_eq(back, data))
        __builtin_trap();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuArena a;
    CuStr in;
    CuStr out;
    CuStr again;
    size_t used;
    size_t used_again;

    in = cu_str_n((const char *)data, size);
    cu_arena_init(&a);
    if (cu_zlib_inflate(&a, in, 1UL << 20, &out, &used)) {
        if (used > size)
            __builtin_trap();
        if (!cu_zlib_inflate(&a, cu_str_prefix(in, used), out.len, &again, &used_again) ||
            used_again != used || !cu_str_eq(again, out))
            __builtin_trap();
        round_trip(&a, out);
    }
    round_trip(&a, in);
    cu_arena_release(&a);
    return 0;
}
