/* goldmark renderer/html/html.go and the node renderers of extension/
 * table.go, strikethrough.go, tasklist.go, definition_list.go and
 * footnote.go as C, with the walk of renderer.go's Render and ast.Walk done
 * iteratively. Each function names the Go function it ports. */
#include "mark_html.h"
#include "mark_util.h"

typedef struct Renderer {
    MarkCtx *ctx;
    const CuMarkOptions *opt;
    CuBuilder *w;
    int hard_wraps;
    int xhtml;
    int unsafe;
} Renderer;

static void write_str(Renderer *r, const char *s)
{
    cu_sb_str(r->w, cu_str_cstr(s));
}

static void write_bytes(Renderer *r, CuStr s)
{
    cu_sb_str(r->w, s);
}

static CuStr line_str(Renderer *r, MarkLine l)
{
    (void)r;
    return cu_str_n((const char *)l.bytes, (size_t)l.n);
}

/* ---- The default Writer (html.defaultWriter) ---- */

/* defaultWriter.RawWrite: HTML-escapes " & < > */
static void raw_write(Renderer *r, CuStr s)
{
    mark_escape_html(r->w, s);
}

/* RawWrite of a line view: its padding spaces, then its bytes. */
static void raw_write_line(Renderer *r, MarkLine l)
{
    long k;

    for (k = 0; k < l.pad; k++) cu_sb_char(r->w, ' ');
    raw_write(r, line_str(r, l));
}

/* defaultWriter.SecureWrite: NUL becomes U+FFFD, nothing is escaped. */
static void secure_write(Renderer *r, MarkLine l)
{
    long k;
    size_t n = 0;
    size_t i;
    CuStr s = line_str(r, l);

    for (k = 0; k < l.pad; k++) cu_sb_char(r->w, ' ');
    for (i = 0; i < s.len; i++) {
        if (s.ptr[i] == '\0') {
            write_bytes(r, cu_str_n(s.ptr + i - n, n));
            n = 0;
            write_str(r, "\xef\xbf\xbd");
            continue;
        }
        n++;
    }
    if (n != 0) write_bytes(r, cu_str_n(s.ptr + s.len - n, n));
}

/* html.escapeRune */
static void escape_rune(Renderer *r, unsigned long rune)
{
    unsigned char buf[4];
    long n;

    if (rune < 256) {
        const char *v = mark_escape_html_byte((unsigned char)rune);

        if (v != NULL) {
            write_str(r, v);
            return;
        }
    }
    n = mark_encode_rune(buf, mark_to_valid_rune(rune));
    write_bytes(r, cu_str_n((const char *)buf, (size_t)n));
}

/* strconv.ParseUint(s, base, 32) on at most 7 digits: never overflows. */
static unsigned long parse_digits(const unsigned char *s, size_t len, unsigned long base)
{
    unsigned long v = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned long d;

        if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
        else if (s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
        else d = s[i] - 'A' + 10;
        v = v * base + d;
    }
    return v;
}

/* defaultWriter.Write: unescapes backslash escapes, resolves entity and
 * numeric references, replaces NUL, and HTML-escapes the rest. */
static void writer_write(Renderer *r, CuStr source)
{
    const unsigned char *s = (const unsigned char *)source.ptr;
    size_t limit = source.len;
    int escaped = 0;
    size_t n = 0;
    size_t i;

    for (i = 0; i < limit; i++) {
        unsigned char c = s[i];

        if (escaped) {
            if (mark_byte_punct(c)) {
                raw_write(r, cu_str_substr(source, n, i - 1));
                n = i;
                escaped = 0;
                continue;
            }
        }
        if (c == '\0') {
            raw_write(r, cu_str_substr(source, n, i));
            write_str(r, "\xef\xbf\xbd");
            n = i + 1;
            escaped = 0;
            continue;
        }
        if (c == '&') {
            size_t pos = i;
            size_t next = i + 1;

            if (next < limit && s[next] == '#') {
                size_t nnext = next + 1;

                if (nnext < limit) {
                    unsigned char nc = s[nnext];

                    if (nc == 'x' || nc == 'X') { /* &#x22; */
                        size_t start = nnext + 1;

                        for (i = start; i < limit && mark_byte_hex(s[i]); i++) {
                        }
                        if (i > start && i < limit && s[i] == ';' && i - start < 7) {
                            unsigned long v = parse_digits(s + start, i - start, 16);

                            raw_write(r, cu_str_substr(source, n, pos));
                            n = i + 1;
                            escape_rune(r, v);
                            continue;
                        }
                    } else if (nc >= '0' && nc <= '9') { /* &#1234; */
                        size_t start = nnext;

                        for (i = start; i < limit && mark_byte_numeric(s[i]); i++) {
                        }
                        if (i > start && i < limit && i - start < 8 && s[i] == ';') {
                            unsigned long v = parse_digits(s + start, i - start, 10);

                            raw_write(r, cu_str_substr(source, n, pos));
                            n = i + 1;
                            escape_rune(r, v);
                            continue;
                        }
                    }
                }
            } else {
                size_t start = next;

                for (i = start; i < limit && mark_byte_alnum(s[i]); i++) {
                }
                if (i > start && i < limit && s[i] == ';') { /* an entity reference */
                    const MarkEntity *e = mark_entity_lookup(cu_str_substr(source, start, i));

                    if (e != NULL) {
                        raw_write(r, cu_str_substr(source, n, pos));
                        n = i + 1;
                        raw_write(r, cu_str_n(e->chars, e->chars_len));
                        continue;
                    }
                }
            }
            i = next - 1;
        }
        if (c == '\\') {
            escaped = 1;
            continue;
        }
        escaped = 0;
    }
    raw_write(r, cu_str_skip(source, n));
}

