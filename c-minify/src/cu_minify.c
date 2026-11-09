/* c-minify entry point (cu_minify.h): minify.M.Minify for one of the two
 * ported top-level media types. */
#include "minify_parse.h"
#include <string.h>

CuMinifyStatus cu_minify(CuArena *scratch, CuMinifyType type, const CuMinifyOptions *opt, CuStr src, CuBuilder *out)
{
    CuTemp t;
    MinCtx ctx;
    unsigned char *buf;
    CuMinifyStatus status;

    if (src.len > CU_MINIFY_MAX_INPUT) return CU_MINIFY_LIMIT;
    ctx.a = scratch;
    ctx.flags = opt != NULL ? opt->flags : CU_MINIFY_SITE;
    ctx.depth = 0;
    ctx.failed = 0;
    ctx.limit = 0;
    ctx.unsupported = 0;
    ctx.invalid = 0;
    ctx.attr_buf.ptr = NULL;
    ctx.attr_buf.cap = 0;
    t = cu_temp_begin(scratch);
    /* parse.NewInput: the bytes followed by a NUL, mutable, since the
     * lexers lowercase and rewrite in place. */
    buf = (unsigned char *)cu_arena_push_bytes(scratch, src.len + 1);
    if (buf == NULL) {
        ctx.failed = 1;
    } else {
        if (src.len > 0) memcpy(buf, src.ptr, src.len);
        buf[src.len] = 0;
        if (type == CU_MINIFY_HTML) minify_html(&ctx, min_bytes(buf, src.len), out);
        else minify_xml(&ctx, min_bytes(buf, src.len), out);
    }
    if (ctx.failed || cu_arena_failed(scratch) || out->failed) status = CU_MINIFY_NOMEM;
    else if (ctx.limit) status = CU_MINIFY_LIMIT;
    else if (ctx.unsupported) status = CU_MINIFY_UNSUPPORTED;
    else if (ctx.invalid) status = CU_MINIFY_INVALID;
    else status = CU_MINIFY_OK;
    cu_temp_end(t);
    return status;
}
