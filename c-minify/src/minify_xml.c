/* tdewolff/minify v2.20.5 xml/xml.go, xml/buffer.go, xml/table.go and
 * parse xml/util.go. See minify_parse.h. */
#include "minify_xml_lex.h"
#include <string.h>

/* ---- buffer.go --------------------------------------------------------- */

typedef struct MinXmlToken {
    MinXmlTokenType type;
    MinBytes data;
    MinBytes text;
    MinBytes attr_val;
} MinXmlToken;

/* No pointer to the context or the lexer is kept, as in html's buffer. */
typedef struct MinXmlTokens {
    MinXmlToken *buf;
    size_t len;
    size_t cap;
    size_t pos;
    MinXmlToken error_tok;
} MinXmlTokens;

static int tokens_init(MinXmlTokens *tb, MinCtx *ctx)
{
    tb->cap = 8;
    tb->buf = CU_PUSH(ctx->a, MinXmlToken, tb->cap);
    tb->len = 0;
    tb->pos = 0;
    tb->error_tok.type = MX_ERROR_TOKEN;
    tb->error_tok.data = min_bytes_nil();
    tb->error_tok.text = min_bytes_nil();
    tb->error_tok.attr_val = min_bytes_nil();
    if (tb->buf == NULL) {
        ctx->failed = 1;
        return 0;
    }
    return 1;
}

/* TokenBuffer.read */
static void tokens_read(MinXmlLexer *l, MinXmlToken *t)
{
    t->type = min_xml_lex_next(l, &t->data);
    t->text = l->text;
    t->attr_val = t->type == MX_ATTRIBUTE_TOKEN ? l->attr_val : min_bytes_nil();
}

/* TokenBuffer.Peek, as html/buffer.go's. */
static MinXmlToken *tokens_peek(MinXmlTokens *tb, MinCtx *ctx, MinXmlLexer *l, size_t pos)
{
    if (ctx->failed) return &tb->error_tok;
    pos += tb->pos;
    if (pos >= tb->len) {
        size_t d = tb->len - tb->pos;
        size_t p = pos - tb->pos + 1;
        size_t i;

        if (tb->len > 0 && tb->buf[tb->len - 1].type == MX_ERROR_TOKEN) return &tb->buf[tb->len - 1];
        if (2 * p > tb->cap) {
            size_t ncap = 2 * tb->cap + p;
            MinXmlToken *fresh = CU_PUSH(ctx->a, MinXmlToken, ncap);

            if (fresh == NULL) {
                ctx->failed = 1;
                return &tb->error_tok;
            }
            if (d > 0) memcpy(fresh, tb->buf + tb->pos, d * sizeof *fresh);
            tb->buf = fresh;
            tb->cap = ncap;
        } else if (d > 0) {
            memmove(tb->buf, tb->buf + tb->pos, d * sizeof *tb->buf);
        }
        tb->len = p;
        pos -= tb->pos;
        tb->pos = 0;
        for (i = d; i < p; i++) {
            tokens_read(l, &tb->buf[i]);
            if (tb->buf[i].type == MX_ERROR_TOKEN) {
                tb->len = i + 1;
                pos = i;
                break;
            }
        }
    }
    return &tb->buf[pos];
}

/* TokenBuffer.Shift */
static MinXmlToken *tokens_shift(MinXmlTokens *tb, MinCtx *ctx, MinXmlLexer *l)
{
    MinXmlToken *t;

    if (ctx->failed) return &tb->error_tok;
    if (tb->pos >= tb->len) {
        t = &tb->buf[0];
        tokens_read(l, t);
        return t;
    }
    t = &tb->buf[tb->pos];
    tb->pos++;
    return t;
}

/* ---- table.go and util.go ---------------------------------------------- */

/* table.go EntitiesMap */
static int xml_entity_lookup(const unsigned char *name, size_t n, const unsigned char **value, size_t *value_len)
{
    static const unsigned char apos = '\'';
    static const unsigned char gt = '>';
    static const unsigned char quot = '"';

    *value_len = 1;
    if (n == 4 && memcmp(name, "apos", 4) == 0) {
        *value = &apos;
        return 1;
    }
    if (n == 2 && memcmp(name, "gt", 2) == 0) {
        *value = &gt;
        return 1;
    }
    if (n == 4 && memcmp(name, "quot", 4) == 0) {
        *value = &quot;
        return 1;
    }
    return 0;
}

/* table.go TextRevEntitiesMap: '<' and '&'. */
static const char *const xml_rev[256] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   /* 0x00 */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   /* 0x10 */
    NULL, NULL, NULL, NULL, NULL, NULL, "&amp;", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 0x20 */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "&lt;", NULL, NULL, NULL, /* 0x30 */
};