/* ---- Attributes (html.RenderAttributes and the filters) ---- */

/* html.GlobalAttributeFilter */
static const char *const global_attributes[] = {
    "accesskey", "autocapitalize", "autofocus", "class", "contenteditable", "dir", "draggable", "enterkeyhint",
    "hidden", "id", "inert", "inputmode", "is", "itemid", "itemprop", "itemref", "itemscope", "itemtype",
    "lang", "part", "role", "slot", "spellcheck", "style", "tabindex", "title", "translate", NULL};

static const char *const blockquote_attributes[] = {"cite", NULL};
static const char *const list_attributes[] = {"start", "reversed", "type", NULL};
static const char *const list_item_attributes[] = {"value", NULL};
static const char *const thematic_attributes[] = {"align", "color", "noshade", "size", "width", NULL};
static const char *const link_attributes[] = {"download", "hreflang", "media", "ping", "referrerpolicy", "rel",
                                              "shape", "target", NULL};
static const char *const image_attributes[] = {"align", "border", "crossorigin", "decoding", "height",
                                               "importance", "intrinsicsize", "ismap", "loading",
                                               "referrerpolicy", "sizes", "srcset", "usemap", "width", NULL};
static const char *const no_attributes[] = {NULL};
/* extension/table.go's filters */
static const char *const table_attributes[] = {"align", "bgcolor", "border", "cellpadding", "cellspacing",
                                               "frame", "rules", "summary", "width", NULL};
static const char *const table_header_attributes[] = {"align", "bgcolor", "char", "charoff", "valign", NULL};
static const char *const table_row_attributes[] = {"align", "bgcolor", "char", "charoff", "valign", NULL};
static const char *const table_th_attributes[] = {"abbr", "align", "axis", "bgcolor", "char", "charoff",
                                                  "colspan", "headers", "height", "rowspan", "scope", "valign",
                                                  "width", NULL};
static const char *const table_td_attributes[] = {"abbr", "align", "axis", "bgcolor", "char", "charoff",
                                                  "colspan", "headers", "height", "rowspan", "scope", "valign",
                                                  "width", NULL};

static int in_list(CuStr name, const char *const *list)
{
    size_t i;

    for (i = 0; list[i] != NULL; i++) {
        if (cu_str_eq(name, cu_str_cstr(list[i]))) return 1;
    }
    return 0;
}

/* RenderAttributes with a filter of the global names plus extra. A value
 * that is not a string makes goldmark panic (attr.Value.([]byte)); there
 * is no output to match, so such an attribute is left out here. */
static void render_attributes(Renderer *r, const MarkNode *node, const char *const *extra)
{
    size_t i;

    for (i = 0; i < cu_vec_len(&node->attrs); i++) {
        const MarkAttr *a = CU_VEC_AT(&node->attrs, MarkAttr, i);

        if (!in_list(a->name, global_attributes) && !in_list(a->name, extra)) {
            if (!cu_str_starts_with(a->name, CU_LIT("data-"))) continue;
        }
        if (a->value.kind != MARK_ATTR_STRING) continue;
        write_str(r, " ");
        write_bytes(r, a->name);
        write_str(r, "=\"");
        mark_escape_html(r->w, a->value.str);
        write_str(r, "\"");
    }
}

/* ---- Blocks ---- */

/* Renderer.writeLines */
static void write_lines(Renderer *r, const MarkNode *n)
{
    size_t l = n->lines == NULL ? 0 : cu_vec_len(n->lines);
    size_t i;

    for (i = 0; i < l; i++) {
        MarkSegment s = mark_lines_at(n->lines, (long)i);

        raw_write_line(r, mark_segment_value(s, r->ctx->src, r->ctx->src_len));
    }
}

