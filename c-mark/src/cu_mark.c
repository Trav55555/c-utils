/* c-mark entry point (cu_mark.h): goldmark's markdown.Convert, which parses
 * with the default parser and renders with the HTML renderer. */
#include "cu_mark.h"
#include "mark_html.h"
#include "mark_parser.h"

CuMarkStatus cu_mark_render(CuArena *scratch, CuStr src, const CuMarkOptions *opt, CuBuilder *out)
{
    CuMarkOptions defaults;
    CuTemp t;
    MarkCtx ctx;
    MarkNode *doc;
    CuMarkStatus status;

    if (src.len > CU_MARK_MAX_INPUT) return CU_MARK_LIMIT;
    if (opt == NULL) {
        defaults.flags = 0;
        defaults.table_align = CU_MARK_ALIGN_DEFAULT;
        defaults.code_hook = NULL;
        defaults.code_ctx = NULL;
        opt = &defaults;
    }
    t = cu_temp_begin(scratch);
    mark_ctx_init(&ctx, scratch, src, opt->flags);
    doc = mark_parse(&ctx);
    if (doc != NULL) mark_render_html(&ctx, doc, opt, out);
    if (ctx.limit) status = CU_MARK_LIMIT;
    else if (ctx.failed || cu_arena_failed(scratch) || out->failed) status = CU_MARK_NOMEM;
    else status = CU_MARK_OK;
    cu_temp_end(t);
    return status;
}
