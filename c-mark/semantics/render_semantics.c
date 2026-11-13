/* Small end-to-end renders for Cerberus. Expected bytes come from the
 * independently pinned Goldmark corpus; full coverage remains in tests/. */
#include "cu_mark.h"
#include <stdio.h>

static int render_matches(const char *markdown, unsigned long flags,
                          const char *expected)
{
    CuArena scratch;
    CuArena output_arena;
    CuBuilder output;
    CuMarkOptions options;
    CuStr rendered;
    CuMarkStatus status;
    int matches;

    cu_arena_init(&scratch);
    cu_arena_init(&output_arena);
    options.flags = flags;
    options.table_align = CU_MARK_ALIGN_DEFAULT;
    options.code_hook = NULL;
    options.code_ctx = NULL;
    cu_sb_init(&output, &output_arena);
    status = cu_mark_render(&scratch, cu_str_cstr(markdown), &options, &output);
    matches = status == CU_MARK_OK && cu_sb_finish(&output, &rendered) &&
              cu_str_eq(rendered, cu_str_cstr(expected));
    cu_arena_release(&scratch);
    cu_arena_release(&output_arena);
    return matches;
}

int main(void)
{
    if (!render_matches("foo  \n", CU_MARK_UNSAFE | CU_MARK_XHTML,
                        "<p>foo</p>\n")) {
        fputs("plain paragraph render mismatch\n", stderr);
        return 1;
    }
    /* Goldmark corpus hugo/spec #62: all six headings share the foo ID
     * stem, exercising c-mark's duplicate-ID temp-scope path. */
    if (!render_matches("# foo\n## foo\n### foo\n#### foo\n##### foo\n###### foo\n",
                        CU_MARK_AUTO_HEADING_ID | CU_MARK_HUGO_IDS,
                        "<h1 id=\"foo\">foo</h1>\n"
                        "<h2 id=\"foo-1\">foo</h2>\n"
                        "<h3 id=\"foo-2\">foo</h3>\n"
                        "<h4 id=\"foo-3\">foo</h4>\n"
                        "<h5 id=\"foo-4\">foo</h5>\n"
                        "<h6 id=\"foo-5\">foo</h6>\n")) {
        fputs("duplicate heading render mismatch\n", stderr);
        return 1;
    }
    return 0;
}