static const MinEntityMaps xml_text_maps = {xml_entity_lookup, xml_rev};
static const MinEntityMaps xml_attr_maps = {xml_entity_lookup, NULL};

static void w(CuBuilder *out, MinBytes b)
{
    if (b.len > 0) cu_sb_str(out, min_as_str(b));
}

/* util.go EscapeAttrVal: the value in the quotes that occur less often,
 * the others escaped, in ctx's attribute buffer. */
static MinBytes escape_attr_val(MinCtx *ctx, MinBytes b)
{
    size_t singles = 0;
    size_t doubles = 0;
    size_t i;
    size_t n;
    unsigned char quote;
    const char *escaped;
    unsigned char *t;
    size_t j;
    size_t start;

    for (i = 0; i < b.len; i++) {
        if (b.ptr[i] == '"') doubles++;
        else if (b.ptr[i] == '\'') singles++;
    }
    n = b.len + 2;
    if (doubles > singles) {
        n += singles * 4;
        quote = '\'';
        escaped = "&#39;";
    } else {
        n += doubles * 4;
        quote = '"';
        escaped = "&#34;";
    }
    t = min_buf_reserve(&ctx->attr_buf, ctx->a, n, &ctx->failed);
    if (t == NULL) return b;
    t[0] = quote;
    j = 1;
    start = 0;
    for (i = 0; i < b.len; i++) {
        if (b.ptr[i] == quote) {
            memcpy(t + j, b.ptr + start, i - start);
            j += i - start;
            memcpy(t + j, escaped, 5);
            j += 5;
            start = i + 1;
        }
    }
    memcpy(t + j, b.ptr + start, b.len - start);
    j += b.len - start;
    t[j] = quote;
    return min_bytes(t, j + 1);
}

/* util.go EscapeCDATAVal: the CDATA text with < and & escaped when that
 * is no longer than the CDATA wrapper; returns 0 to keep the CDATA. */
static int escape_cdata_val(MinCtx *ctx, MinBytes b, MinBytes *out)
{
    size_t n = 0;
    size_t i;
    unsigned char *t;
    size_t j;
    size_t start;

    for (i = 0; i < b.len; i++) {
        unsigned char c = b.ptr[i];

        if (c == '<' || c == '&') {
            n += c == '<' ? 3 : 4;
            if (n > 12) return 0; /* len("<![CDATA[]]>") */
        }
    }
    t = min_buf_reserve(&ctx->attr_buf, ctx->a, b.len + n, &ctx->failed);
    if (t == NULL) return 0;
    j = 0;
    start = 0;
    for (i = 0; i < b.len; i++) {
        unsigned char c = b.ptr[i];

        if (c == '<' || c == '&') {
            memcpy(t + j, b.ptr + start, i - start);
            j += i - start;
            if (c == '<') {
                memcpy(t + j, "&lt;", 4);
                j += 4;
            } else {
                memcpy(t + j, "&amp;", 5);
                j += 5;
            }
            start = i + 1;
        }
    }
    memcpy(t + j, b.ptr + start, b.len - start);
    j += b.len - start;
    *out = min_bytes(t, j);
    return 1;
}

/* ---- xml.go ------------------------------------------------------------ */