/* Renderer.renderHeading */
static void render_heading(Renderer *r, const MarkNode *n, int entering)
{
    static const char digits[] = "0123456";
    long level = n->level < 0 || n->level > 6 ? 0 : n->level;

    if (entering) {
        write_str(r, "<h");
        cu_sb_char(r->w, digits[level]);
        if (mark_node_has_attributes(n)) render_attributes(r, n, no_attributes);
        cu_sb_char(r->w, '>');
    } else {
        write_str(r, "</h");
        cu_sb_char(r->w, digits[level]);
        write_str(r, ">\n");
    }
}

/* Renderer.renderBlockquote */
static void render_blockquote(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        if (mark_node_has_attributes(n)) {
            write_str(r, "<blockquote");
            render_attributes(r, n, blockquote_attributes);
            cu_sb_char(r->w, '>');
        } else {
            write_str(r, "<blockquote>\n");
        }
    } else {
        write_str(r, "</blockquote>\n");
    }
}

/* Renderer.renderCodeBlock */
static void render_code_block(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        write_str(r, "<pre><code>");
        write_lines(r, n);
    } else {
        write_str(r, "</code></pre>\n");
    }
}

/* The CuMarkCodeHook call: 1 when the hook rendered the block. */
static int call_code_hook(Renderer *r, MarkNode *n)
{
    CuMarkCode code;
    CuBuilder text;
    CuVec attrs;
    size_t l = n->lines == NULL ? 0 : cu_vec_len(n->lines);
    size_t i;
    int has;

    if (r->opt == NULL || r->opt->code_hook == NULL) return 0;
    code.info = cu_str_empty();
    if (n->info != NULL) {
        MarkLine info = mark_segment_value(n->info->segment, r->ctx->src, r->ctx->src_len);

        code.info = mark_line_slice(r->ctx->arena, info, 0, mark_line_len(info));
    }
    code.lang = mark_fenced_language(n, r->ctx->src, r->ctx->src_len, &has);
    cu_sb_init(&text, r->ctx->arena);
    for (i = 0; i < l; i++) {
        MarkLine line = mark_segment_value(mark_lines_at(n->lines, (long)i), r->ctx->src, r->ctx->src_len);
        long k;

        for (k = 0; k < line.pad; k++) cu_sb_char(&text, ' ');
        cu_sb_str(&text, line_str(r, line));
    }
    if (!cu_sb_finish(&text, &code.text)) {
        r->ctx->failed = 1;
        return 0;
    }
    CU_VEC_INIT(&attrs, r->ctx->arena, CuMarkAttr);
    for (i = 0; i < cu_vec_len(&n->attrs); i++) {
        const MarkAttr *a = CU_VEC_AT(&n->attrs, MarkAttr, i);
        CuMarkAttr out;

        if (a->value.kind != MARK_ATTR_STRING) continue;
        out.name = a->name;
        out.value = a->value.str;
        CU_VEC_PUSH(&attrs, &out);
    }
    if (cu_vec_failed(&attrs)) {
        r->ctx->failed = 1;
        return 0;
    }
    code.attrs = CU_VEC_DATA(&attrs, CuMarkAttr);
    code.attr_count = cu_vec_len(&attrs);
    return r->opt->code_hook(r->opt->code_ctx, &code, r->w) != 0;
}

/* Renderer.renderFencedCodeBlock, both halves at once: the block has no
 * children, so nothing comes between them. */
static void render_fenced_code_block(Renderer *r, MarkNode *n)
{
    int has;
    CuStr language;

    if (call_code_hook(r, n)) return;
    write_str(r, "<pre><code");
    language = mark_fenced_language(n, r->ctx->src, r->ctx->src_len, &has);
    if (has) {
        write_str(r, " class=\"language-");
        writer_write(r, language);
        write_str(r, "\"");
    }
    cu_sb_char(r->w, '>');
    write_lines(r, n);
    write_str(r, "</code></pre>\n");
}

/* Renderer.renderHTMLBlock */
static void render_html_block(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        if (r->unsafe) {
            size_t l = n->lines == NULL ? 0 : cu_vec_len(n->lines);
            size_t i;

            for (i = 0; i < l; i++) {
                secure_write(r, mark_segment_value(mark_lines_at(n->lines, (long)i), r->ctx->src, r->ctx->src_len));
            }
        } else {
            write_str(r, "<!-- raw HTML omitted -->\n");
        }
    } else if (n->closure_line.start >= 0) { /* HTMLBlock.HasClosure */
        if (r->unsafe) secure_write(r, mark_segment_value(n->closure_line, r->ctx->src, r->ctx->src_len));
        else write_str(r, "<!-- raw HTML omitted -->\n");
    }
}

