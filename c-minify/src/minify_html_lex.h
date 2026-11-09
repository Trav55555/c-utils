#ifndef MINIFY_HTML_LEX_H
#define MINIFY_HTML_LEX_H
#include "minify_parse.h"

/* tdewolff/parse v2.7.3 html/lex.go: the HTML5 lexer, without template
 * delimiters (Hugo passes none; TemplateDelims is not ported). The raw tag
 * is compared by minify/html's hash, which names the same strings as
 * parse/html's smaller one. */

typedef enum MinHtmlTokenType {
    MH_ERROR_TOKEN = 0,
    MH_COMMENT_TOKEN,
    MH_DOCTYPE_TOKEN,
    MH_START_TAG_TOKEN,
    MH_START_TAG_CLOSE_TOKEN,
    MH_START_TAG_VOID_TOKEN,
    MH_END_TAG_TOKEN,
    MH_ATTRIBUTE_TOKEN,
    MH_TEXT_TOKEN,
    MH_SVG_TOKEN,
    MH_MATH_TOKEN
} MinHtmlTokenType;

typedef struct MinHtmlLexer {
    MinInput *r;
    int err; /* a lexer error (shiftXML: "unexpected NULL character") */
    unsigned long raw_tag;
    int in_tag;
    MinBytes text;
    MinBytes attr_val;
} MinHtmlLexer;

void min_html_lex_init(MinHtmlLexer *l, MinInput *r);
/* Lexer.Next: the token type and its bytes (*data). Text() and AttrVal()
 * are the text and attr_val fields afterwards. */
MinHtmlTokenType min_html_lex_next(MinHtmlLexer *l, MinBytes *data);

#endif
