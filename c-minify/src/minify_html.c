/* tdewolff/minify v2.20.5 html/html.go and html/buffer.go, with the parts
 * of minify.go (MinifyMimetype, Bytes) and common.go (DataURI) that the
 * HTML minifier reaches under Hugo's minifier set. See minify_parse.h. */
#include "minify_html_lex.h"
#include "minify_html_table.h"
#include <string.h>

/* ---- buffer.go --------------------------------------------------------- */

/* buffer.go Token */
typedef struct MinHtmlToken {
    MinHtmlTokenType type;
    unsigned long hash;
    MinBytes data;
    MinBytes text; /* ptr NULL: Go's nil, a removed attribute */
    MinBytes attr_val;
    unsigned traits;
} MinHtmlToken;

/* buffer.go TokenBuffer: tokens read ahead of the one being minified. */
typedef struct MinHtmlTokens {
    MinCtx *ctx;
    MinHtmlLexer *l;
    MinHtmlToken *buf;
    size_t len;
    size_t cap;
    size_t pos;
    MinHtmlToken error_tok; /* what Peek and Shift hand out after an allocation failed */
} MinHtmlTokens;

static void token_clear(MinHtmlToken *t)
{
    t->type = MH_ERROR_TOKEN;
    t->hash = 0;
    t->data = min_bytes_nil();
    t->text = min_bytes_nil();
    t->attr_val = min_bytes_nil();
    t->traits = 0;
}

static int tokens_init(MinHtmlTokens *tb, MinCtx *ctx, MinHtmlLexer *l)
{
    tb->ctx = ctx;
    tb->l = l;
    tb->cap = 8;
    tb->buf = CU_PUSH(ctx->a, MinHtmlToken, tb->cap);
    tb->len = 0;
    tb->pos = 0;
    token_clear(&tb->error_tok);
    if (tb->buf == NULL) {
        ctx->failed = 1;
        return 0;
    }
    return 1;
}

/* TokenBuffer.read */
static void tokens_read(MinHtmlTokens *tb, MinHtmlToken *t)
{
    t->type = min_html_lex_next(tb->l, &t->data);
    t->text = tb->l->text;
    if (t->type == MH_ATTRIBUTE_TOKEN) {
        t->attr_val = tb->l->attr_val;
        if (t->attr_val.len > 1 && (t->attr_val.ptr[0] == '"' || t->attr_val.ptr[0] == '\'')) {
            /* quotes will be readded in attribute loop if necessary */
            t->attr_val = min_slice(t->attr_val, 1, t->attr_val.len - 1);
        }
        t->hash = minify_html_to_hash(t->text.ptr, t->text.len);
        t->traits = minify_html_attr_traits(t->hash);
    } else if (t->type == MH_START_TAG_TOKEN || t->type == MH_END_TAG_TOKEN) {
        t->attr_val = min_bytes_nil();
        t->hash = minify_html_to_hash(t->text.ptr, t->text.len);
        t->traits = minify_html_tag_traits(t->hash); /* zero if not exist */
    } else {
        t->attr_val = min_bytes_nil();
        t->hash = 0;
        t->traits = 0;
    }
}

/* TokenBuffer.Peek: the token pos ahead, reading and keeping the ones
 * before it. Past an error token it returns that token. */
static MinHtmlToken *tokens_peek(MinHtmlTokens *tb, size_t pos)
{
    if (tb->ctx->failed) return &tb->error_tok;
    pos += tb->pos;
    if (pos >= tb->len) {
        size_t c = tb->cap;
        size_t d = tb->len - tb->pos;
        size_t p = pos - tb->pos + 1; /* required peek length */
        MinHtmlToken *buf;
        size_t i;

        if (tb->len > 0 && tb->buf[tb->len - 1].type == MH_ERROR_TOKEN) return &tb->buf[tb->len - 1];
        if (2 * p > c) {
            size_t ncap = 2 * c + p;

            buf = CU_PUSH(tb->ctx->a, MinHtmlToken, ncap);
            if (buf == NULL) {
                tb->ctx->failed = 1;
                return &tb->error_tok;
            }
            if (d > 0) memcpy(buf, tb->buf + tb->pos, d * sizeof *buf);
            tb->cap = ncap;
        } else {
            buf = tb->buf;
            if (d > 0) memmove(buf, tb->buf + tb->pos, d * sizeof *buf);
        }
        tb->len = p;
        pos -= tb->pos;
        for (i = d; i < p; i++) {
            tokens_read(tb, &buf[i]);
            if (buf[i].type == MH_ERROR_TOKEN) {
                tb->len = i + 1;
                pos = i;
                break;
            }
        }
        tb->pos = 0;
        tb->buf = buf;
    }
    return &tb->buf[pos];
}

/* TokenBuffer.Shift */
static MinHtmlToken *tokens_shift(MinHtmlTokens *tb)
{
    MinHtmlToken *t;

    if (tb->ctx->failed) return &tb->error_tok;
    if (tb->pos >= tb->len) {
        t = &tb->buf[0];
        tokens_read(tb, t);
        return t;
    }
    t = &tb->buf[tb->pos];
    tb->pos++;
    return t;
}

