/* c-mark entry point (cu_mark.h). */
#include "cu_mark.h"

CuMarkStatus cu_mark_render(CuArena *scratch, CuStr src, const CuMarkOptions *opt, CuBuilder *out)
{
    (void)scratch;
    (void)opt;
    (void)out;
    if (src.len > CU_MARK_MAX_INPUT) return CU_MARK_LIMIT;
    return CU_MARK_OK;
}