/* Renderer.renderList */
static void render_list(Renderer *r, const MarkNode *n, int entering)
{
    const char *tag = mark_list_is_ordered(n) ? "ol" : "ul";

    if (entering) {
        cu_sb_char(r->w, '<');
        write_str(r, tag);
        if (mark_list_is_ordered(n) && n->start != 1) {
            write_str(r, " start=\"");
            cu_sb_long(r->w, n->start);
            write_str(r, "\"");
        }
        if (mark_node_has_attributes(n)) render_attributes(r, n, list_attributes);
        write_str(r, ">\n");
    } else {
        write_str(r, "</");
        write_str(r, tag);
        write_str(r, ">\n");
    }
}

/* Renderer.renderListItem */
static void render_list_item(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        if (mark_node_has_attributes(n)) {
            write_str(r, "<li");
            render_attributes(r, n, list_item_attributes);
            cu_sb_char(r->w, '>');
        } else {
            write_str(r, "<li>");
        }
        if (n->first != NULL && n->first->kind != MARK_TEXT_BLOCK) cu_sb_char(r->w, '\n');
    } else {
        write_str(r, "</li>\n");
    }
}

/* Renderer.renderParagraph */
static void render_paragraph(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        if (mark_node_has_attributes(n)) {
            write_str(r, "<p");
            render_attributes(r, n, no_attributes);
            cu_sb_char(r->w, '>');
        } else {
            write_str(r, "<p>");
        }
    } else {
        write_str(r, "</p>\n");
    }
}

/* Renderer.renderTextBlock */
static void render_text_block(Renderer *r, const MarkNode *n, int entering)
{
    if (!entering) {
        if (n->next != NULL && n->first != NULL) cu_sb_char(r->w, '\n');
    }
}

/* Renderer.renderThematicBreak */
static void render_thematic_break(Renderer *r, const MarkNode *n, int entering)
{
    if (!entering) return;
    write_str(r, "<hr");
    if (mark_node_has_attributes(n)) render_attributes(r, n, thematic_attributes);
    if (r->xhtml) write_str(r, " />\n");
    else write_str(r, ">\n");
}

/* ---- Inlines ---- */

/* html.hasPrefix: a case-insensitive ASCII prefix test */
static int has_prefix_fold(CuStr s, const char *prefix)
{
    size_t n = strlen(prefix);
    size_t i;

    if (s.len < n) return 0;
    for (i = 0; i < n; i++) {
        if (cu_ascii_to_lower(s.ptr[i]) != cu_ascii_to_lower(prefix[i])) return 0;
    }
    return 1;
}

/* html.IsDangerousURL */
static int is_dangerous_url(CuStr url)
{
    if (has_prefix_fold(url, "data:image/") && url.len >= 11) {
        CuStr v = cu_str_skip(url, 11);

        if (has_prefix_fold(v, "png;") || has_prefix_fold(v, "gif;") || has_prefix_fold(v, "jpeg;") ||
            has_prefix_fold(v, "webp;") || has_prefix_fold(v, "svg+xml;")) {
            return 0;
        }
        return 1;
    }
    return has_prefix_fold(url, "javascript:") || has_prefix_fold(url, "vbscript:") || has_prefix_fold(url, "file:") ||
           has_prefix_fold(url, "data:");
}

/* w.Write(util.EscapeHTML(util.URLEscape(url, resolve))) */
static void write_url(Renderer *r, CuStr url, int resolve)
{
    CuBuilder b;
    CuStr escaped;

    cu_sb_init(&b, r->ctx->arena);
    mark_url_escape(&b, url, resolve);
    if (!cu_sb_finish(&b, &escaped)) {
        r->ctx->failed = 1;
        return;
    }
    mark_escape_html(r->w, escaped);
}

/* Node.Text(source) of a node without children. */
static CuStr leaf_text(Renderer *r, const MarkNode *c)
{
    if (c->kind == MARK_TEXT || c->kind == MARK_DELIMITER || c->kind == MARK_LINK_LABEL_STATE) {
        MarkLine l = mark_segment_value(c->segment, r->ctx->src, r->ctx->src_len);

        return mark_line_slice(r->ctx->arena, l, 0, mark_line_len(l));
    }
    if (c->kind == MARK_STRING) return c->value;
    return cu_str_empty();
}

/* html.nodeToHTMLText, iteratively: the text of n's descendants with
 * leaves HTML-escaped. */
static void node_to_html_text(Renderer *r, const MarkNode *n)
{
    const MarkNode *c = n->first;

    while (c != NULL) {
        if (c->kind == MARK_STRING && (c->flags & MARK_TEXT_CODE)) {
            write_bytes(r, c->value);
        } else if (c->first == NULL) {
            mark_escape_html(r->w, leaf_text(r, c));
            if (c->kind == MARK_TEXT && (c->flags & MARK_TEXT_SOFT_LINE_BREAK)) cu_sb_char(r->w, '\n');
        } else {
            c = c->first;
            continue;
        }
        while (c != n && c->next == NULL) c = c->parent;
        if (c == n) return;
        c = c->next;
    }
}