/* TokenBuffer.Attributes: for each hash, the last attribute token of the
 * current tag with that hash, or NULL. */
static void tokens_attributes(MinHtmlTokens *tb, const unsigned long *hashes, size_t nhashes, MinHtmlToken **out)
{
    size_t n = 0;
    size_t i;
    size_t j;

    for (;;) {
        if (tokens_peek(tb, n)->type != MH_ATTRIBUTE_TOKEN) break;
        n++;
    }
    for (j = 0; j < nhashes; j++) out[j] = NULL;
    for (i = tb->pos; i < tb->pos + n; i++) {
        MinHtmlToken *attr = &tb->buf[i];

        for (j = 0; j < nhashes; j++) {
            if (hashes[j] == attr->hash) out[j] = attr;
        }
    }
}

/* ---- the minifier set (minify.go) -------------------------------------- */

typedef enum MinSlot { SLOT_NONE = 0, SLOT_CSS, SLOT_JS, SLOT_JSON, SLOT_SVG, SLOT_XML, SLOT_HTML } MinSlot;

/* Hugo's minifiers.New: the literal mimetypes, then the two patterns
 * ^(application|text)/(x-)?(java|ecma)script$ and
 * ^(application|text)/(x-|(ld|manifest)\+)?json$. Case-sensitive, as
 * M.MinifyMimetype's map lookup and regexps are. */
static MinSlot slot_of(MinBytes mime)
{
    MinBytes rest;

    if (min_bytes_eq(mime, "text/css")) return SLOT_CSS;
    if (min_bytes_eq(mime, "text/javascript")) return SLOT_JS;
    if (min_bytes_eq(mime, "application/json")) return SLOT_JSON;
    if (min_bytes_eq(mime, "image/svg+xml")) return SLOT_SVG;
    if (min_bytes_eq(mime, "application/xml") || min_bytes_eq(mime, "application/rss+xml") ||
        min_bytes_eq(mime, "text/xml")) {
        return SLOT_XML;
    }
    if (min_bytes_eq(mime, "text/html")) return SLOT_HTML;
    if (min_has_prefix(mime, "application/")) rest = min_slice(mime, 12, mime.len);
    else if (min_has_prefix(mime, "text/")) rest = min_slice(mime, 5, mime.len);
    else return SLOT_NONE;
    if (min_has_prefix(rest, "x-")) {
        MinBytes r2 = min_slice(rest, 2, rest.len);

        if (min_bytes_eq(r2, "javascript") || min_bytes_eq(r2, "ecmascript")) return SLOT_JS;
        if (min_bytes_eq(r2, "json")) return SLOT_JSON;
        return SLOT_NONE;
    }
    if (min_bytes_eq(rest, "javascript") || min_bytes_eq(rest, "ecmascript")) return SLOT_JS;
    if (min_bytes_eq(rest, "json") || min_bytes_eq(rest, "ld+json") || min_bytes_eq(rest, "manifest+json")) {
        return SLOT_JSON;
    }
    return SLOT_NONE;
}

int cu_minify_type_for(CuStr mediatype, CuMinifyType *out)
{
    MinBytes b = min_bytes((unsigned char *)mediatype.ptr, mediatype.len); /* read only below */

    switch (slot_of(min_mediatype_mime(b))) {
    case SLOT_HTML:
        *out = CU_MINIFY_HTML;
        return 1;
    case SLOT_XML:
        *out = CU_MINIFY_XML;
        return 1;
    default:
        return 0;
    }
}

/* Copies b into a NUL-terminated buffer, as a nested parse.NewInput does
 * for the embedded bytes. */
static int copy_input(MinCtx *ctx, MinBytes b, MinBytes *out)
{
    unsigned char *p = (unsigned char *)cu_arena_push_bytes(ctx->a, b.len + 1);

    if (p == NULL) {
        ctx->failed = 1;
        return 0;
    }
    if (b.len > 0) memcpy(p, b.ptr, b.len);
    p[b.len] = 0;
    *out = min_bytes(p, b.len);
    return 1;
}

static int nested(MinCtx *ctx, MinSlot slot, MinBytes data, CuBuilder *out)
{
    MinBytes copy;
    int ok;

    if (ctx->depth >= CU_MINIFY_MAX_DEPTH) {
        ctx->limit = 1;
        return 0;
    }
    if (!copy_input(ctx, data, &copy)) return 0;
    ctx->depth++;
    ok = slot == SLOT_HTML ? minify_html(ctx, copy, out) : minify_xml(ctx, copy, out);
    ctx->depth--;
    return ok;
}

int minify_mimetype(MinCtx *ctx, MinBytes mimetype, MinBytes data, CuBuilder *out)
{
    switch (slot_of(mimetype)) {
    case SLOT_NONE:
        return 0;
    case SLOT_HTML:
    case SLOT_XML:
        nested(ctx, slot_of(mimetype), data, out);
        return 1;
    case SLOT_CSS:
    case SLOT_JS:
        /* Not ported, but both write nothing for empty or all-whitespace
         * input (checked against the oracle); anything else is refused. */
        if (!min_is_all_whitespace(data)) ctx->unsupported = 1;
        return 1;
    default:
        ctx->unsupported = 1;
        return 1;
    }
}

