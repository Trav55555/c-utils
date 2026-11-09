/* tdewolff/parse v2.7.3 html/lex.go. See minify_html_lex.h. */
#include "minify_html_lex.h"
#include "minify_html_table.h"

static int is_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* The NUL that ends the input: c == 0 && l.r.Err() != nil. */
static int at_end(const MinHtmlLexer *l, unsigned char c)
{
    return c == 0 && min_eof(l->r);
}

/* Lexer.at */
static int at(const MinHtmlLexer *l, const char *s)
{
    size_t i;

    for (i = 0; s[i] != 0; i++) {
        if (min_peek(l->r, i) != (unsigned char)s[i]) return 0;
    }
    return 1;
}

/* Lexer.atCaseInsensitive: s is lowercase. */
static int at_case_insensitive(const MinHtmlLexer *l, const char *s)
{
    size_t i;

    for (i = 0; s[i] != 0; i++) {
        unsigned char c = min_peek(l->r, i);

        if (c != (unsigned char)s[i] && (unsigned char)(c + ('a' - 'A')) != (unsigned char)s[i]) return 0;
    }
    return 1;
}

/* ToHash(ToLower(Copy(b))) without the copy: the hash of b lowercased. */
static unsigned long lower_hash(MinBytes b)
{
    unsigned char tmp[32];
    size_t i;

    if (b.len > sizeof tmp) return 0; /* longer than any hashed name */
    for (i = 0; i < b.len; i++) {
        unsigned char c = b.ptr[i];

        tmp[i] = (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
    }
    return minify_html_to_hash(tmp, b.len);
}

void min_html_lex_init(MinHtmlLexer *l, MinInput *r)
{
    l->r = r;
    l->err = 0;
    l->raw_tag = 0;
    l->in_tag = 0;
    l->text = min_bytes_nil();
    l->attr_val = min_bytes_nil();
}

/* Lexer.shiftRawText */
static MinBytes shift_raw_text(MinHtmlLexer *l)
{
    if (l->raw_tag == MH_PLAINTEXT) {
        for (;;) {
            if (at_end(l, min_peek(l->r, 0))) return min_shift(l->r);
            min_move(l->r, 1);
        }
    }
    /* RCDATA, RAWTEXT and SCRIPT */
    for (;;) {
        unsigned char c = min_peek(l->r, 0);

        if (c == '<') {
            if (min_peek(l->r, 1) == '/') {
                size_t mark = min_pos(l->r);
                MinBytes lex;

                min_move(l->r, 2);
                for (;;) {
                    if (!is_alpha(min_peek(l->r, 0))) break;
                    min_move(l->r, 1);
                }
                lex = min_lexeme(l->r);
                if (lower_hash(min_slice(lex, mark + 2, lex.len)) == l->raw_tag) {
                    min_rewind(l->r, mark);
                    return min_shift(l->r);
                }
            } else if (l->raw_tag == MH_SCRIPT && min_peek(l->r, 1) == '!' && min_peek(l->r, 2) == '-' &&
                       min_peek(l->r, 3) == '-') {
                int in_script = 0;

                min_move(l->r, 4);
                for (;;) {
                    c = min_peek(l->r, 0);
                    if (c == '-' && min_peek(l->r, 1) == '-' && min_peek(l->r, 2) == '>') {
                        min_move(l->r, 3);
                        break;
                    } else if (c == '<') {
                        int is_end = min_peek(l->r, 1) == '/';
                        size_t mark;
                        MinBytes lex;

                        min_move(l->r, is_end ? 2 : 1);
                        mark = min_pos(l->r);
                        for (;;) {
                            if (!is_alpha(min_peek(l->r, 0))) break;
                            min_move(l->r, 1);
                        }
                        lex = min_lexeme(l->r);
                        if (lower_hash(min_slice(lex, mark, lex.len)) == MH_SCRIPT) {
                            if (!is_end) {
                                in_script = 1;
                            } else {
                                if (!in_script) {
                                    min_rewind(l->r, mark - 2);
                                    return min_shift(l->r);
                                }
                                in_script = 0;
                            }
                        }
                    } else if (at_end(l, c)) {
                        return min_shift(l->r);
                    } else {
                        min_move(l->r, 1);
                    }
                }
            } else {
                min_move(l->r, 1);
            }
        } else if (at_end(l, c)) {
            return min_shift(l->r);
        } else {
            min_move(l->r, 1);
        }
    }
}

/* Lexer.shiftBogusComment */
static MinBytes shift_bogus_comment(MinHtmlLexer *l)
{
    for (;;) {
        unsigned char c = min_peek(l->r, 0);

        if (c == '>') {
            MinBytes lex = min_lexeme(l->r);

            l->text = min_slice(lex, 2, lex.len);
            min_move(l->r, 1);
            return min_shift(l->r);
        } else if (at_end(l, c)) {
            MinBytes lex = min_lexeme(l->r);

            l->text = min_slice(lex, 2, lex.len);
            return min_shift(l->r);
        }
        min_move(l->r, 1);
    }
}

/* Lexer.readMarkup: after "<!". */
static MinHtmlTokenType read_markup(MinHtmlLexer *l, MinBytes *data)
{
    if (at(l, "--")) {
        min_move(l->r, 2);
        for (;;) {
            MinBytes lex;

            if (at_end(l, min_peek(l->r, 0))) {
                lex = min_lexeme(l->r);
                l->text = min_slice(lex, 4, lex.len);
                *data = min_shift(l->r);
                return MH_COMMENT_TOKEN;
            } else if (at(l, "-->")) {
                lex = min_lexeme(l->r);
                l->text = min_slice(lex, 4, lex.len);
                min_move(l->r, 3);
                *data = min_shift(l->r);
                return MH_COMMENT_TOKEN;
            } else if (at(l, "--!>")) {
                lex = min_lexeme(l->r);
                l->text = min_slice(lex, 4, lex.len);
                min_move(l->r, 4);
                *data = min_shift(l->r);
                return MH_COMMENT_TOKEN;
            }
            min_move(l->r, 1);
        }
    } else if (at(l, "[CDATA[")) {
        min_move(l->r, 7);
        for (;;) {
            MinBytes lex;

            if (at_end(l, min_peek(l->r, 0))) {
                lex = min_lexeme(l->r);
                l->text = min_slice(lex, 9, lex.len);
                *data = min_shift(l->r);
                return MH_TEXT_TOKEN;
            } else if (at(l, "]]>")) {
                lex = min_lexeme(l->r);
                l->text = min_slice(lex, 9, lex.len);
                min_move(l->r, 3);
                *data = min_shift(l->r);
                return MH_TEXT_TOKEN;
            }
            min_move(l->r, 1);
        }
    } else if (at_case_insensitive(l, "doctype")) {
        min_move(l->r, 7);
        if (min_peek(l->r, 0) == ' ') min_move(l->r, 1);
        for (;;) {
            unsigned char c = min_peek(l->r, 0);

            if (c == '>' || at_end(l, c)) {
                MinBytes lex = min_lexeme(l->r);

                l->text = min_slice(lex, 9, lex.len);
                if (c == '>') min_move(l->r, 1);
                *data = min_shift(l->r);
                return MH_DOCTYPE_TOKEN;
            }
            min_move(l->r, 1);
        }
    }
    *data = shift_bogus_comment(l);
    return MH_COMMENT_TOKEN;
}

/* Lexer.shiftXML: the content of an svg or math element, the end tag
 * included. A NUL before the end of the input is an error. */
static MinBytes shift_xml(MinHtmlLexer *l, unsigned long raw_tag)
{
    int in_quote = 0;

    for (;;) {
        unsigned char c = min_peek(l->r, 0);

        if (c == '"') {
            in_quote = !in_quote;
            min_move(l->r, 1);
        } else if (c == '<' && !in_quote && min_peek(l->r, 1) == '/') {
            size_t mark = min_pos(l->r);
            MinBytes lex;

            min_move(l->r, 2);
            for (;;) {
                if (!is_alpha(min_peek(l->r, 0))) break;
                min_move(l->r, 1);
            }
            lex = min_lexeme(l->r);
            if (lower_hash(min_slice(lex, mark + 2, lex.len)) == raw_tag) break;
        } else if (c == 0) {
            if (!min_eof(l->r)) l->err = 1; /* unexpected NULL character */
            return min_shift(l->r);
        } else {
            min_move(l->r, 1);
        }
    }
    for (;;) {
        unsigned char c = min_peek(l->r, 0);

        if (c == '>') {
            min_move(l->r, 1);
            break;
        } else if (c == 0) {
            if (!min_eof(l->r)) l->err = 1; /* unexpected NULL character */
            return min_shift(l->r);
        }
        min_move(l->r, 1);
    }
    return min_shift(l->r);
}

/* Lexer.shiftStartTag: after "<" and the first letter. */
static MinHtmlTokenType shift_start_tag(MinHtmlLexer *l, MinBytes *data)
{
    MinBytes lex;
    unsigned long h;

    for (;;) {
        unsigned char c = min_peek(l->r, 0);

        if (c == ' ' || c == '>' || (c == '/' && min_peek(l->r, 1) == '>') || c == '\t' || c == '\n' || c == '\r' ||
            c == '\f' || at_end(l, c)) {
            break;
        }
        min_move(l->r, 1);
    }
    lex = min_lexeme(l->r);
    l->text = min_slice(lex, 1, lex.len);
    min_to_lower(l->text);
    h = minify_html_to_hash(l->text.ptr, l->text.len);
    if (h == MH_TEXTAREA || h == MH_TITLE || h == MH_STYLE || h == MH_XMP || h == MH_IFRAME || h == MH_SCRIPT ||
        h == MH_PLAINTEXT || h == MH_SVG || h == MH_MATH) {
        if (h == MH_SVG || h == MH_MATH) {
            *data = shift_xml(l, h);
            if (l->err) {
                *data = min_bytes_nil();
                return MH_ERROR_TOKEN;
            }
            l->in_tag = 0;
            return h == MH_SVG ? MH_SVG_TOKEN : MH_MATH_TOKEN;
        }
        l->raw_tag = h;
    }
    *data = min_shift(l->r);
    return MH_START_TAG_TOKEN;
}

/* Lexer.shiftAttribute */
static MinBytes shift_attribute(MinHtmlLexer *l)
{
    size_t name_start = min_pos(l->r);
    size_t name_end;
    unsigned char c;
    MinBytes lex;

    for (;;) { /* attribute name state */
        c = min_peek(l->r, 0);
        if (c == ' ' || c == '=' || c == '>' || (c == '/' && min_peek(l->r, 1) == '>') || c == '\t' || c == '\n' ||
            c == '\r' || c == '\f' || at_end(l, c)) {
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
                } else if (at_end(l, c)) {
                    break;
                } else {
                    min_move(l->r, 1);
                }
            }
        } else { /* attribute value unquoted state */
            for (;;) {
                c = min_peek(l->r, 0);
                if (c == ' ' || c == '>' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || at_end(l, c)) break;
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
    min_to_lower(l->text);
    return min_shift(l->r);
}

/* Lexer.shiftEndTag: after "</". */
static MinBytes shift_end_tag(MinHtmlLexer *l)
{
    size_t end;
    MinBytes data;

    for (;;) {
        unsigned char c = min_peek(l->r, 0);
        MinBytes lex;

        if (c == '>') {
            lex = min_lexeme(l->r);
            l->text = min_slice(lex, 2, lex.len);
            min_move(l->r, 1);
            break;
        } else if (at_end(l, c)) {
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
    data = min_shift(l->r);
    min_to_lower(data);
    return data;
}

/* Lexer.Next */
MinHtmlTokenType min_html_lex_next(MinHtmlLexer *l, MinBytes *data)
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
        if (at_end(l, c)) {
            *data = min_bytes_nil();
            return MH_ERROR_TOKEN;
        } else if (c != '>' && (c != '/' || min_peek(l->r, 1) != '>')) {
            *data = shift_attribute(l);
            return MH_ATTRIBUTE_TOKEN;
        }
        min_skip(l->r);
        l->in_tag = 0;
        if (c == '/') {
            min_move(l->r, 2);
            *data = min_shift(l->r);
            return MH_START_TAG_VOID_TOKEN;
        }
        min_move(l->r, 1);
        *data = min_shift(l->r);
        return MH_START_TAG_CLOSE_TOKEN;
    }

    if (l->raw_tag != 0) {
        MinBytes raw = shift_raw_text(l);

        if (raw.len > 0) {
            l->text = raw;
            l->raw_tag = 0;
            *data = raw;
            return MH_TEXT_TOKEN;
        }
        l->raw_tag = 0;
    }

    for (;;) {
        c = min_peek(l->r, 0);
        if (c == '<') {
            int is_end_tag;

            c = min_peek(l->r, 1);
            is_end_tag = c == '/' && min_peek(l->r, 2) != '>' && (min_peek(l->r, 2) != 0 || !min_peek_eof(l->r, 2));
            if (!is_end_tag && !is_alpha(c) && c != '!' && c != '?') {
                /* not a tag */
            } else if (min_pos(l->r) > 0) {
                /* return currently buffered texttoken so that we can return tag next iteration */
                l->text = min_shift(l->r);
                *data = l->text;
                return MH_TEXT_TOKEN;
            } else if (is_end_tag) {
                min_move(l->r, 2);
                /* only endtags that are not followed by > or EOF arrive here */
                if (!is_alpha(min_peek(l->r, 0))) {
                    *data = shift_bogus_comment(l);
                    return MH_COMMENT_TOKEN;
                }
                *data = shift_end_tag(l);
                return MH_END_TAG_TOKEN;
            } else if (is_alpha(c)) {
                min_move(l->r, 1);
                l->in_tag = 1;
                return shift_start_tag(l, data);
            } else if (c == '!') {
                min_move(l->r, 2);
                return read_markup(l, data);
            } else if (c == '?') {
                min_move(l->r, 1);
                *data = shift_bogus_comment(l);
                return MH_COMMENT_TOKEN;
            }
        } else if (at_end(l, c)) {
            if (min_pos(l->r) > 0) {
                l->text = min_shift(l->r);
                *data = l->text;
                return MH_TEXT_TOKEN;
            }
            *data = min_bytes_nil();
            return MH_ERROR_TOKEN;
        }
        min_move(l->r, 1);
    }
}