/* AutoLink.Label: the value's text. */
static CuStr autolink_label(Renderer *r, const MarkNode *n)
{
    if (n->autolink_value == NULL) return cu_str_empty();
    return leaf_text(r, n->autolink_value);
}

/* Renderer.renderAutoLink */
static void render_auto_link(Renderer *r, const MarkNode *n)
{
    CuStr url = mark_autolink_url(r->ctx->arena, n, r->ctx->src, r->ctx->src_len);
    CuStr label = autolink_label(r, n);

    if (cu_arena_failed(r->ctx->arena)) {
        r->ctx->failed = 1;
        return;
    }
    write_str(r, "<a href=\"");
    if (n->autolink_type == MARK_AUTOLINK_EMAIL && !has_prefix_fold(url, "mailto:")) write_str(r, "mailto:");
    write_url(r, url, 0);
    if (mark_node_has_attributes(n)) {
        cu_sb_char(r->w, '"');
        render_attributes(r, n, link_attributes);
        cu_sb_char(r->w, '>');
    } else {
        write_str(r, "\">");
    }
    mark_escape_html(r->w, label);
    write_str(r, "</a>");
}

/* Renderer.renderCodeSpan on entering (it skips its children). */
static void render_code_span(Renderer *r, const MarkNode *n)
{
    const MarkNode *c;

    if (mark_node_has_attributes(n)) {
        write_str(r, "<code");
        render_attributes(r, n, no_attributes);
        cu_sb_char(r->w, '>');
    } else {
        write_str(r, "<code>");
    }
    for (c = n->first; c != NULL; c = c->next) {
        MarkLine value = mark_segment_value(c->segment, r->ctx->src, r->ctx->src_len);
        long len = mark_line_len(value);

        if (len > 0 && mark_line_at(value, len - 1) == '\n') {
            raw_write_line(r, mark_line_sub(value, 0, len - 1));
            write_str(r, " ");
        } else {
            raw_write_line(r, value);
        }
    }
}

/* Renderer.renderEmphasis */
static void render_emphasis(Renderer *r, const MarkNode *n, int entering)
{
    const char *tag = n->level == 2 ? "strong" : "em";

    if (entering) {
        cu_sb_char(r->w, '<');
        write_str(r, tag);
        if (mark_node_has_attributes(n)) render_attributes(r, n, no_attributes);
        cu_sb_char(r->w, '>');
    } else {
        write_str(r, "</");
        write_str(r, tag);
        cu_sb_char(r->w, '>');
    }
}

/* Renderer.renderLink */
static void render_link(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        write_str(r, "<a href=\"");
        if (r->unsafe || !is_dangerous_url(n->destination)) write_url(r, n->destination, 1);
        cu_sb_char(r->w, '"');
        if (n->has_title) {
            write_str(r, " title=\"");
            writer_write(r, n->title);
            cu_sb_char(r->w, '"');
        }
        if (mark_node_has_attributes(n)) render_attributes(r, n, link_attributes);
        cu_sb_char(r->w, '>');
    } else {
        write_str(r, "</a>");
    }
}

/* Renderer.renderImage on entering (it skips its children). */
static void render_image(Renderer *r, const MarkNode *n)
{
    write_str(r, "<img src=\"");
    if (r->unsafe || !is_dangerous_url(n->destination)) write_url(r, n->destination, 1);
    write_str(r, "\" alt=\"");
    node_to_html_text(r, n);
    cu_sb_char(r->w, '"');
    if (n->has_title) {
        write_str(r, " title=\"");
        writer_write(r, n->title);
        cu_sb_char(r->w, '"');
    }
    if (mark_node_has_attributes(n)) render_attributes(r, n, image_attributes);
    if (r->xhtml) write_str(r, " />");
    else write_str(r, ">");
}

/* Renderer.renderRawHTML on entering */
static void render_raw_html(Renderer *r, const MarkNode *n)
{
    size_t l;
    size_t i;

    if (!r->unsafe) {
        write_str(r, "<!-- raw HTML omitted -->");
        return;
    }
    l = n->segments == NULL ? 0 : cu_vec_len(n->segments);
    for (i = 0; i < l; i++) {
        MarkLine v = mark_segment_value(mark_lines_at(n->segments, (long)i), r->ctx->src, r->ctx->src_len);
        long k;

        for (k = 0; k < v.pad; k++) cu_sb_char(r->w, ' ');
        write_bytes(r, line_str(r, v));
    }
}