int minify_bytes(MinCtx *ctx, MinBytes mediatype, MinBytes data, MinBytes *out)
{
    CuBuilder b;
    CuStr s;
    MinSlot slot = slot_of(min_mediatype_mime(mediatype));
    int was_invalid = ctx->invalid;
    int ok;

    if (slot == SLOT_NONE) return 0;
    cu_sb_init(&b, ctx->a);
    ok = minify_mimetype(ctx, min_mediatype_mime(mediatype), data, &b);
    /* Go ignores the error here (data, _ = m.Bytes(...)) and keeps the
     * data: a lexer error in the embedded bytes is not one for the page. */
    if (ctx->invalid && !was_invalid) {
        ctx->invalid = 0;
        return 0;
    }
    if (!ok || ctx->failed || ctx->unsupported || ctx->limit || !cu_sb_finish(&b, &s)) {
        if (b.failed) ctx->failed = 1;
        return 0;
    }
    *out = min_bytes((unsigned char *)s.ptr, s.len);
    return 1;
}

/* ---- common.go DataURI ------------------------------------------------- */

/* minify.DataURI: the data URI with its data minified by its media type
 * and re-encoded the shorter way. Returns uri itself when parse.DataURI
 * fails. */
static MinBytes data_uri(MinCtx *ctx, MinBytes uri)
{
    MinBytes orig;
    MinBytes mediatype;
    MinBytes data;
    MinBytes minified;
    size_t base64_len;
    size_t ascii_len;
    size_t i;
    unsigned char *result;
    size_t n;

    if (!copy_input(ctx, uri, &orig)) return uri;
    if (!min_data_uri(ctx->a, uri, &mediatype, &data, &ctx->failed)) return uri;
    if (ctx->failed) return uri;
    if (minify_bytes(ctx, mediatype, data, &minified)) data = minified;
    if (ctx->failed || ctx->unsupported || ctx->limit || ctx->invalid) return uri;
    base64_len = 7 + (data.len + 2) / 3 * 4;
    ascii_len = data.len;
    for (i = 0; i < data.len; i++) {
        unsigned char c = data.ptr[i];

        if (c <= ' ' || c >= 0x7f || strchr("\"#%&<>[\\]^`{|}", c) != NULL) ascii_len += 2;
        if (ascii_len > base64_len) break;
    }
    if (orig.len < base64_len && orig.len < ascii_len) return orig;
    if (base64_len < ascii_len) {
        data = min_base64_encode(ctx->a, data, &ctx->failed);
        if (ctx->failed) return uri;
        memcpy(mediatype.ptr + mediatype.len, ";base64", 7); /* the buffer from parse.DataURI has room */
        mediatype.len += 7;
    } else {
        data = min_encode_data_uri(ctx->a, data, &ctx->failed);
        if (ctx->failed) return uri;
    }
    if (mediatype.len >= 10 && min_equal_fold(min_slice(mediatype, 0, 10), "text/plain")) {
        mediatype = min_slice(mediatype, 10, mediatype.len);
    }
    for (i = 0; i + 17 <= mediatype.len; i++) {
        /* must start with semicolon and be followed by end of mediatype or semicolon */
        if (mediatype.ptr[i] == ';' && min_equal_fold(min_slice(mediatype, i + 1, i + 17), "charset=us-ascii") &&
            (i + 17 >= mediatype.len || mediatype.ptr[i + 17] == ';')) {
            memmove(mediatype.ptr + i, mediatype.ptr + i + 17, mediatype.len - (i + 17));
            mediatype.len -= 17;
            break;
        }
    }
    n = 5 + mediatype.len + 1 + data.len;
    result = (unsigned char *)cu_arena_push_bytes(ctx->a, n);
    if (result == NULL) {
        ctx->failed = 1;
        return uri;
    }
    memcpy(result, "data:", 5);
    memcpy(result + 5, mediatype.ptr, mediatype.len);
    result[5 + mediatype.len] = ',';
    memcpy(result + 6 + mediatype.len, data.ptr, data.len);
    return min_bytes(result, n);
}

/* ---- html.go ----------------------------------------------------------- */

static int html_entity_lookup(const unsigned char *name, size_t n, const unsigned char **value, size_t *value_len)
{
    const MinifyEntity *e = minify_html_entity(name, n);

    if (e == NULL) return 0;
    *value = (const unsigned char *)e->value;
    *value_len = e->value_len;
    return 1;
}

/* table.go TextRevEntitiesMap: only '<'. */
static const char *const html_rev[256] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 0x00 */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 0x10 */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 0x20 */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "&lt;", NULL, NULL, NULL, /* 0x30 */
};

static const MinEntityMaps html_text_maps = {html_entity_lookup, html_rev};
static const MinEntityMaps html_attr_maps = {html_entity_lookup, NULL};

static void w(CuBuilder *out, MinBytes b)
{
    if (b.len > 0) cu_sb_str(out, min_as_str(b));
}

static void wlit(CuBuilder *out, const char *s)
{
    cu_sb_str(out, cu_str_cstr(s));
}

static int flag(const MinCtx *ctx, unsigned long f)
{
    return (ctx->flags & f) != 0;
}

