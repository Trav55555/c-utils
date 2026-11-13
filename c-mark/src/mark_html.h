#ifndef MARK_HTML_H
#define MARK_HTML_H
#include "mark_parser.h"

/* goldmark renderer/html/html.go: renders a parsed document to out. The
 * walk is iterative (ast.Walk recurses in Go). Output failures are sticky
 * on out; the code hook's own failures go through out as well. */
void mark_render_html(MarkCtx *ctx, MarkNode *doc, const CuMarkOptions *opt, CuBuilder *out);

#endif