/* Renderer.renderText on entering. EastAsianLineBreaks is off. */
static void render_text(Renderer *r, const MarkNode *n)
{
    MarkLine v = mark_segment_value(n->segment, r->ctx->src, r->ctx->src_len);

    if (n->flags & MARK_TEXT_RAW) {
        raw_write_line(r, v);
        return;
    }
    writer_write(r, mark_line_slice(r->ctx->arena, v, 0, mark_line_len(v)));
    if ((n->flags & MARK_TEXT_HARD_LINE_BREAK) || ((n->flags & MARK_TEXT_SOFT_LINE_BREAK) && r->hard_wraps)) {
        if (r->xhtml) write_str(r, "<br />\n");
        else write_str(r, "<br>\n");
    } else if (n->flags & MARK_TEXT_SOFT_LINE_BREAK) {
        cu_sb_char(r->w, '\n');
    }
}

/* Renderer.renderString on entering */
static void render_string(Renderer *r, const MarkNode *n)
{
    if (n->flags & MARK_TEXT_CODE) write_bytes(r, n->value);
    else if (n->flags & MARK_TEXT_RAW) raw_write(r, n->value);
    else writer_write(r, n->value);
}

/* ---- extension/table.go (TableHTMLRenderer) ---- */

/* TableHTMLRenderer.renderTable */
static void render_table(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        write_str(r, "<table");
        if (mark_node_has_attributes(n)) render_attributes(r, n, table_attributes);
        write_str(r, ">\n");
    } else {
        write_str(r, "</table>\n");
    }
}

/* TableHTMLRenderer.renderTableHeader */
static void render_table_header(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        write_str(r, "<thead");
        if (mark_node_has_attributes(n)) render_attributes(r, n, table_header_attributes);
        write_str(r, ">\n");
        write_str(r, "<tr>\n"); /* Header <tr> has no separate handle */
    } else {
        write_str(r, "</tr>\n");
        write_str(r, "</thead>\n");
        if (n->next != NULL) write_str(r, "<tbody>\n");
    }
}

/* TableHTMLRenderer.renderTableRow */
static void render_table_row(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        write_str(r, "<tr");
        if (mark_node_has_attributes(n)) render_attributes(r, n, table_row_attributes);
        write_str(r, ">\n");
    } else {
        write_str(r, "</tr>\n");
        if (n->parent != NULL && n->parent->last == n) write_str(r, "</tbody>\n");
    }
}

/* ast.Alignment.String */
static const char *alignment_string(int a)
{
    switch (a) {
    case MARK_ALIGN_LEFT:
        return "left";
    case MARK_ALIGN_RIGHT:
        return "right";
    case MARK_ALIGN_CENTER:
        return "center";
    case MARK_ALIGN_NONE:
        return "none";
    default:
        return "";
    }
}

/* TableHTMLRenderer.renderTableCell. The style method writes the
 * alignment into the node's style attribute (appended after ';' to one
 * that is there) before the attributes are rendered. */
static void render_table_cell(Renderer *r, MarkNode *n, int entering)
{
    const char *tag = n->parent != NULL && n->parent->kind == MARK_TABLE_HEADER ? "th" : "td";

    if (!entering) {
        write_str(r, "</");
        write_str(r, tag);
        write_str(r, ">\n");
        return;
    }
    cu_sb_char(r->w, '<');
    write_str(r, tag);
    if (n->alignment != MARK_ALIGN_NONE) {
        CuMarkTableAlign amethod = r->opt == NULL ? CU_MARK_ALIGN_DEFAULT : r->opt->table_align;

        if (amethod == CU_MARK_ALIGN_DEFAULT) amethod = r->xhtml ? CU_MARK_ALIGN_ATTRIBUTE : CU_MARK_ALIGN_STYLE;
        if (amethod == CU_MARK_ALIGN_ATTRIBUTE) {
            const MarkAttr *a = mark_node_attribute(n, CU_LIT("align"));

            if (a == NULL || a->value.kind != MARK_ATTR_STRING) { /* Skip align render if overridden */
                write_str(r, " align=\"");
                write_str(r, alignment_string(n->alignment));
                write_str(r, "\"");
            }
        } else if (amethod == CU_MARK_ALIGN_STYLE) {
            const MarkAttr *a = mark_node_attribute(n, CU_LIT("style"));
            CuBuilder b;
            CuStr style;

            cu_sb_init(&b, r->ctx->arena);
            /* a non-string style makes Go's v.([]byte) panic; left as is */
            if (a != NULL && a->value.kind == MARK_ATTR_STRING) {
                cu_sb_str(&b, a->value.str);
                cu_sb_char(&b, ';');
            }
            cu_sb_str(&b, CU_LIT("text-align:"));
            cu_sb_str(&b, cu_str_cstr(alignment_string(n->alignment)));
            if (!cu_sb_finish(&b, &style)) {
                r->ctx->failed = 1;
                return;
            }
            mark_node_set_attribute(n, CU_LIT("style"), mark_attr_string(style));
            if (cu_vec_failed(&n->attrs)) {
                r->ctx->failed = 1;
                return;
            }
        }
    }
    if (mark_node_has_attributes(n)) render_attributes(r, n, tag[1] == 'd' ? table_td_attributes : table_th_attributes);
    cu_sb_char(r->w, '>');
}