/* util.go EscapeAttrVal: the value with the quotes it needs, in ctx's
 * attribute buffer; or b itself when none are needed. */
static MinBytes escape_attr_val(MinCtx *ctx, MinBytes b, unsigned char orig_quote, int must_quote, int is_xml)
{
    size_t singles = 0;
    size_t doubles = 0;
    int unquoted = 1;
    size_t i;
    size_t n;
    unsigned char quote;
    const char *escaped;
    size_t escaped_len;
    unsigned char *t;
    size_t j;
    size_t start;

    for (i = 0; i < b.len; i++) {
        unsigned char c = b.ptr[i];

        /* util.go charTable: tab, LF, FF, CR, space, ", ', <, =, >, ` */
        if (c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ' || c == '"' || c == '\'' || c == '<' ||
            c == '=' || c == '>' || c == '`') {
            unquoted = 0;
            if (c == '"') doubles++;
            else if (c == '\'') singles++;
        }
    }
    if (unquoted && (!must_quote || orig_quote == 0) && !is_xml) return b;
    if ((singles == 0 && orig_quote == '\'' && !is_xml) || (doubles == 0 && orig_quote == '"')) {
        t = min_buf_reserve(&ctx->attr_buf, ctx->a, b.len + 2, &ctx->failed);
        if (t == NULL) return b;
        t[0] = orig_quote;
        if (b.len > 0) memcpy(t + 1, b.ptr, b.len);
        t[1 + b.len] = orig_quote;
        return min_bytes(t, b.len + 2);
    }
    n = b.len + 2;
    if (singles >= doubles || is_xml) {
        n += doubles * 4;
        quote = '"';
        escaped = "&#34;";
        if (singles == doubles && orig_quote == '\'' && !is_xml) {
            quote = '\'';
            escaped = "&#39;";
        }
    } else {
        n += singles * 4;
        quote = '\'';
        escaped = "&#39;";
    }
    escaped_len = 5;
    t = min_buf_reserve(&ctx->attr_buf, ctx->a, n, &ctx->failed);
    if (t == NULL) return b;
    t[0] = quote;
    j = 1;
    start = 0;
    for (i = 0; i < b.len; i++) {
        if (b.ptr[i] == quote) {
            memcpy(t + j, b.ptr + start, i - start);
            j += i - start;
            memcpy(t + j, escaped, escaped_len);
            j += escaped_len;
            start = i + 1;
        }
    }
    memcpy(t + j, b.ptr + start, b.len - start);
    j += b.len - start;
    t[j] = quote;
    return min_bytes(t, j + 1);
}

/* bytes.ReplaceAll(b, old, new) with len(new) <= len(old), in place. */
static MinBytes replace_all(MinBytes b, const char *old, const char *new_)
{
    size_t olen = strlen(old);
    size_t nlen = strlen(new_);
    size_t i = 0;
    size_t j = 0;

    while (i < b.len) {
        if (b.len - i >= olen && memcmp(b.ptr + i, old, olen) == 0) {
            memcpy(b.ptr + j, new_, nlen);
            j += nlen;
            i += olen;
        } else {
            b.ptr[j++] = b.ptr[i++];
        }
    }
    return min_slice(b, 0, j);
}

/* html.go: the viewport content rewrite inside the Meta branch. */
static void minify_viewport(MinCtx *ctx, MinHtmlToken *content)
{
    MinBytes v = replace_all(content->attr_val, " ", "");
    size_t i;

    for (i = 0; i < v.len; i++) {
        if (v.ptr[i] == '=' && i + 2 < v.len) {
            size_t n;

            i++;
            n = min_number_len(min_slice(v, i, v.len));
            if (n > 0) {
                MinBytes num = min_minify_number(ctx->a, min_slice(v, i, i + n), -1, &ctx->failed);

                if (num.len < n) {
                    memmove(v.ptr + i + num.len, v.ptr + i + n, v.len - (i + n));
                    v.len = v.len + num.len - n;
                }
                i += num.len;
            }
            i--; /* mitigate for-loop increase */
        }
    }
    content->attr_val = v;
}

