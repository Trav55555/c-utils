#ifndef CU_MINIFY_H
#define CU_MINIFY_H
#include "cu.h"

/* c-minify: HTML and XML minification, a Dependable C port of
 * tdewolff/minify v2.20.5 with tdewolff/parse v2.7.3 (github.com/tdewolff,
 * MIT, Copyright (c) 2015 Taco de Wolff).
 *
 * For every input, the output is the byte string minify writes when Hugo
 * 0.120.4 runs it with --minify for traversannan.com: Hugo's default
 * tdewolff configuration plus the site's html keepComments. The oracle
 * (c-minify/oracle) defines that string; the corpus and differential tests
 * check it. Where minify has a quirk, c-minify has the same quirk: the
 * point is to replace Hugo's minifier without changing a published byte.
 *
 * The HTML minifier hands embedded content to other minifiers: <style>
 * and style attributes to CSS, <script> and on* attributes to JS, inline
 * <svg> to SVG, JSON script types to JSON, and iframe content, data: URLs
 * and XML script types back to HTML or XML. HTML and XML are ported; an
 * input whose minification would reach the CSS, JS, JSON or SVG minifier
 * ends as CU_MINIFY_UNSUPPORTED, with nothing to rely on in out.
 * Not ported: template delimiters, the URL option (relative scheme
 * stripping), and minifier sets other than Hugo's. */

typedef enum CuMinifyType {
    CU_MINIFY_HTML = 1, /* text/html */
    CU_MINIFY_XML       /* application/xml, application/rss+xml, text/xml */
} CuMinifyType;

/* html.Minifier fields, plus xml.Minifier's KeepWhitespace (the same flag
 * drives both, as a document and the XML it embeds run under one set). */
#define CU_MINIFY_KEEP_COMMENTS             0x01UL
#define CU_MINIFY_KEEP_CONDITIONAL_COMMENTS 0x02UL
#define CU_MINIFY_KEEP_DEFAULT_ATTR_VALS    0x04UL
#define CU_MINIFY_KEEP_DOCUMENT_TAGS        0x08UL
#define CU_MINIFY_KEEP_END_TAGS             0x10UL
#define CU_MINIFY_KEEP_QUOTES               0x20UL
#define CU_MINIFY_KEEP_WHITESPACE           0x40UL

/* Hugo 0.120.4's defaults (minifiers/config.go) with the site's
 * keepComments. This is the configuration the port is checked against;
 * the other flag combinations exist for minify's own test tables. */
#define CU_MINIFY_SITE (CU_MINIFY_KEEP_COMMENTS | CU_MINIFY_KEEP_CONDITIONAL_COMMENTS | \
                        CU_MINIFY_KEEP_DEFAULT_ATTR_VALS | CU_MINIFY_KEEP_DOCUMENT_TAGS | CU_MINIFY_KEEP_END_TAGS)

typedef struct CuMinifyOptions {
    unsigned long flags;
} CuMinifyOptions;

typedef enum CuMinifyStatus {
    CU_MINIFY_OK = 0,
    CU_MINIFY_NOMEM,       /* an arena allocation failed */
    CU_MINIFY_LIMIT,       /* the input passes a bound below */
    CU_MINIFY_UNSUPPORTED, /* the input reaches a minifier that is not ported */
    CU_MINIFY_INVALID      /* minify returns an error for the input: a NUL byte
                            * where its XML lexer forbids one (XML, or inline
                            * svg/math in HTML), or JS it cannot parse */
} CuMinifyStatus;

/* Bounds. minify has none; a dependable minifier needs them. An input
 * inside them minifies exactly as minify minifies it; one outside them is
 * refused with CU_MINIFY_LIMIT, never minified differently. The depth
 * bound counts the HTML minifier's re-entries into itself and the XML
 * minifier: iframe content, downlevel-hidden conditional comments (when
 * comments are not kept), XML and HTML script types, and data: URLs whose
 * media type is HTML or XML, each of which minify handles with a nested
 * call on the embedded bytes. Everything else is iterative. */
#define CU_MINIFY_MAX_INPUT (16UL << 20) /* bytes */
#define CU_MINIFY_MAX_DEPTH 64           /* nested minifier calls */

/* Minifies src (any bytes; invalid UTF-8 and NUL are handled as minify
 * handles them) as type and appends the result to out. opt may be NULL for
 * CU_MINIFY_SITE. scratch holds a mutable copy of the input, the token
 * look-ahead and attribute buffers under a temp scope that is ended before
 * returning; it must not be the builder's arena, since appending to out
 * under that scope would abort (cu.h, containers and scopes). Memory in
 * scratch is proportional to the input (a copy of it, a copy of each
 * nested region, and the tokens looked ahead); cap it with
 * cu_arena_set_limit for untrusted input, which ends as CU_MINIFY_NOMEM.
 * On a status other than CU_MINIFY_OK, out may hold a partial result and
 * should be discarded. */
CU_MUST_USE CuMinifyStatus cu_minify(CuArena *scratch, CuMinifyType type, const CuMinifyOptions *opt, CuStr src,
                                     CuBuilder *out);

/* The type Hugo's minifier set would minify mediatype as, matched exactly
 * as minify matches the mimetype part (parameters after ';' ignored,
 * case-sensitive): stores it and returns 1, or returns 0 for a media type
 * that has no ported minifier (text/css, javascript, json, image/svg+xml)
 * or none at all. */
CU_MUST_USE int cu_minify_type_for(CuStr mediatype, CuMinifyType *out);

#endif
