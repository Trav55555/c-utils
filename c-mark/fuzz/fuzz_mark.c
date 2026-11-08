/* libFuzzer harness for c-mark. The first byte picks the options: bit 0 the
 * Hugo configuration, bits 1-2 the table alignment method, bit 3 and bit 7
 * two groups of extensions, bits 4-6 the renderer options. The second byte
 * turns on a fenced-code hook that reads every field it is handed. The rest
 * is the document.
 *
 * Checks: the render never crashes and ends as OK, LIMIT or NOMEM; a
 * render that succeeded succeeds again with the same bytes (a read of
 * uninitialised or freed memory would show as a difference under the
 * debug arenas, where every push is its own allocation); and with the
 * scratch arena capped it ends as NOMEM or exactly as before. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cu_mark.h"

static int hook(void *ctx, const CuMarkCode *code, CuBuilder *out)
{
    size_t i;

    (void)ctx;
    cu_sb_str(out, CU_LIT("<pre data-info=\""));
    cu_sb_str(out, code->info);
    cu_sb_str(out, CU_LIT("\" data-lang=\""));
    cu_sb_str(out, code->lang);
    cu_sb_str(out, CU_LIT("\""));
    for (i = 0; i < code->attr_count; i++) {
        cu_sb_str(out, CU_LIT(" "));
        cu_sb_str(out, code->attrs[i].name);
        cu_sb_str(out, CU_LIT("="));
        cu_sb_str(out, code->attrs[i].value);
    }
    cu_sb_str(out, CU_LIT(">"));
    cu_sb_str(out, code->text);
    cu_sb_str(out, CU_LIT("</pre>\n"));
    return 1;
}

static void options_of(uint8_t sel, int with_hook, CuMarkOptions *opt)
{
    opt->flags = 0;
    if (sel & 0x01) opt->flags |= CU_MARK_HUGO;
    if (sel & 0x08) opt->flags |= CU_MARK_TABLE | CU_MARK_STRIKETHROUGH | CU_MARK_LINKIFY | CU_MARK_TASKLIST;
    if (sel & 0x80) {
        opt->flags |= CU_MARK_TYPOGRAPHER | CU_MARK_DEFINITION_LIST | CU_MARK_FOOTNOTE | CU_MARK_AUTO_HEADING_ID |
                      CU_MARK_ATTRIBUTE;
    }
    if (sel & 0x10) opt->flags |= CU_MARK_UNSAFE;
    if (sel & 0x20) opt->flags |= CU_MARK_XHTML;
    if (sel & 0x40) opt->flags |= CU_MARK_HARD_WRAPS;
    opt->table_align = (CuMarkTableAlign)((sel >> 1) & 3);
    opt->code_hook = with_hook ? hook : NULL;
    opt->code_ctx = NULL;
}

/* Renders src into a fresh builder on a; *out holds the bytes on success. */
static CuMarkStatus render(CuArena *scratch, CuArena *a, CuStr src, const CuMarkOptions *opt, CuStr *out)
{
    CuBuilder b;
    CuMarkStatus st;

    cu_sb_init(&b, a);
    st = cu_mark_render(scratch, src, opt, &b);
    if (!cu_sb_finish(&b, out)) *out = cu_str_empty();
    return st;
}

static int status_ok(CuMarkStatus st)
{
    return st == CU_MARK_OK || st == CU_MARK_LIMIT || st == CU_MARK_NOMEM;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuArena a;
    CuArena scratch;
    CuMarkOptions opt;
    CuStr src;
    CuStr first;
    CuStr again;
    CuMarkStatus st;
    CuMarkStatus st2;

    if (size < 2 || size > 1 << 16) return 0;
    options_of(data[0], data[1] & 1, &opt);
    src = cu_str_n((const char *)data + 2, size - 2);
    cu_arena_init_with(&a, 4096, 1);
    cu_arena_init_with(&scratch, 4096, 1);

    st = render(&scratch, &a, src, &opt, &first);
    if (!status_ok(st)) __builtin_trap();
    if (st == CU_MARK_LIMIT && first.len != 0) __builtin_trap();
    if (st == CU_MARK_OK) {
        st2 = render(&scratch, &a, src, &opt, &again);
        if (st2 != CU_MARK_OK || !cu_str_eq(first, again)) __builtin_trap();
    }

    cu_arena_release(&scratch);
    cu_arena_init_with(&scratch, 4096, 1);
    cu_arena_set_limit(&scratch, 2048 + (size % 16) * 512);
    st2 = render(&scratch, &a, src, &opt, &again);
    if (!status_ok(st2)) __builtin_trap();
    if (st2 != CU_MARK_NOMEM && (st2 != st || (st == CU_MARK_OK && !cu_str_eq(first, again)))) __builtin_trap();

    cu_arena_release(&scratch);
    cu_arena_release(&a);
    return 0;
}
