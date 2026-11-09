#ifndef MINIFY_XML_LEX_H
#define MINIFY_XML_LEX_H
#include "minify_parse.h"

/* tdewolff/parse v2.7.3 xml/lex.go: the XML 1.0 lexer. A NUL byte inside
 * the input ends the token it is in and then fails the next call with
 * "unexpected NULL character" (err set), as Go's does. */

typedef enum MinXmlTokenType {
    MX_ERROR_TOKEN = 0,
    MX_COMMENT_TOKEN,
    MX_DOCTYPE_TOKEN,
    MX_CDATA_TOKEN,
    MX_START_TAG_TOKEN,
    MX_START_TAG_PI_TOKEN,
    MX_START_TAG_CLOSE_TOKEN,
    MX_START_TAG_CLOSE_VOID_TOKEN,
    MX_START_TAG_CLOSE_PI_TOKEN,
    MX_END_TAG_TOKEN,
    MX_ATTRIBUTE_TOKEN,
    MX_TEXT_TOKEN
} MinXmlTokenType;

typedef struct MinXmlLexer {
    MinInput *r;
    int err;
    int in_tag;
    MinBytes text;
    MinBytes attr_val;
} MinXmlLexer;

void min_xml_lex_init(MinXmlLexer *l, MinInput *r);
/* Lexer.Next */
MinXmlTokenType min_xml_lex_next(MinXmlLexer *l, MinBytes *data);

#endif