/* Minifier.Minify */
int minify_xml(MinCtx *ctx, CuStr src, CuBuilder *out)
{
    MinBytes buf;
    MinInput *input = CU_PUSH(ctx->a, MinInput, 1);
    MinXmlLexer *l = CU_PUSH(ctx->a, MinXmlLexer, 1);
    MinXmlTokens *tb = CU_PUSH(ctx->a, MinXmlTokens, 1);
    int keep_whitespace = (ctx->flags & CU_MINIFY_KEEP_WHITESPACE) != 0;
    int omit_space = 1; /* on true the next text token must not start with a space */

    if (input == NULL || l == NULL || tb == NULL || !min_copy_input(ctx->a, src, &buf, &ctx->failed)) {
        ctx->failed = 1;
        return 0;
    }
    min_input_init(input, buf.ptr, buf.len);
    min_xml_lex_init(l, input);
    if (!tokens_init(tb, ctx)) return 0;
    for (;;) {
        MinXmlToken t = *tokens_shift(tb, ctx, l);

        if (ctx->failed) return 0;
        if (t.type == MX_CDATA_TOKEN) {
            MinBytes text;

            if (t.text.len == 0) continue;
            if (escape_cdata_val(ctx, t.text, &text)) {
                t.type = MX_TEXT_TOKEN;
                t.data = text;
            }
            if (ctx->failed) return 0;
        }
        switch (t.type) {
        case MX_ERROR_TOKEN:
            if (l->err) {
                ctx->invalid = 1;
                return 0;
            }
            return !out->failed;
        case MX_DOCTYPE_TOKEN:
            w(out, t.data);
            break;
        case MX_CDATA_TOKEN:
            w(out, t.data);
            if (t.text.len > 0 && min_is_whitespace(t.text.ptr[t.text.len - 1])) omit_space = 1;
            break;
        case MX_TEXT_TOKEN:
            t.data = min_replace_multiple_whitespace_and_entities(t.data, &xml_text_maps);

            /* whitespace removal; trim left */
            if (omit_space && t.data.len > 0 && min_is_whitespace(t.data.ptr[0])) t.data = min_slice(t.data, 1, t.data.len);

            /* whitespace removal; trim right */
            omit_space = 0;
            if (t.data.len == 0) {
                omit_space = 1;
            } else if (min_is_whitespace(t.data.ptr[t.data.len - 1])) {
                size_t i = 0;

                omit_space = 1;
                for (;;) {
                    const MinXmlToken *next = tokens_peek(tb, ctx, l, i);

                    /* trim if EOF, text token with whitespace begin or block token */
                    if (next->type == MX_ERROR_TOKEN) {
                        t.data.len--;
                        omit_space = 0;
                        break;
                    } else if (next->type == MX_TEXT_TOKEN) {
                        /* this only happens when a comment, doctype, cdata startpi tag was in between
                         * remove if the text token starts with a whitespace */
                        if (next->data.len > 0 && min_is_whitespace(next->data.ptr[0])) {
                            t.data.len--;
                            omit_space = 0;
                        }
                        break;
                    } else if (next->type == MX_CDATA_TOKEN) {
                        if (next->text.len > 0 && min_is_whitespace(next->text.ptr[0])) {
                            t.data.len--;
                            omit_space = 0;
                        }
                        break;
                    } else if (next->type == MX_START_TAG_TOKEN || next->type == MX_END_TAG_TOKEN) {
                        if (!keep_whitespace) {
                            t.data.len--;
                            omit_space = 0;
                        }
                        break;
                    }
                    i++;
                }
            }
            w(out, t.data);
            break;
        case MX_START_TAG_TOKEN:
            if (keep_whitespace) omit_space = 0;
            w(out, t.data);
            break;
        case MX_START_TAG_PI_TOKEN:
            w(out, t.data);
            break;
        case MX_ATTRIBUTE_TOKEN:
            cu_sb_str(out, cu_str_cstr(" "));
            w(out, t.text);
            cu_sb_str(out, cu_str_cstr("="));
            if (t.attr_val.len < 2 || t.attr_val.ptr[0] != '"' || t.attr_val.ptr[t.attr_val.len - 1] != '"') {
                w(out, t.attr_val);
            } else {
                MinBytes val = min_slice(t.attr_val, 1, t.attr_val.len - 1);

                val = min_replace_entities(val, &xml_attr_maps);
                /* prefer single or double quotes depending on what occurs more often in value */
                val = escape_attr_val(ctx, val);
                if (ctx->failed) return 0;
                w(out, val);
            }
            break;
        case MX_START_TAG_CLOSE_TOKEN: {
            const MinXmlToken *next = tokens_peek(tb, ctx, l, 0);
            int skip_extra = 0;

            if (next->type == MX_TEXT_TOKEN && min_is_all_whitespace(next->data)) {
                next = tokens_peek(tb, ctx, l, 1);
                skip_extra = 1;
            }
            if (next->type == MX_END_TAG_TOKEN) {
                /* collapse empty tags to single void tag */
                tokens_shift(tb, ctx, l);
                if (skip_extra) tokens_shift(tb, ctx, l);
                cu_sb_str(out, cu_str_cstr("/>"));
            } else {
                w(out, t.data);
            }
            break;
        }
        case MX_START_TAG_CLOSE_VOID_TOKEN:
        case MX_START_TAG_CLOSE_PI_TOKEN:
            w(out, t.data);
            break;
        case MX_END_TAG_TOKEN:
            if (keep_whitespace) omit_space = 0;
            if (t.data.len > 3 + t.text.len) {
                t.data.ptr[2 + t.text.len] = '>';
                t.data.len = 3 + t.text.len;
            }
            w(out, t.data);
            break;
        default:
            /* CommentToken: Go's switch has no case for it; nothing is written. */
            break;
        }
    }
}