/* ---- extension/strikethrough.go, tasklist.go ---- */

/* StrikethroughHTMLRenderer.renderStrikethrough */
static void render_strikethrough(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        if (mark_node_has_attributes(n)) {
            write_str(r, "<del");
            render_attributes(r, n, no_attributes);
            cu_sb_char(r->w, '>');
        } else {
            write_str(r, "<del>");
        }
    } else {
        write_str(r, "</del>");
    }
}

/* TaskCheckBoxHTMLRenderer.renderTaskCheckBox on entering */
static void render_task_checkbox(Renderer *r, const MarkNode *n)
{
    if (n->is_checked) write_str(r, "<input checked=\"\" disabled=\"\" type=\"checkbox\"");
    else write_str(r, "<input disabled=\"\" type=\"checkbox\"");
    if (r->xhtml) write_str(r, " /> ");
    else write_str(r, "> ");
}

/* ---- extension/definition_list.go (DefinitionListHTMLRenderer) ---- */

/* DefinitionListHTMLRenderer.renderDefinitionList */
static void render_definition_list(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        if (mark_node_has_attributes(n)) {
            write_str(r, "<dl");
            render_attributes(r, n, no_attributes);
            write_str(r, ">\n");
        } else {
            write_str(r, "<dl>\n");
        }
    } else {
        write_str(r, "</dl>\n");
    }
}

/* DefinitionListHTMLRenderer.renderDefinitionTerm */
static void render_definition_term(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        if (mark_node_has_attributes(n)) {
            write_str(r, "<dt");
            render_attributes(r, n, no_attributes);
            cu_sb_char(r->w, '>');
        } else {
            write_str(r, "<dt>");
        }
    } else {
        write_str(r, "</dt>\n");
    }
}

/* DefinitionListHTMLRenderer.renderDefinitionDescription */
static void render_definition_description(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        write_str(r, "<dd");
        if (mark_node_has_attributes(n)) render_attributes(r, n, no_attributes);
        if (n->is_tight) write_str(r, ">");
        else write_str(r, ">\n");
    } else {
        write_str(r, "</dd>\n");
    }
}

/* ---- extension/footnote.go (FootnoteHTMLRenderer, default config) ---- */

/* FootnoteHTMLRenderer.renderFootnoteLink on entering; IDPrefix is
 * empty, LinkTitle empty, LinkClass "footnote-ref" (no ^^ or %% to
 * apply). */
static void render_footnote_link(Renderer *r, const MarkNode *n)
{
    write_str(r, "<sup id=\"fnref");
    if (n->ref_index > 0) cu_sb_long(r->w, n->ref_index);
    cu_sb_char(r->w, ':');
    cu_sb_long(r->w, n->index);
    write_str(r, "\"><a href=\"#fn:");
    cu_sb_long(r->w, n->index);
    write_str(r, "\" class=\"footnote-ref\" role=\"doc-noteref\">");
    cu_sb_long(r->w, n->index);
    write_str(r, "</a></sup>");
}

/* FootnoteHTMLRenderer.renderFootnoteBacklink on entering; BacklinkClass
 * "footnote-backref", BacklinkHTML "&#x21a9;&#xfe0e;", no title. */
static void render_footnote_backlink(Renderer *r, const MarkNode *n)
{
    write_str(r, "&#160;<a href=\"#fnref");
    if (n->ref_index > 0) cu_sb_long(r->w, n->ref_index);
    cu_sb_char(r->w, ':');
    cu_sb_long(r->w, n->index);
    write_str(r, "\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a>");
}

/* FootnoteHTMLRenderer.renderFootnote */
static void render_footnote(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        write_str(r, "<li id=\"fn:");
        cu_sb_long(r->w, n->index);
        write_str(r, "\"");
        if (mark_node_has_attributes(n)) render_attributes(r, n, list_item_attributes);
        write_str(r, ">\n");
    } else {
        write_str(r, "</li>\n");
    }
}