/* Minifier.Minify. Returns 0 when it could not finish; ctx says why. */
int minify_html(MinCtx *ctx, MinBytes src, CuBuilder *out)
{
    MinInput input;
    MinHtmlLexer lexer;
    MinHtmlTokens tb;
    MinHtmlLexer *l = &lexer;
    unsigned long raw_tag_hash = 0;
    MinBytes raw_tag_mediatype = min_bytes_nil();
    int omit_space = 1; /* if true the next leading space is omitted */
    int in_pre = 0;

    min_input_init(&input, src.ptr, src.len);
    min_html_lex_init(l, &input);
    if (!tokens_init(&tb, ctx, l)) return 0;
    for (;;) {
        MinHtmlToken t = *tokens_shift(&tb);

        if (ctx->failed || ctx->unsupported || ctx->limit || ctx->invalid) return 0;
        switch (t.type) {
        case MH_ERROR_TOKEN:
            if (l->err) {
                ctx->invalid = 1;
                return 0;
            }
            return !out->failed;
        case MH_DOCTYPE_TOKEN:
            wlit(out, "<!doctype html>");
            break;
        case MH_COMMENT_TOKEN:
            if (flag(ctx, CU_MINIFY_KEEP_COMMENTS)) {
                w(out, t.data);
            } else if (flag(ctx, CU_MINIFY_KEEP_CONDITIONAL_COMMENTS) && 6 < t.text.len &&
                       (min_has_prefix(t.text, "[if ") || min_has_suffix(t.text, "[endif]") ||
                        min_has_suffix(t.text, "[endif]--"))) {
                /* [if ...] is always 7 or more characters, [endif] is only encountered for downlevel-revealed */
                if (min_has_prefix(t.data, "<!--[if ") && min_has_suffix(t.data, "<![endif]-->")) { /* downlevel-hidden */
                    size_t begin = 0; /* bytes.IndexByte(t.Data, '>') + 1; the suffix guarantees a '>' */
                    size_t end = t.data.len - 12;

                    while (t.data.ptr[begin] != '>') begin++;
                    begin++;
                    if (begin < end) {
                        w(out, min_slice(t.data, 0, begin));
                        nested(ctx, SLOT_HTML, min_slice(t.data, begin, end), out);
                        if (ctx->failed || ctx->unsupported || ctx->limit || ctx->invalid) return 0;
                        w(out, min_slice(t.data, end, t.data.len));
                    } else {
                        w(out, t.data); /* malformed */
                    }
                } else {
                    w(out, t.data); /* downlevel-revealed or short downlevel-hidden */
                }
            } else if (1 < t.text.len && t.text.ptr[0] == '#') {
                /* SSI tags */
                w(out, t.data);
            }
            break;
        case MH_SVG_TOKEN:
            if (!minify_mimetype(ctx, min_bytes((unsigned char *)"image/svg+xml", 13), t.data, out)) w(out, t.data);
            break;
        case MH_MATH_TOKEN:
            if (!minify_mimetype(ctx, min_bytes((unsigned char *)"application/mathml+xml", 22), t.data, out)) {
                w(out, t.data);
            }
            break;
        case MH_TEXT_TOKEN:
            if (raw_tag_hash != 0) {
                if (raw_tag_hash == MH_STYLE || raw_tag_hash == MH_SCRIPT || raw_tag_hash == MH_IFRAME) {
                    MinBytes mimetype;

                    if (raw_tag_hash == MH_IFRAME) mimetype = min_bytes((unsigned char *)"text/html", 9);
                    else if (raw_tag_mediatype.len > 0) mimetype = min_mediatype_mime(raw_tag_mediatype);
                    else if (raw_tag_hash == MH_SCRIPT) mimetype = min_bytes((unsigned char *)"application/javascript", 22);
                    else mimetype = min_bytes((unsigned char *)"text/css", 8);
                    if (!minify_mimetype(ctx, mimetype, t.data, out)) w(out, t.data);
                } else {
                    w(out, t.data);
                }
            } else if (in_pre) {
                w(out, t.data);
            } else {
                t.data = min_replace_multiple_whitespace_and_entities(t.data, &html_text_maps);

                /* whitespace removal; trim left */
                if (omit_space && t.data.len > 0 && min_is_whitespace(t.data.ptr[0])) {
                    t.data = min_slice(t.data, 1, t.data.len);
                }

                /* whitespace removal; trim right */
                omit_space = 0;
                if (t.data.len == 0) {
                    omit_space = 1;
                } else if (min_is_whitespace(t.data.ptr[t.data.len - 1])) {
                    size_t i = 0;

                    omit_space = 1;
                    for (;;) {
                        const MinHtmlToken *next = tokens_peek(&tb, i);

                        /* trim if EOF, text token with leading whitespace or block token */
                        if (next->type == MH_ERROR_TOKEN) {
                            t.data.len--;
                            omit_space = 0;
                            break;
                        } else if (next->type == MH_TEXT_TOKEN && !min_is_all_whitespace(next->data)) {
                            /* stop looking when text encountered */
                            break;
                        } else if (next->type == MH_START_TAG_TOKEN || next->type == MH_END_TAG_TOKEN) {
                            if (flag(ctx, CU_MINIFY_KEEP_WHITESPACE)) break;
                            /* remove when followed by a block tag */
                            if (next->traits & MH_BLOCK_TAG) {
                                t.data.len--;
                                omit_space = 0;
                                break;
                            } else if (next->type == MH_START_TAG_TOKEN) {
                                break;
                            }
                        }
                        i++;
                    }
                }
                w(out, t.data);
            }
            break;
        case MH_START_TAG_TOKEN:
        case MH_END_TAG_TOKEN: {
            int has_attributes = 0;

            raw_tag_hash = 0;
            if (t.type == MH_START_TAG_TOKEN) {
                if (tokens_peek(&tb, 0)->type == MH_ATTRIBUTE_TOKEN) has_attributes = 1;
                if (t.traits & MH_RAW_TAG) {
                    /* ignore empty script and style tags */
                    if (!has_attributes && (t.hash == MH_SCRIPT || t.hash == MH_STYLE)) {
                        if (tokens_peek(&tb, 1)->type == MH_END_TAG_TOKEN) {
                            tokens_shift(&tb);
                            tokens_shift(&tb);
                            break;
                        }
                    }
                    raw_tag_hash = t.hash;
                    raw_tag_mediatype = min_bytes_nil();

                    /* do not minify content of <style amp-boilerplate> */
                    if (has_attributes && t.hash == MH_STYLE) {
                        unsigned long want = MH_AMP_BOILERPLATE;
                        MinHtmlToken *attrs[1];

                        tokens_attributes(&tb, &want, 1, attrs);
                        if (attrs[0] != NULL) raw_tag_hash = 0;
                    }
                }
            } else if (t.hash == MH_TEMPLATE) {
                omit_space = 1; /* EndTagToken */
            }

            if (t.hash == MH_PRE) in_pre = t.type == MH_START_TAG_TOKEN;

            /* remove superfluous tags, except for html, head and body tags when KeepDocumentTags is set */
            if (!has_attributes &&
                ((!flag(ctx, CU_MINIFY_KEEP_DOCUMENT_TAGS) && (t.hash == MH_HTML || t.hash == MH_HEAD || t.hash == MH_BODY)) ||
                 t.hash == MH_COLGROUP)) {
                break;
            } else if (t.type == MH_END_TAG_TOKEN) {
                int omit_end_tag = 0;

                if (!flag(ctx, CU_MINIFY_KEEP_END_TAGS)) {
                    if (t.hash == MH_THEAD || t.hash == MH_TBODY || t.hash == MH_TFOOT || t.hash == MH_TR ||
                        t.hash == MH_TH || t.hash == MH_TD || t.hash == MH_OPTION || t.hash == MH_DD ||
                        t.hash == MH_DT || t.hash == MH_LI || t.hash == MH_RB || t.hash == MH_RT ||
                        t.hash == MH_RTC || t.hash == MH_RP) {
                        omit_end_tag = 1; /* omit end tags */
                    } else if (t.hash == MH_P) {
                        size_t i = 0;

                        for (;;) {
                            const MinHtmlToken *next = tokens_peek(&tb, i);

                            i++;
                            /* continue if text token is empty or whitespace */
                            if (next->type == MH_TEXT_TOKEN && min_is_all_whitespace(next->data)) continue;
                            if (next->type == MH_ERROR_TOKEN ||
                                (next->type == MH_END_TAG_TOKEN && (next->traits & MH_KEEP_P_TAG) == 0) ||
                                (next->type == MH_START_TAG_TOKEN && (next->traits & MH_OMIT_P_TAG) != 0)) {
                                omit_end_tag = 1; /* omit p end tag */
                            }
                            break;
                        }
                    } else if (t.hash == MH_OPTGROUP) {
                        size_t i = 0;

                        for (;;) {
                            const MinHtmlToken *next = tokens_peek(&tb, i);

                            i++;
                            /* continue if text token */
                            if (next->type == MH_TEXT_TOKEN) continue;
                            if (next->type == MH_ERROR_TOKEN || next->hash != MH_OPTION) {
                                omit_end_tag = 1; /* omit optgroup end tag */
                            }
                            break;
                        }
                    }
                }

                if (!omit_end_tag) {
                    if (flag(ctx, CU_MINIFY_KEEP_WHITESPACE) || (t.traits & MH_OBJECT_TAG)) {
                        omit_space = 0;
                    } else if (t.traits & MH_BLOCK_TAG) {
                        omit_space = 1; /* omit spaces after block elements */
                    }

                    if (3 + t.text.len < t.data.len) {
                        t.data.ptr[2 + t.text.len] = '>';
                        t.data.len = 3 + t.text.len;
                    }
                    w(out, t.data);
                }

                /* skip text in select and optgroup tags */
                if (t.hash == MH_OPTION || t.hash == MH_OPTGROUP) {
                    if (tokens_peek(&tb, 0)->type == MH_TEXT_TOKEN) tokens_shift(&tb);
                }
                break;
            }

            if (flag(ctx, CU_MINIFY_KEEP_WHITESPACE) || (t.traits & MH_OBJECT_TAG)) {
                omit_space = 0;
            } else if (t.traits & MH_BLOCK_TAG) {
                omit_space = 1; /* omit spaces after block elements */
            }

            w(out, t.data);

            if (has_attributes) {
                if (t.hash == MH_META) {
                    static const unsigned long want[4] = {MH_CONTENT, MH_HTTP_EQUIV, MH_CHARSET, MH_NAME};
                    MinHtmlToken *attrs[4];
                    MinHtmlToken *content;

                    tokens_attributes(&tb, want, 4, attrs);
                    content = attrs[0];
                    if (content != NULL) {
                        MinHtmlToken *http_equiv = attrs[1];
                        MinHtmlToken *name = attrs[3];

                        if (http_equiv != NULL) {
                            http_equiv->attr_val = min_trim_whitespace(http_equiv->attr_val);
                            if (attrs[2] == NULL && min_equal_fold(http_equiv->attr_val, "content-type")) {
                                content->attr_val = min_minify_mediatype(content->attr_val);
                                if (min_bytes_eq(content->attr_val, "text/html;charset=utf-8")) {
                                    unsigned char *utf8 = (unsigned char *)cu_arena_push_bytes(ctx->a, 12);

                                    if (utf8 == NULL) {
                                        ctx->failed = 1;
                                        return 0;
                                    }
                                    memcpy(utf8, "charsetutf-8", 12);
                                    http_equiv->text = min_bytes_nil();
                                    content->text = min_bytes(utf8, 7);
                                    content->hash = MH_CHARSET;
                                    content->attr_val = min_bytes(utf8 + 7, 5);
                                }
                            }
                        }
                        if (name != NULL) {
                            name->attr_val = min_trim_whitespace(name->attr_val);
                            if (min_equal_fold(name->attr_val, "keywords")) {
                                content->attr_val = replace_all(content->attr_val, ", ", ",");
                            } else if (min_equal_fold(name->attr_val, "viewport")) {
                                minify_viewport(ctx, content);
                            }
                        }
                    }
                } else if (t.hash == MH_SCRIPT) {
                    static const unsigned long want[2] = {MH_SRC, MH_CHARSET};
                    MinHtmlToken *attrs[2];

                    tokens_attributes(&tb, want, 2, attrs);
                    if (attrs[0] != NULL && attrs[1] != NULL) attrs[1]->text = min_bytes_nil();
                } else if (t.hash == MH_INPUT) {
                    static const unsigned long want[2] = {MH_TYPE, MH_VALUE};
                    MinHtmlToken *attrs[2];

                    tokens_attributes(&tb, want, 2, attrs);
                    if (attrs[0] != NULL && attrs[1] != NULL) {
                        int is_radio = min_equal_fold(attrs[0]->attr_val, "radio");

                        if (!is_radio && attrs[1]->attr_val.len == 0) attrs[1]->text = min_bytes_nil();
                        else if (is_radio && min_equal_fold(attrs[1]->attr_val, "on")) attrs[1]->text = min_bytes_nil();
                    }
                } else if (t.hash == MH_A) {
                    static const unsigned long want[2] = {MH_ID, MH_NAME};
                    MinHtmlToken *attrs[2];

                    tokens_attributes(&tb, want, 2, attrs);
                    if (attrs[0] != NULL && attrs[1] != NULL) {
                        if (attrs[0]->attr_val.len == attrs[1]->attr_val.len &&
                            memcmp(attrs[0]->attr_val.ptr, attrs[1]->attr_val.ptr, attrs[0]->attr_val.len) == 0) {
                            attrs[1]->text = min_bytes_nil();
                        }
                    }
                }

                /* write attributes */
                for (;;) {
                    MinHtmlToken attr = *tokens_shift(&tb);
                    MinBytes val;

                    if (ctx->failed) return 0;
                    if (attr.type != MH_ATTRIBUTE_TOKEN) break;
                    if (attr.text.ptr == NULL) continue; /* removed attribute */

                    val = attr.attr_val;
                    if (attr.traits & MH_TRIM_ATTR) {
                        val = min_replace_multiple_whitespace_and_entities(val, &html_attr_maps);
                        val = min_trim_whitespace(val);
                    } else {
                        val = min_replace_entities(val, &html_attr_maps);
                    }
                    if (t.traits != 0) {
                        if (val.len == 0 && (attr.hash == MH_CLASS || attr.hash == MH_DIR || attr.hash == MH_ID ||
                                             attr.hash == MH_NAME || (attr.hash == MH_ACTION && t.hash == MH_FORM))) {
                            continue; /* omit empty attribute values */
                        }
                        if (raw_tag_hash != 0 && attr.hash == MH_TYPE) {
                            if (!copy_input(ctx, val, &raw_tag_mediatype)) return 0;
                        }

                        if (attr.hash == MH_ENCTYPE || attr.hash == MH_FORMENCTYPE || attr.hash == MH_ACCEPT ||
                            (attr.hash == MH_TYPE &&
                             (t.hash == MH_A || t.hash == MH_LINK || t.hash == MH_EMBED || t.hash == MH_OBJECT ||
                              t.hash == MH_SOURCE || t.hash == MH_SCRIPT))) {
                            val = min_minify_mediatype(val);
                        }

                        /* default attribute values can be omitted */
                        if (!flag(ctx, CU_MINIFY_KEEP_DEFAULT_ATTR_VALS)) {
                            int js_type = 0;

                            if (attr.hash == MH_TYPE && t.hash == MH_SCRIPT) {
                                MinBytes lower;

                                if (!copy_input(ctx, val, &lower)) return 0;
                                min_to_lower(lower);
                                js_type = minify_html_js_mimetype(lower.ptr, lower.len);
                            }
                            if ((attr.hash == MH_TYPE &&
                                 (js_type || (t.hash == MH_STYLE && min_equal_fold(val, "text/css")) ||
                                  (t.hash == MH_LINK && min_equal_fold(val, "text/css")) ||
                                  (t.hash == MH_INPUT && min_equal_fold(val, "text")) ||
                                  (t.hash == MH_BUTTON && min_equal_fold(val, "submit")))) ||
                                (attr.hash == MH_METHOD && min_equal_fold(val, "get")) ||
                                (attr.hash == MH_ENCTYPE && min_equal_fold(val, "application/x-www-form-urlencoded")) ||
                                /* html.go's oneBytes is []byte("one"), so a value of 1 stays */
                                (attr.hash == MH_COLSPAN && min_bytes_eq(val, "one")) ||
                                (attr.hash == MH_ROWSPAN && min_bytes_eq(val, "one")) ||
                                (attr.hash == MH_SHAPE && min_equal_fold(val, "rect")) ||
                                (attr.hash == MH_SPAN && min_bytes_eq(val, "one")) ||
                                (attr.hash == MH_MEDIA && t.hash == MH_STYLE && min_equal_fold(val, "all"))) {
                                continue;
                            }
                        }

                        if (attr.hash == MH_STYLE) {
                            /* CSS minifier for attribute inline code */
                            CuBuilder b;
                            CuStr s;

                            val = min_trim_whitespace(val);
                            cu_sb_init(&b, ctx->a);
                            if (minify_mimetype(ctx, min_bytes((unsigned char *)"text/css", 8), val, &b)) {
                                if (ctx->unsupported) return 0;
                                if (!cu_sb_finish(&b, &s)) {
                                    ctx->failed = 1;
                                    return 0;
                                }
                                val = min_bytes((unsigned char *)s.ptr, s.len);
                            }
                            if (val.len == 0) continue;
                        } else if (2 < attr.text.len && attr.text.ptr[0] == 'o' && attr.text.ptr[1] == 'n') {
                            /* JS minifier for attribute inline code */
                            CuBuilder b;
                            CuStr s;

                            val = min_trim_whitespace(val);
                            if (11 <= val.len && min_equal_fold(min_slice(val, 0, 11), "javascript:")) {
                                val = min_slice(val, 11, val.len);
                            }
                            cu_sb_init(&b, ctx->a);
                            if (minify_mimetype(ctx, min_bytes((unsigned char *)"application/javascript", 22), val, &b)) {
                                if (ctx->unsupported) return 0;
                                if (!cu_sb_finish(&b, &s)) {
                                    ctx->failed = 1;
                                    return 0;
                                }
                                val = min_bytes((unsigned char *)s.ptr, s.len);
                            }
                            if (val.len == 0) continue;
                        } else if (attr.traits & MH_URL_ATTR) { /* anchors are already handled */
                            val = min_trim_whitespace(val);
                            if (5 < val.len) {
                                if (min_equal_fold(min_slice(val, 0, 4), "http")) {
                                    /* m.URL is nil in Hugo: the scheme is lowercased, never stripped */
                                    if (val.ptr[4] == ':') {
                                        min_to_lower(min_slice(val, 0, 4));
                                    } else if ((val.ptr[4] == 's' || val.ptr[4] == 'S') && val.ptr[5] == ':') {
                                        min_to_lower(min_slice(val, 0, 5));
                                    }
                                } else if (min_equal_fold(min_slice(val, 0, 5), "data:")) {
                                    val = data_uri(ctx, val);
                                    if (ctx->failed || ctx->unsupported || ctx->limit || ctx->invalid) return 0;
                                }
                            }
                        }
                    }

                    wlit(out, " ");
                    w(out, attr.text);
                    if (0 < val.len && (attr.traits & MH_BOOLEAN_ATTR) == 0) {
                        /* use double quotes for RDFa attributes */
                        int is_xml = attr.hash == MH_VOCAB || attr.hash == MH_TYPEOF || attr.hash == MH_PROPERTY ||
                                     attr.hash == MH_RESOURCE || attr.hash == MH_PREFIX || attr.hash == MH_CONTENT ||
                                     attr.hash == MH_ABOUT || attr.hash == MH_REV || attr.hash == MH_DATATYPE ||
                                     attr.hash == MH_INLIST;
                        unsigned char quote = 0;

                        wlit(out, "=");
                        /* no quotes if possible, else prefer single or double depending on which occurs more often in value */
                        if (0 < attr.data.len &&
                            (attr.data.ptr[attr.data.len - 1] == '\'' || attr.data.ptr[attr.data.len - 1] == '"')) {
                            quote = attr.data.ptr[attr.data.len - 1];
                        }
                        val = escape_attr_val(ctx, val, quote, flag(ctx, CU_MINIFY_KEEP_QUOTES), is_xml);
                        if (ctx->failed) return 0;
                        w(out, val);
                    }
                }
            } else {
                tokens_shift(&tb); /* StartTagClose */
            }
            wlit(out, ">");

            /* skip text in select and optgroup tags */
            if (t.hash == MH_SELECT || t.hash == MH_OPTGROUP) {
                if (tokens_peek(&tb, 0)->type == MH_TEXT_TOKEN) tokens_shift(&tb);
            }

            /* keep space after phrasing tags (<i>, <span>, ...) FontAwesome etc. */
            if (t.type == MH_START_TAG_TOKEN && t.traits == MH_NORMAL_TAG) {
                const MinHtmlToken *next = tokens_peek(&tb, 0);

                if (next->hash == t.hash && next->type == MH_END_TAG_TOKEN) omit_space = 0;
            }
            break;
        }
        default:
            /* StartTagClose, StartTagVoid and Attribute tokens are consumed
             * with their start tag; Go's switch has no case for them. */
            break;
        }
    }
}
