/* libFuzzer harness for c-minify. The first byte picks the type (bit 0:
 * XML, else HTML) and the options (bit 1: the site's flags, else bits 2-7
 * are the KEEP_* flags). The rest is the document.
 *
 * Checks: the minifier never crashes and ends as OK, LIMIT, NOMEM,
 * UNSUPPORTED or INVALID; a run that succeeded succeeds again with the
 * same bytes (a read of uninitialised or freed memory would show as a
 * difference under the debug arenas, where every push is its own
 * allocation); and with the scratch arena capped it ends as NOMEM or
 * exactly as before. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cu_minify.h"

static CuMinifyStatus minify(CuArena *scratch, CuArena *a, CuMinifyType type, const CuMinifyOptions *opt, CuStr src,
                             CuStr *out)
{
    CuBuilder b;
    CuMinifyStatus st;

    cu_sb_init(&b, a);
    st = cu_minify(scratch, type, opt, src, &b);
    if (!cu_sb_finish(&b, out)) *out = cu_str_empty();
    return st;
}

static int status_ok(CuMinifyStatus st)
{
    return st == CU_MINIFY_OK || st == CU_MINIFY_LIMIT || st == CU_MINIFY_NOMEM || st == CU_MINIFY_UNSUPPORTED ||
           st == CU_MINIFY_INVALID;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuArena a;
    CuArena scratch;
    CuMinifyOptions opt;
    CuMinifyType type;
    CuStr src;
    CuStr first;
    CuStr again;
    CuMinifyStatus st;
    CuMinifyStatus st2;

    if (size < 1 || size > 1 << 16) return 0;
    type = (data[0] & 1) ? CU_MINIFY_XML : CU_MINIFY_HTML;
    opt.flags = (data[0] & 2) ? CU_MINIFY_SITE : (unsigned long)(data[0] >> 2);
    src = cu_str_n((const char *)data + 1, size - 1);
    cu_arena_init_with(&a, 4096, 1);
    cu_arena_init_with(&scratch, 4096, 1);

    st = minify(&scratch, &a, type, &opt, src, &first);
    if (!status_ok(st)) __builtin_trap();
    if (st == CU_MINIFY_OK) {
        st2 = minify(&scratch, &a, type, &opt, src, &again);
        if (st2 != CU_MINIFY_OK || !cu_str_eq(first, again)) __builtin_trap();
    }

    cu_arena_release(&scratch);
    cu_arena_init_with(&scratch, 4096, 1);
    cu_arena_set_limit(&scratch, 2048 + (size % 16) * 512);
    st2 = minify(&scratch, &a, type, &opt, src, &again);
    if (!status_ok(st2)) __builtin_trap();
    if (st2 != CU_MINIFY_NOMEM && (st2 != st || (st == CU_MINIFY_OK && !cu_str_eq(first, again)))) __builtin_trap();

    cu_arena_release(&scratch);
    cu_arena_release(&a);
    return 0;
}
