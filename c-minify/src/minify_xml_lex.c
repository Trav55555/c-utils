/* tdewolff/parse v2.7.3 xml/lex.go. See minify_xml_lex.h. */
#include "minify_xml_lex.h"

static int is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Lexer.at */
static int at(const MinXmlLexer *l, const char *s)
{
    size_t i;

    for (i = 0; s[i] != 0; i++) {
        if (min_peek(l->r, i) != (unsigned char)s[i]) return 0;
    }
    return 1;
}

void min_xml_lex_init(MinXmlLexer *l, MinInput *r)
{
    l->r = r;
    l->err = 0;
    l->in_tag = 0;
    l->text = min_bytes_nil();
    l->attr_val = min_bytes_nil();
}

/* Lexer.shiftDOCTYPEText: after "<!DOCTYPE". */
static MinBytes shift_doctype_text(MinXmlLexer *l)
{
    int in_string = 0;
    int in_brackets = 0;

    for (;;) {
        unsigned char c = min_peek(l->r, 0);
        MinBytes lex;

        if (c == '"') {
            in_string = !in_string;
        } else if ((c == '[' || c == ']') && !in_string) {
            in_brackets = c == '[';
        } else if (c == '>' && !in_string && !in_brackets) {
            lex = min_lexeme(l->r);
            l->text = min_slice(lex, 9, lex.len);
            min_move(l->r, 1);
            return min_shift(l->r);
        } else if (c == 0) {
            lex = min_lexeme(l->r);
            l->text = min_slice(lex, 9, lex.len);
            return min_shift(l->r);
        }
        min_move(l->r, 1);
    }
}

/* Lexer.shiftCDATAText: after "<![CDATA[". */
static MinBytes shift_cdata_text(MinXmlLexer *l)
{
    for (;;) {
        unsigned char c = min_peek(l->r, 0);
        MinBytes lex;

        if (c == ']' && min_peek(l->r, 1) == ']' && min_peek(l->r, 2) == '>') {
            lex = min_lexeme(l->r);
            l->text = min_slice(lex, 9, lex.len);
            min_move(l->r, 3);
            return min_shift(l->r);
        } else if (c == 0) {
            lex = min_lexeme(l->r);
            l->text = min_slice(lex, 9, lex.len);
            return min_shift(l->r);
        }
        min_move(l->r, 1);
    }
}

/* Lexer.shiftCommentText: after "<!--". */
static MinBytes shift_comment_text(MinXmlLexer *l)
{
    for (;;) {
        unsigned char c = min_peek(l->r, 0);

        if (c == '-' && min_peek(l->r, 1) == '-' && min_peek(l->r, 2) == '>') {
            MinBytes lex = min_lexeme(l->r);

            l->text = min_slice(lex, 4, lex.len);
            min_move(l->r, 3);
            return min_shift(l->r);
        } else if (c == 0) {
            return min_shift(l->r);
        }
        min_move(l->r, 1);
    }
}

/* Lexer.shiftStartTag: after "<" or "<?". */
static MinBytes shift_start_tag(MinXmlLexer *l)
{
    size_t name_start = min_pos(l->r);
    MinBytes lex;

    for (;;) {
        unsigned char c = min_peek(l->r, 0);

        if (c == ' ' || c == '>' || ((c == '/' || c == '?') && min_peek(l->r, 1) == '>') || c == '\t' || c == '\n' ||
            c == '\r' || c == 0) {
            break;
        }
        min_move(l->r, 1);
    }
    lex = min_lexeme(l->r);
    l->text = min_slice(lex, name_start, lex.len);
    return min_shift(l->r);
}

/* Lexer.shiftAttribute */
static MinBytes shift_attribute(MinXmlLexer *l)
{
    size_t name_start = min_pos(l->r);
    size_t name_end;
    unsigned char c;
    MinBytes lex;

    for (;;) { /* attribute name state */
        c = min_peek(l->r, 0);
        if (c == ' ' || c == '=' || c == '>' || ((c == '/' || c == '?') && min_peek(l->r, 1) == '>') || c == '\t' ||
            c == '\n' || c == '\r' || c == 0) {
            break;
        }
        min_move(l->r, 1);
    }
    name_end = min_pos(l->r);
    for (;;) { /* after attribute name state */
        c = min_peek(l->r, 0);
        if (is_space(c)) {
            min_move(l->r, 1);
            continue;
        }
        break;
    }
    if (c == '=') {
        size_t attr_pos;
        unsigned char delim;

        min_move(l->r, 1);
        for (;;) { /* before attribute value state */
            c = min_peek(l->r, 0);
            if (is_space(c)) {
                min_move(l->r, 1);
                continue;
            }
            break;
        }
        attr_pos = min_pos(l->r);
        delim = c;
        if (delim == '"' || delim == '\'') { /* attribute value single- and double-quoted state */
            min_move(l->r, 1);
            for (;;) {
                c = min_peek(l->r, 0);
                if (c == delim) {
                    min_move(l->r, 1);
                    break;
                } else if (c == 0) {
                    break;
                }
                min_move(l->r, 1);
                if (c == '\t' || c == '\n' || c == '\r') {
                    lex = min_lexeme(l->r);
                    lex.ptr[lex.len - 1] = ' ';
                }
            }
        } else { /* attribute value unquoted state */
            for (;;) {
                c = min_peek(l->r, 0);
                if (c == ' ' || c == '>' || ((c == '/' || c == '?') && min_peek(l->r, 1) == '>') || c == '\t' ||
                    c == '\n' || c == '\r' || c == 0) {
                    break;
                }
                min_move(l->r, 1);
            }
        }
        lex = min_lexeme(l->r);
        l->attr_val = min_slice(lex, attr_pos, lex.len);
    } else {
        min_rewind(l->r, name_end);
        l->attr_val = min_bytes_nil();
    }
    lex = min_lexeme(l->r);
    l->text = min_slice(lex, name_start, name_end);
    return min_shift(l->r);
}