/* FootnoteHTMLRenderer.renderFootnoteList */
static void render_footnote_list(Renderer *r, const MarkNode *n, int entering)
{
    if (entering) {
        write_str(r, "<div class=\"footnotes\" role=\"doc-endnotes\"");
        if (mark_node_has_attributes(n)) render_attributes(r, n, no_attributes);
        cu_sb_char(r->w, '>');
        if (r->xhtml) write_str(r, "\n<hr />\n");
        else write_str(r, "\n<hr>\n");
        write_str(r, "<ol>\n");
    } else {
        write_str(r, "</ol>\n");
        write_str(r, "</div>\n");
    }
}

/* ---- The walk (renderer.Render over ast.Walk) ---- */

/* One renderer call; returns 1 for ast.WalkSkipChildren. */
static int render_node(Renderer *r, MarkNode *n, int entering)
{
    switch (n->kind) {
    case MARK_DOCUMENT:
        return 0;
    case MARK_HEADING:
        render_heading(r, n, entering);
        return 0;
    case MARK_BLOCKQUOTE:
        render_blockquote(r, n, entering);
        return 0;
    case MARK_CODE_BLOCK:
        render_code_block(r, n, entering);
        return 0;
    case MARK_FENCED_CODE_BLOCK:
        if (entering) render_fenced_code_block(r, n);
        return 0;
    case MARK_HTML_BLOCK:
        render_html_block(r, n, entering);
        return 0;
    case MARK_LIST:
        render_list(r, n, entering);
        return 0;
    case MARK_LIST_ITEM:
        render_list_item(r, n, entering);
        return 0;
    case MARK_PARAGRAPH:
        render_paragraph(r, n, entering);
        return 0;
    case MARK_TEXT_BLOCK:
        render_text_block(r, n, entering);
        return 0;
    case MARK_THEMATIC_BREAK:
        render_thematic_break(r, n, entering);
        return 0;
    case MARK_AUTO_LINK:
        if (entering) render_auto_link(r, n);
        return 0;
    case MARK_CODE_SPAN:
        if (entering) {
            render_code_span(r, n);
            return 1;
        }
        write_str(r, "</code>");
        return 0;
    case MARK_EMPHASIS:
        render_emphasis(r, n, entering);
        return 0;
    case MARK_IMAGE:
        if (entering) {
            render_image(r, n);
            return 1;
        }
        return 0;
    case MARK_LINK:
        render_link(r, n, entering);
        return 0;
    case MARK_RAW_HTML:
        if (entering) render_raw_html(r, n);
        return 1;
    case MARK_TEXT:
        if (entering) render_text(r, n);
        return 0;
    case MARK_STRING:
        if (entering) render_string(r, n);
        return 0;
    case MARK_TABLE:
        render_table(r, n, entering);
        return 0;
    case MARK_TABLE_HEADER:
        render_table_header(r, n, entering);
        return 0;
    case MARK_TABLE_ROW:
        render_table_row(r, n, entering);
        return 0;
    case MARK_TABLE_CELL:
        render_table_cell(r, n, entering);
        return 0;
    case MARK_STRIKETHROUGH:
        render_strikethrough(r, n, entering);
        return 0;
    case MARK_TASK_CHECKBOX:
        if (entering) render_task_checkbox(r, n);
        return 0;
    case MARK_DEFINITION_LIST:
        render_definition_list(r, n, entering);
        return 0;
    case MARK_DEFINITION_TERM:
        render_definition_term(r, n, entering);
        return 0;
    case MARK_DEFINITION_DESCRIPTION:
        render_definition_description(r, n, entering);
        return 0;
    case MARK_FOOTNOTE_LINK:
        if (entering) render_footnote_link(r, n);
        return 0;
    case MARK_FOOTNOTE_BACKLINK:
        if (entering) render_footnote_backlink(r, n);
        return 0;
    case MARK_FOOTNOTE:
        render_footnote(r, n, entering);
        return 0;
    case MARK_FOOTNOTE_LIST:
        render_footnote_list(r, n, entering);
        return 0;
    default:
        /* Delimiter and linkLabelState have no renderer registered. */
        return 0;
    }
}

void mark_render_html(MarkCtx *ctx, MarkNode *doc, const CuMarkOptions *opt, CuBuilder *out)
{
    Renderer r;
    MarkNode *n = doc;

    r.ctx = ctx;
    r.opt = opt;
    r.w = out;
    r.hard_wraps = (ctx->flags & CU_MARK_HARD_WRAPS) != 0;
    r.xhtml = (ctx->flags & CU_MARK_XHTML) != 0;
    r.unsafe = (ctx->flags & CU_MARK_UNSAFE) != 0;
    for (;;) {
        int skip = render_node(&r, n, 1);

        if (ctx->failed || out->failed) return;
        if (!skip && n->first != NULL) {
            n = n->first;
            continue;
        }
        for (;;) {
            render_node(&r, n, 0);
            if (n == doc) return;
            if (n->next != NULL) {
                n = n->next;
                break;
            }
            n = n->parent;
        }
    }
}