/* Lexer.shiftEndTag: after "</". */
static MinBytes shift_end_tag(MinXmlLexer *l)
{
    size_t end;

    for (;;) {
        unsigned char c = min_peek(l->r, 0);
        MinBytes lex;

        if (c == '>') {
            lex = min_lexeme(l->r);
            l->text = min_slice(lex, 2, lex.len);
            min_move(l->r, 1);
            break;
        } else if (c == 0) {
            lex = min_lexeme(l->r);
            l->text = min_slice(lex, 2, lex.len);
            break;
        }
        min_move(l->r, 1);
    }
    end = l->text.len;
    while (end > 0) {
        unsigned char c = l->text.ptr[end - 1];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            end--;
            continue;
        }
        break;
    }
    l->text = min_slice(l->text, 0, end);
    return min_shift(l->r);
}

/* Lexer.Next */
MinXmlTokenType min_xml_lex_next(MinXmlLexer *l, MinBytes *data)
{
    unsigned char c;

    l->text = min_bytes_nil();
    if (l->in_tag) {
        l->attr_val = min_bytes_nil();
        for (;;) { /* before attribute name state */
            c = min_peek(l->r, 0);
            if (is_space(c)) {
                min_move(l->r, 1);
                continue;
            }
            break;
        }
        if (c == 0) {
            if (!min_eof(l->r)) l->err = 1; /* unexpected NULL character */
            *data = min_bytes_nil();
            return MX_ERROR_TOKEN;
        } else if (c != '>' && ((c != '/' && c != '?') || min_peek(l->r, 1) != '>')) {
            *data = shift_attribute(l);
            return MX_ATTRIBUTE_TOKEN;
        }
        min_skip(l->r);
        l->in_tag = 0;
        if (c == '/') {
            min_move(l->r, 2);
            *data = min_shift(l->r);
            return MX_START_TAG_CLOSE_VOID_TOKEN;
        } else if (c == '?') {
            min_move(l->r, 2);
            *data = min_shift(l->r);
            return MX_START_TAG_CLOSE_PI_TOKEN;
        }
        min_move(l->r, 1);
        *data = min_shift(l->r);
        return MX_START_TAG_CLOSE_TOKEN;
    }

    for (;;) {
        c = min_peek(l->r, 0);
        if (c == '<') {
            if (min_pos(l->r) > 0) {
                l->text = min_shift(l->r);
                *data = l->text;
                return MX_TEXT_TOKEN;
            }
            c = min_peek(l->r, 1);
            if (c == '/') {
                min_move(l->r, 2);
                *data = shift_end_tag(l);
                return MX_END_TAG_TOKEN;
            } else if (c == '!') {
                min_move(l->r, 2);
                if (at(l, "--")) {
                    min_move(l->r, 2);
                    *data = shift_comment_text(l);
                    return MX_COMMENT_TOKEN;
                } else if (at(l, "[CDATA[")) {
                    min_move(l->r, 7);
                    *data = shift_cdata_text(l);
                    return MX_CDATA_TOKEN;
                } else if (at(l, "DOCTYPE")) {
                    min_move(l->r, 7);
                    *data = shift_doctype_text(l);
                    return MX_DOCTYPE_TOKEN;
                }
                l->r->pos -= 2; /* l.r.Move(-2) */
            } else if (c == '?') {
                min_move(l->r, 2);
                l->in_tag = 1;
                *data = shift_start_tag(l);
                return MX_START_TAG_PI_TOKEN;
            }
            min_move(l->r, 1);
            l->in_tag = 1;
            *data = shift_start_tag(l);
            return MX_START_TAG_TOKEN;
        } else if (c == 0) {
            if (min_pos(l->r) > 0) {
                l->text = min_shift(l->r);
                *data = l->text;
                return MX_TEXT_TOKEN;
            }
            if (!min_eof(l->r)) l->err = 1; /* unexpected NULL character */
            *data = min_bytes_nil();
            return MX_ERROR_TOKEN;
        }
        min_move(l->r, 1);
    }
}
