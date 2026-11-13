/* goldmark's inline parsers as C: parser/delimiter.go, emphasis.go,
 * code_span.go, auto_link.go, raw_html.go and link.go, with the delimiter
 * and link-label list methods of parser/parser.go's parseContext. Each
 * function names the Go function it ports. The two regular expressions of
 * raw_html.go are written out as matchers that read runes from the reader
 * exactly as regexp.FindReaderSubmatchIndex does. */
#include "mark_parser.h"
#include "mark_util.h"

/* The current line's bytes as one view: a padded line is copied. The inline
 * parsers run at a trigger byte, past any padding, so the copy is rare. */
static CuStr line_bytes(MarkCtx *ctx, MarkLine line)
{
    if (line.pad == 0) return cu_str_n((const char *)line.bytes, (size_t)line.n);
    return mark_line_copy(ctx->arena, line, 0, mark_line_len(line));
}

/* ---- Delimiters (parser/delimiter.go, parseContext) ---- */

/* parseContext.PushDelimiter */
void mark_push_delimiter(MarkCtx *ctx, MarkNode *d)
{
    if (ctx->delimiters == NULL) {
        ctx->delimiters = d;
        ctx->last_delimiter = d;
    } else {
        MarkNode *l = ctx->last_delimiter;

        ctx->last_delimiter = d;
        l->next_delim = d;
        d->prev_delim = l;
    }
}

/* parseContext.RemoveDelimiter */
static void remove_delimiter(MarkCtx *ctx, MarkNode *d)
{
    if (d->prev_delim == NULL) {
        ctx->delimiters = d->next_delim;
    } else {
        d->prev_delim->next_delim = d->next_delim;
        if (d->next_delim != NULL) d->next_delim->prev_delim = d->prev_delim;
    }
    if (d->next_delim == NULL) ctx->last_delimiter = d->prev_delim;
    if (ctx->delimiters != NULL) ctx->delimiters->prev_delim = NULL;
    if (ctx->last_delimiter != NULL) ctx->last_delimiter->next_delim = NULL;
    d->next_delim = NULL;
    d->prev_delim = NULL;
    if (d->parent == NULL) return; /* Go would dereference nil */
    if (d->length != 0) {
        if (!mark_merge_or_replace_text(ctx->arena, d->parent, d, d->segment)) ctx->failed = 1;
    } else {
        mark_node_remove_child(d->parent, d);
    }
}

/* parseContext.ClearDelimiters: walks the siblings back from the last
 * delimiter to bottom. */
static void clear_delimiters(MarkCtx *ctx, MarkNode *bottom)
{
    MarkNode *c;

    if (ctx->last_delimiter == NULL) return;
    for (c = ctx->last_delimiter; c != NULL && c != bottom;) {
        MarkNode *prev = c->prev;

        if (c->kind == MARK_DELIMITER) remove_delimiter(ctx, c);
        c = prev;
    }
}

/* Delimiter.ConsumeCharacters */
static void consume_characters(MarkNode *d, long n)
{
    d->length -= n;
    d->segment = mark_segment_with_stop(d->segment, d->segment.start + d->length);
}

/* Delimiter.CalcComsumption */
static long calc_consumption(const MarkNode *d, const MarkNode *closer)
{
    if ((d->can_close || closer->can_open) && (d->original_length + closer->original_length) % 3 == 0 &&
        closer->original_length % 3 != 0) {
        return 0;
    }
    if (d->length >= 2 && closer->length >= 2) return 2;
    return 1;
}

/* The rune at line index j: util.ToRune(line, j) over a padded view. */
static unsigned long line_rune_at(MarkLine line, long j)
{
    if (j < line.pad) return ' ';
    return mark_to_rune(line.bytes, line.n, j - line.pad);
}

/* The flag computation of ScanDelimiter: the run of line[0], its left and
 * right flanking, and the canOpen/canClose rules ('_' is stricter). */
long mark_scan_delimiter_flags(MarkLine line, unsigned long before, int *can_open, int *can_close)
{
    long len = mark_line_len(line);
    unsigned char c = mark_line_at(line, 0);
    long j = 0;
    unsigned long after = ' ';
    int before_is_punct;
    int before_is_space;
    int after_is_punct;
    int after_is_space;
    int is_left;
    int is_right;

    for (; j < len && c == mark_line_at(line, j); j++) {
    }
    if (j != len) after = line_rune_at(line, j);
    before_is_punct = mark_rune_punct(before);
    before_is_space = mark_rune_space(before);
    after_is_punct = mark_rune_punct(after);
    after_is_space = mark_rune_space(after);
    is_left = !after_is_space && (!after_is_punct || before_is_space || before_is_punct);
    is_right = !before_is_space && (!before_is_punct || after_is_space || after_is_punct);
    if (c == '_') {
        *can_open = is_left && (!is_right || before_is_punct);
        *can_close = is_right && (!is_left || after_is_punct);
    } else {
        *can_open = is_left;
        *can_close = is_right;
    }
    return j;
}

/* ScanDelimiter with the emphasis processor (IsDelimiter: '*' or '_')
 * and min 1. */
static MarkNode *scan_delimiter(MarkCtx *ctx, MarkLine line, unsigned long before)
{
    unsigned char c = mark_line_at(line, 0);
    long j;
    int can_open;
    int can_close;
    MarkNode *d;

    if (c != '*' && c != '_') return NULL;
    j = mark_scan_delimiter_flags(line, before, &can_open, &can_close);
    if (j < 1) return NULL;
    d = mark_node_new(ctx->arena, MARK_DELIMITER);
    if (d == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    d->can_open = can_open;
    d->can_close = can_close;
    d->length = j;
    d->original_length = j;
    d->delim_char = c;
    return d;
}

/* ProcessDelimiters */
void mark_process_delimiters(MarkCtx *ctx, MarkNode *bottom, int bottom_set)
{
    MarkNode *last_delimiter = ctx->last_delimiter;
    MarkNode *closer = NULL;

    if (last_delimiter == NULL) return;
    if (bottom_set) {
        if (bottom != last_delimiter) {
            MarkNode *c;

            for (c = last_delimiter->prev; c != NULL && c != bottom;) {
                if (c->kind == MARK_DELIMITER) closer = c;
                c = c->prev;
            }
        }
    } else {
        closer = ctx->delimiters;
    }
    if (closer == NULL) {
        clear_delimiters(ctx, bottom);
        return;
    }
    while (closer != NULL) {
        long consume = 0;
        int found = 0;
        int maybe_opener = 0;
        MarkNode *opener;
        MarkNode *node;
        MarkNode *parent;
        MarkNode *child;
        MarkNode *c;

        if (ctx->failed) return;
        if (!closer->can_close) {
            closer = closer->next_delim;
            continue;
        }
        /* Both processors' CanOpenCloser compare the characters, so the
         * processor is implied by the character ('~' is strikethrough). */
        for (opener = closer->prev_delim; opener != NULL && opener != bottom; opener = opener->prev_delim) {
            if (opener->can_open && opener->delim_char == closer->delim_char) {
                maybe_opener = 1;
                consume = calc_consumption(opener, closer);
                if (consume > 0) {
                    found = 1;
                    break;
                }
            }
        }
        if (!found) {
            MarkNode *next = closer->next_delim;

            if (!maybe_opener && !closer->can_open) remove_delimiter(ctx, closer);
            closer = next;
            continue;
        }
        consume_characters(opener, consume);
        consume_characters(closer, consume);

        /* emphasisDelimiterProcessor.OnMatch, or
         * strikethroughDelimiterProcessor.OnMatch for '~' */
        node = mark_node_new(ctx->arena, opener->delim_char == '~' ? MARK_STRIKETHROUGH : MARK_EMPHASIS);
        if (node == NULL) {
            ctx->failed = 1;
            return;
        }
        node->level = consume;

        parent = opener->parent;
        child = opener->next;
        for (; child != NULL && child != closer;) {
            MarkNode *next = child->next;

            mark_node_append_child(node, child);
            child = next;
        }
        if (parent != NULL) mark_node_insert_after(parent, opener, node);

        for (c = opener->next_delim; c != NULL && c != closer;) {
            MarkNode *next = c->next_delim;

            remove_delimiter(ctx, c);
            c = next;
        }

        if (opener->length == 0) remove_delimiter(ctx, opener);

        if (closer->length == 0) {
            MarkNode *next = closer->next_delim;

            remove_delimiter(ctx, closer);
            closer = next;
        }
    }
    clear_delimiters(ctx, bottom);
}

/* ---- emphasis.go ---- */

/* emphasisParser.Parse */
static MarkNode *emphasis_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block)
{
    unsigned long before = mark_reader_preceding(block);
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(block, &segment);
    MarkNode *node;

    (void)parent;
    node = scan_delimiter(ctx, line, before);
    if (node == NULL) return NULL;
    node->segment = mark_segment_with_stop(segment, segment.start + node->original_length);
    mark_reader_advance(block, node->original_length);
    mark_push_delimiter(ctx, node);
    return node;
}

/* ---- code_span.go ---- */

static int is_space_or_newline(unsigned char c)
{
    return c == ' ' || c == '\n';
}

/* CodeSpan.IsBlank */
static int code_span_is_blank(const MarkCtx *ctx, const MarkNode *n)
{
    const MarkNode *c;

    for (c = n->first; c != NULL; c = c->next) {
        MarkLine v = mark_segment_value(c->segment, ctx->src, ctx->src_cap);

        if (!mark_line_is_blank(v)) return 0;
    }
    return 1;
}

static unsigned char source_at(const MarkCtx *ctx, long i)
{
    if (i < 0 || i >= ctx->src_len) return 0;
    return ctx->src[i];
}

/* codeSpanParser.Parse */
static MarkNode *code_span_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block)
{
    MarkSegment start_segment;
    MarkLine line = mark_reader_peek_line(block, &start_segment);
    long opener = 0;
    long l;
    MarkSegment pos;
    MarkNode *node;

    (void)parent;
    for (; opener < mark_line_len(line) && mark_line_at(line, opener) == '`'; opener++) {
    }
    mark_reader_advance(block, opener);
    mark_reader_position(block, &l, &pos);
    node = mark_node_new(ctx->arena, MARK_CODE_SPAN);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    for (;;) {
        MarkSegment segment;
        long len;
        long i;
        MarkNode *t;

        if (ctx->failed) return NULL;
        line = mark_reader_peek_line(block, &segment);
        if (!line.valid) {
            mark_reader_set_position(block, l, pos);
            t = mark_text_new(ctx->arena, mark_segment_with_stop(start_segment, start_segment.start + opener));
            if (t == NULL) ctx->failed = 1;
            return t;
        }
        len = mark_line_len(line);
        for (i = 0; i < len; i++) {
            if (mark_line_at(line, i) == '`') {
                long oldi = i;
                long closure;

                for (; i < len && mark_line_at(line, i) == '`'; i++) {
                }
                closure = i - oldi;
                if (closure == opener && (i >= len || mark_line_at(line, i) != '`')) {
                    segment = mark_segment_with_stop(segment, segment.start + i - closure);
                    if (!mark_segment_is_empty(segment)) {
                        t = mark_raw_text_new(ctx->arena, segment);
                        if (t == NULL) {
                            ctx->failed = 1;
                            return NULL;
                        }
                        mark_node_append_child(node, t);
                    }
                    mark_reader_advance(block, i);
                    goto end;
                }
            }
        }
        t = mark_raw_text_new(ctx->arena, segment);
        if (t == NULL) {
            ctx->failed = 1;
            return NULL;
        }
        mark_node_append_child(node, t);
        mark_reader_advance_line(block);
    }
end:
    if (!code_span_is_blank(ctx, node)) {
        /* trim one leading and one trailing space */
        MarkSegment segment = node->first->segment;
        int should_trim = 1;

        if (!(!mark_segment_is_empty(segment) && is_space_or_newline(source_at(ctx, segment.start)))) should_trim = 0;
        segment = node->last->segment;
        if (!(!mark_segment_is_empty(segment) && is_space_or_newline(source_at(ctx, segment.stop - 1)))) should_trim = 0;
        if (should_trim) {
            MarkNode *t = node->first;

            t->segment = mark_segment_with_start(t->segment, t->segment.start + 1);
            t = node->last;
            t->segment = mark_segment_with_stop(t->segment, t->segment.stop - 1);
        }
    }
    return node;
}

/* ---- auto_link.go ---- */

/* autoLinkParser.Parse */
static MarkNode *autolink_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block)
{
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(block, &segment);
    CuStr rest = line_bytes(ctx, mark_line_skip(line, 1));
    long stop = mark_find_email_index((const unsigned char *)rest.ptr, (long)rest.len);
    int typ = MARK_AUTOLINK_EMAIL;
    MarkNode *value;
    MarkNode *node;

    (void)parent;
    if (stop < 0) {
        stop = mark_find_url_index((const unsigned char *)rest.ptr, (long)rest.len);
        typ = MARK_AUTOLINK_URL;
    }
    if (stop < 0) return NULL;
    stop++;
    if (stop >= mark_line_len(line) || mark_line_at(line, stop) != '>') return NULL;
    value = mark_text_new(ctx->arena, mark_segment(segment.start + 1, segment.start + stop));
    node = mark_node_new(ctx->arena, MARK_AUTO_LINK);
    if (value == NULL || node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    mark_reader_advance(block, stop + 1);
    node->autolink_type = typ;
    node->autolink_value = value;
    return node;
}

/* ---- raw_html.go ---- */

/* readRuneReader: the next rune, or 0 at the end or at invalid UTF-8
 * (which stops the regexp's input in Go). Does not advance. */
static int peek_rune(MarkReader *r, unsigned long *rune, long *size)
{
    MarkSegment seg;
    MarkLine line = mark_reader_peek_line(r, &seg);

    if (!line.valid) return 0;
    if (line.pad > 0) {
        *rune = ' ';
        *size = 1;
        return 1;
    }
    *rune = mark_decode_rune(line.bytes, line.n, size);
    return *rune != MARK_RUNE_ERROR;
}

/* A cursor over the reader's rune stream for the matchers; consumed
 * counts bytes so the match can be re-applied with Advance. */
typedef struct RuneCursor {
    MarkReader *r;
    long consumed;
} RuneCursor;

typedef struct RuneMark {
    long line;
    MarkSegment pos;
    long consumed;
} RuneMark;

static RuneMark rc_mark(const RuneCursor *rc)
{
    RuneMark m;

    mark_reader_position(rc->r, &m.line, &m.pos);
    m.consumed = rc->consumed;
    return m;
}

static void rc_reset(RuneCursor *rc, RuneMark m)
{
    mark_reader_set_position(rc->r, m.line, m.pos);
    rc->consumed = m.consumed;
}

static int rc_peek(RuneCursor *rc, unsigned long *rune)
{
    long size;

    return peek_rune(rc->r, rune, &size);
}

static void rc_take(RuneCursor *rc)
{
    unsigned long rune;
    long size;

    if (!peek_rune(rc->r, &rune, &size)) return;
    mark_reader_advance(rc->r, size);
    rc->consumed += size;
}

/* Consumes c if it is next. */
static int rc_accept(RuneCursor *rc, unsigned long c)
{
    unsigned long rune;

    if (!rc_peek(rc, &rune) || rune != c) return 0;
    rc_take(rc);
    return 1;
}

static int is_alpha_rune(unsigned long r)
{
    return (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z');
}

static int is_digit_rune(unsigned long r)
{
    return r >= '0' && r <= '9';
}

/* tagnamePattern ([A-Za-z][A-Za-z0-9-]*) */
static int rc_tag_name(RuneCursor *rc)
{
    unsigned long r;

    if (!rc_peek(rc, &r) || !is_alpha_rune(r)) return 0;
    rc_take(rc);
    while (rc_peek(rc, &r) && (is_alpha_rune(r) || is_digit_rune(r) || r == '-')) rc_take(rc);
    return 1;
}

/* spaceOrOneNewline* : (?:[ \t]|(?:\r\n|\n){0,1})* */
static void rc_space_or_newlines(RuneCursor *rc)
{
    unsigned long r;

    for (;;) {
        if (!rc_peek(rc, &r)) return;
        if (r == ' ' || r == '\t' || r == '\n') {
            rc_take(rc);
        } else if (r == '\r') {
            RuneMark m = rc_mark(rc);

            rc_take(rc);
            if (!rc_accept(rc, '\n')) {
                rc_reset(rc, m);
                return;
            }
        } else {
            return;
        }
    }
}

static int attr_ws_rune(unsigned long r)
{
    return r == '\r' || r == '\n' || r == ' ' || r == '\t';
}

static int attr_name_start_rune(unsigned long r)
{
    return is_alpha_rune(r) || r == '_' || r == ':';
}

static int attr_name_rune(unsigned long r)
{
    return attr_name_start_rune(r) || is_digit_rune(r) || r == '.' || r == '-';
}

/* One attributePattern:
 * (?:[\r\n \t]+[a-zA-Z_:][a-zA-Z0-9:._-]*(?:[\r\n \t]*=[\r\n \t]*(?:[^"'=<>`\x00-\x20]+|'[^']*'|"[^"]*"))?)
 * The value is optional: when it does not match the cursor goes back to
 * the end of the name, as the regexp's backtracking would. */
static int rc_attribute(RuneCursor *rc)
{
    unsigned long r;
    RuneMark start = rc_mark(rc);
    RuneMark after_name;
    int ws = 0;

    while (rc_peek(rc, &r) && attr_ws_rune(r)) {
        rc_take(rc);
        ws = 1;
    }
    if (!ws || !rc_peek(rc, &r) || !attr_name_start_rune(r)) {
        rc_reset(rc, start);
        return 0;
    }
    rc_take(rc);
    while (rc_peek(rc, &r) && attr_name_rune(r)) rc_take(rc);
    after_name = rc_mark(rc);
    while (rc_peek(rc, &r) && attr_ws_rune(r)) rc_take(rc);
    if (!rc_accept(rc, '=')) {
        rc_reset(rc, after_name);
        return 1;
    }
    while (rc_peek(rc, &r) && attr_ws_rune(r)) rc_take(rc);
    if (!rc_peek(rc, &r)) {
        rc_reset(rc, after_name);
        return 1;
    }
    if (r == '\'' || r == '"') {
        unsigned long quote = r;

        rc_take(rc);
        while (rc_peek(rc, &r) && r != quote) rc_take(rc);
        if (!rc_accept(rc, quote)) rc_reset(rc, after_name);
        return 1;
    }
    {
        int any = 0;

        while (rc_peek(rc, &r) && r != '"' && r != '\'' && r != '=' && r != '<' && r != '>' && r != '`' && r > 0x20) {
            rc_take(rc);
            any = 1;
        }
        if (!any) rc_reset(rc, after_name);
    }
    return 1;
}

/* openTagRegexp: ^<TAG ATTR* spaceOrOneNewline* /?> ; returns the match
 * length in bytes, or -1. The reader is left where it was. */
static long match_open_tag(MarkReader *r)
{
    RuneCursor rc;
    RuneMark start;
    long n = -1;

    rc.r = r;
    rc.consumed = 0;
    start = rc_mark(&rc);
    if (rc_accept(&rc, '<') && rc_tag_name(&rc)) {
        while (rc_attribute(&rc)) {
        }
        rc_space_or_newlines(&rc);
        rc_accept(&rc, '/');
        if (rc_accept(&rc, '>')) n = rc.consumed;
    }
    rc_reset(&rc, start);
    return n;
}

/* closeTagRegexp: ^</TAG spaceOrOneNewline*> */
static long match_close_tag(MarkReader *r)
{
    RuneCursor rc;
    RuneMark start;
    long n = -1;

    rc.r = r;
    rc.consumed = 0;
    start = rc_mark(&rc);
    if (rc_accept(&rc, '<') && rc_accept(&rc, '/') && rc_tag_name(&rc)) {
        rc_space_or_newlines(&rc);
        if (rc_accept(&rc, '>')) n = rc.consumed;
    }
    rc_reset(&rc, start);
    return n;
}

static MarkNode *raw_html_new(MarkCtx *ctx)
{
    MarkNode *node = mark_node_new(ctx->arena, MARK_RAW_HTML);

    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    node->segments = mark_segments_new(ctx->arena);
    if (node->segments == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    return node;
}

static void segments_append(MarkCtx *ctx, MarkNode *node, MarkSegment s)
{
    mark_lines_append(ctx, node->segments, s);
}

/* bytes.HasPrefix(line, lit) */
static int line_has_prefix(MarkLine line, const char *lit)
{
    size_t k;

    for (k = 0; lit[k] != '\0'; k++) {
        if (mark_line_at(line, (long)k) != (unsigned char)lit[k]) return 0;
    }
    return 1;
}

/* rawHTMLParser.parseComment */
static MarkNode *parse_comment(MarkCtx *ctx, MarkReader *block)
{
    long saved_line;
    MarkSegment saved_segment;
    MarkNode *node;
    MarkSegment segment;
    MarkLine line;
    long offset;

    mark_reader_position(block, &saved_line, &saved_segment);
    node = raw_html_new(ctx);
    if (node == NULL) return NULL;
    line = mark_reader_peek_line(block, &segment);
    if (line_has_prefix(line, "<!---->")) {
        segments_append(ctx, node, mark_segment_with_stop(segment, segment.start + 7));
        mark_reader_advance(block, 7);
        return node;
    }
    if (line_has_prefix(line, "<!-->") || line_has_prefix(line, "<!--->")) return NULL;
    offset = 4;
    line = mark_line_skip(line, offset);
    for (;;) {
        long hindex = mark_line_index(line, CU_LIT("--"));
        long index;

        if (ctx->failed) return NULL;
        if (hindex > -1) hindex += offset;
        index = mark_line_index(line, CU_LIT("-->")) + offset;
        if (index > -1 && hindex == index) {
            if (index == 0 || mark_line_len(line) < 2 || mark_line_at(line, index - offset - 1) != '-') {
                segments_append(ctx, node, mark_segment_with_stop(segment, segment.start + index + 3));
                mark_reader_advance(block, index + 3);
                return node;
            }
        }
        if (hindex > 0) break;
        segments_append(ctx, node, segment);
        mark_reader_advance_line(block);
        line = mark_reader_peek_line(block, &segment);
        offset = 0;
        if (!line.valid) break;
    }
    mark_reader_set_position(block, saved_line, saved_segment);
    return NULL;
}

/* rawHTMLParser.parseUntil */
static MarkNode *parse_until(MarkCtx *ctx, MarkReader *block, CuStr closer)
{
    long saved_line;
    MarkSegment saved_segment;
    MarkNode *node;

    mark_reader_position(block, &saved_line, &saved_segment);
    node = raw_html_new(ctx);
    if (node == NULL) return NULL;
    for (;;) {
        MarkSegment segment;
        MarkLine line = mark_reader_peek_line(block, &segment);
        long index;

        if (ctx->failed) return NULL;
        if (!line.valid) break;
        index = mark_line_index(line, closer);
        if (index > -1) {
            segments_append(ctx, node, mark_segment_with_stop(segment, segment.start + index + (long)closer.len));
            mark_reader_advance(block, index + (long)closer.len);
            return node;
        }
        segments_append(ctx, node, segment);
        mark_reader_advance_line(block);
    }
    mark_reader_set_position(block, saved_line, saved_segment);
    return NULL;
}

/* rawHTMLParser.parseMultiLineRegexp; match is match_open_tag or
 * match_close_tag (text.Reader.Match: find, reset, Advance the length). */
static MarkNode *parse_multi_line_regexp(MarkCtx *ctx, long (*match)(MarkReader *), MarkReader *block)
{
    long sline;
    MarkSegment ssegment;
    long n;
    MarkNode *node;
    long eline;
    MarkSegment esegment;

    mark_reader_position(block, &sline, &ssegment);
    n = match(block);
    if (n < 0) return NULL;
    mark_reader_advance(block, n);
    node = raw_html_new(ctx);
    if (node == NULL) return NULL;
    mark_reader_position(block, &eline, &esegment);
    mark_reader_set_position(block, sline, ssegment);
    for (;;) {
        MarkSegment segment;
        MarkLine line = mark_reader_peek_line(block, &segment);
        long l;
        MarkSegment p;
        long start;
        long end;

        if (ctx->failed) return NULL;
        if (!line.valid) break;
        mark_reader_position(block, &l, &p);
        start = segment.start;
        if (l == sline) start = ssegment.start;
        end = segment.stop;
        if (l == eline) end = esegment.start;
        segments_append(ctx, node, mark_segment(start, end));
        if (l == eline) {
            mark_reader_advance(block, end - start);
            break;
        }
        mark_reader_advance_line(block);
    }
    return node;
}

/* rawHTMLParser.Parse */
static MarkNode *raw_html_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block)
{
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(block, &segment);
    long len = mark_line_len(line);

    (void)parent;
    if (len > 1 && mark_byte_alnum(mark_line_at(line, 1))) return parse_multi_line_regexp(ctx, match_open_tag, block);
    if (len > 2 && mark_line_at(line, 1) == '/' && mark_byte_alnum(mark_line_at(line, 2))) {
        return parse_multi_line_regexp(ctx, match_close_tag, block);
    }
    if (line_has_prefix(line, "<!--")) return parse_comment(ctx, block);
    if (line_has_prefix(line, "<?")) return parse_until(ctx, block, CU_LIT("?>"));
    if (len > 2 && mark_line_at(line, 1) == '!' && mark_line_at(line, 2) >= 'A' && mark_line_at(line, 2) <= 'Z') {
        return parse_until(ctx, block, CU_LIT(">"));
    }
    if (line_has_prefix(line, "<![CDATA[")) return parse_until(ctx, block, CU_LIT("]]>"));
    return NULL;
}

/* ---- link.go ---- */

/* linkLabelStateLength */
static long link_label_state_length(const MarkNode *v)
{
    if (v == NULL || v->ll_last == NULL || v->ll_first == NULL) return 0;
    return v->ll_last->segment.stop - v->ll_first->segment.start;
}

/* pushLinkLabelState */
static void push_link_label_state(MarkCtx *ctx, MarkNode *v)
{
    MarkNode *list = ctx->link_label_state;

    if (list == NULL) {
        v->ll_first = v;
        v->ll_last = v;
        ctx->link_label_state = v;
    } else {
        MarkNode *l = list->ll_last;

        list->ll_last = v;
        l->ll_next = v;
        v->ll_prev = l;
    }
}

/* removeLinkLabelState. The new head's First is set to the removed node,
 * as in Go; linkLabelStateLength then reads a detached node's segment. */
static void remove_link_label_state(MarkCtx *ctx, MarkNode *d)
{
    MarkNode *list = ctx->link_label_state;

    if (list == NULL) return;
    if (d->ll_prev == NULL) {
        list = d->ll_next;
        if (list != NULL) {
            list->ll_first = d;
            list->ll_last = d->ll_last;
            list->ll_prev = NULL;
            ctx->link_label_state = list;
        } else {
            ctx->link_label_state = NULL;
        }
    } else {
        d->ll_prev->ll_next = d->ll_next;
        if (d->ll_next != NULL) d->ll_next->ll_prev = d->ll_prev;
    }
    if (list != NULL && d->ll_next == NULL) list->ll_last = d->ll_prev;
    d->ll_next = NULL;
    d->ll_prev = NULL;
    d->ll_first = NULL;
    d->ll_last = NULL;
}

/* processLinkLabelOpen */
static MarkNode *process_link_label_open(MarkCtx *ctx, MarkReader *block, long pos, int is_image)
{
    long start = pos;
    MarkNode *state;

    if (is_image) start--;
    state = mark_node_new(ctx->arena, MARK_LINK_LABEL_STATE);
    if (state == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    state->segment = mark_segment(start, pos + 1);
    state->is_image = is_image;
    push_link_label_state(ctx, state);
    mark_reader_advance(block, 1);
    return state;
}

/* Whether root or any node under it is a Link (one step of
 * linkParser.containsLink, which recurses; this walks iteratively). */
static int subtree_contains_link(const MarkNode *root)
{
    const MarkNode *n = root;

    for (;;) {
        if (n->kind == MARK_LINK) return 1;
        if (n->first != NULL) {
            n = n->first;
            continue;
        }
        while (n != root && n->next == NULL) n = n->parent;
        if (n == root) return 0;
        n = n->next;
    }
}

/* linkParser.containsLink(n): n and its following siblings, deeply. */
static int contains_link(const MarkNode *n)
{
    for (; n != NULL; n = n->next) {
        if (subtree_contains_link(n)) return 1;
    }
    return 0;
}

/* linkParser.processLinkLabel */
static void process_link_label(MarkCtx *ctx, MarkNode *parent, MarkNode *link, MarkNode *last)
{
    MarkNode *bottom = ctx->link_bottom;
    int bottom_set = ctx->link_bottom_set;
    MarkNode *c;

    ctx->link_bottom = NULL;
    ctx->link_bottom_set = 0;
    mark_process_delimiters(ctx, bottom, bottom_set);
    for (c = last->next; c != NULL;) {
        MarkNode *next = c->next;

        mark_node_remove_child(parent, c);
        mark_node_append_child(link, c);
        c = next;
    }
}

static MarkClosureOptions link_closure_options(void)
{
    MarkClosureOptions o;

    o.code_span = 0;
    o.nesting = 0;
    o.newline = 1;
    o.advance = 1;
    return o;
}

/* The bytes of FindClosure's segments joined (link.go's title and
 * reference loops). */
static CuStr join_segments(MarkCtx *ctx, MarkReader *block, const CuVec *segments)
{
    size_t n = cu_vec_len(segments);
    CuBuilder b;
    CuStr out;
    size_t i;

    if (n == 1) return mark_reader_value(block, ctx->arena, *CU_VEC_AT(segments, MarkSegment, 0));
    cu_sb_init(&b, ctx->arena);
    for (i = 0; i < n; i++) {
        cu_sb_str(&b, mark_reader_value(block, ctx->arena, *CU_VEC_AT(segments, MarkSegment, i)));
    }
    if (!cu_sb_finish(&b, &out)) {
        ctx->failed = 1;
        return cu_str_empty();
    }
    return out;
}

/* parseLinkDestination */
int mark_parse_link_destination(MarkCtx *ctx, MarkReader *block, CuStr *out)
{
    MarkSegment seg;
    long chars;
    MarkLine line;
    long len;
    long opened = 0;
    long i = 0;

    mark_reader_skip_spaces(block, &seg, &chars);
    line = mark_reader_peek_line(block, &seg);
    len = mark_line_len(line);
    if (mark_reader_peek(block) == '<') {
        i = 1;
        while (i < len) {
            unsigned char c = mark_line_at(line, i);

            if (c == '\\' && i < len - 1 && mark_byte_punct(mark_line_at(line, i + 1))) {
                i += 2;
                continue;
            } else if (c == '>') {
                mark_reader_advance(block, i + 1);
                *out = mark_line_slice(ctx->arena, line, 1, i);
                return 1;
            }
            i++;
        }
        return 0;
    }
    while (i < len) {
        unsigned char c = mark_line_at(line, i);

        if (c == '\\' && i < len - 1 && mark_byte_punct(mark_line_at(line, i + 1))) {
            i += 2;
            continue;
        } else if (c == '(') {
            opened++;
        } else if (c == ')') {
            opened--;
            if (opened < 0) break;
        } else if (mark_byte_space(c)) {
            break;
        }
        i++;
    }
    mark_reader_advance(block, i);
    *out = mark_line_slice(ctx->arena, line, 0, i);
    return i != 0;
}

/* parseLinkTitle */
static int parse_link_title(MarkCtx *ctx, MarkReader *block, CuStr *out)
{
    MarkSegment seg;
    long chars;
    unsigned char opener;
    unsigned char closer;
    CuVec segments;

    mark_reader_skip_spaces(block, &seg, &chars);
    opener = mark_reader_peek(block);
    if (opener != '"' && opener != '\'' && opener != '(') return 0;
    closer = opener == '(' ? ')' : opener;
    mark_reader_advance(block, 1);
    if (mark_reader_find_closure(block, ctx->arena, opener, closer, link_closure_options(), &segments)) {
        *out = join_segments(ctx, block, &segments);
        return 1;
    }
    if (cu_vec_failed(&segments)) ctx->failed = 1;
    return 0;
}

static MarkNode *link_new(MarkCtx *ctx)
{
    MarkNode *link = mark_node_new(ctx->arena, MARK_LINK);

    if (link == NULL) ctx->failed = 1;
    return link;
}

/* linkParser.parseReferenceLink: *has_value is Go's second result */
static MarkNode *parse_reference_link(MarkCtx *ctx, MarkNode *parent, MarkNode *last, MarkReader *block, int *has_value)
{
    long l;
    MarkSegment orgpos;
    CuVec segments;
    CuStr maybe_reference;
    CuBuilder kb;
    CuStr key;
    const MarkReference *ref;
    MarkNode *link;

    *has_value = 0;
    mark_reader_position(block, &l, &orgpos);
    mark_reader_advance(block, 1); /* skip '[' */
    if (!mark_reader_find_closure(block, ctx->arena, '[', ']', link_closure_options(), &segments)) {
        if (cu_vec_failed(&segments)) ctx->failed = 1;
        return NULL;
    }
    maybe_reference = join_segments(ctx, block, &segments);
    if (mark_is_blank((const unsigned char *)maybe_reference.ptr, (long)maybe_reference.len)) { /* collapsed */
        maybe_reference = mark_reader_value(block, ctx->arena, mark_segment(last->segment.stop, orgpos.start - 1));
    }
    *has_value = 1;
    /* a link label has at most 999 characters */
    if (maybe_reference.len > 999) return NULL;
    cu_sb_init(&kb, ctx->arena);
    mark_to_link_reference(&kb, maybe_reference);
    if (!cu_sb_finish(&kb, &key)) {
        ctx->failed = 1;
        return NULL;
    }
    ref = mark_reference(ctx, key);
    if (ref == NULL) return NULL;
    link = link_new(ctx);
    if (link == NULL) return NULL;
    process_link_label(ctx, parent, link, last);
    link->title = ref->title;
    link->has_title = ref->has_title;
    link->destination = ref->destination;
    return link;
}

/* linkParser.parseLink */
static MarkNode *parse_link(MarkCtx *ctx, MarkNode *parent, MarkNode *last, MarkReader *block)
{
    MarkSegment seg;
    long chars;
    CuStr title = cu_str_empty();
    int has_title = 0;
    CuStr destination = cu_str_empty();
    MarkNode *link;

    mark_reader_advance(block, 1); /* skip '(' */
    mark_reader_skip_spaces(block, &seg, &chars);
    if (mark_reader_peek(block) == ')') { /* an empty link like '[link]()' */
        mark_reader_advance(block, 1);
    } else {
        if (!mark_parse_link_destination(ctx, block, &destination)) return NULL;
        mark_reader_skip_spaces(block, &seg, &chars);
        if (mark_reader_peek(block) == ')') {
            mark_reader_advance(block, 1);
        } else {
            if (!parse_link_title(ctx, block, &title)) return NULL;
            has_title = 1;
            mark_reader_skip_spaces(block, &seg, &chars);
            if (mark_reader_peek(block) == ')') mark_reader_advance(block, 1);
            else return NULL;
        }
    }
    link = link_new(ctx);
    if (link == NULL) return NULL;
    process_link_label(ctx, parent, link, last);
    link->destination = destination;
    link->title = title;
    link->has_title = has_title;
    return link;
}

/* Replaces the label state by its text (ast.MergeOrReplaceTextSegment on
 * it), the way linkParser.Parse gives up on a label. */
static MarkNode *label_to_text(MarkCtx *ctx, MarkNode *last)
{
    if (last->parent != NULL && !mark_merge_or_replace_text(ctx->arena, last->parent, last, last->segment)) {
        ctx->failed = 1;
    }
    return NULL;
}

/* linkParser.Parse */
static MarkNode *link_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block)
{
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(block, &segment);
    unsigned char c0 = mark_line_at(line, 0);
    MarkNode *tlist;
    MarkNode *last;
    unsigned char c;
    long l;
    MarkSegment pos;
    MarkNode *link = NULL;

    if (c0 == '!') {
        if (mark_line_len(line) > 1 && mark_line_at(line, 1) == '[') {
            mark_reader_advance(block, 1);
            ctx->link_bottom = ctx->last_delimiter;
            ctx->link_bottom_set = 1;
            return process_link_label_open(ctx, block, segment.start + 1, 1);
        }
        return NULL;
    }
    if (c0 == '[') {
        ctx->link_bottom = ctx->last_delimiter;
        ctx->link_bottom_set = 1;
        return process_link_label_open(ctx, block, segment.start, 0);
    }

    /* c0 == ']' */
    tlist = ctx->link_label_state;
    if (tlist == NULL) return NULL;
    last = tlist->ll_last;
    if (last == NULL) return NULL;
    mark_reader_advance(block, 1);
    remove_link_label_state(ctx, last);
    /* a link label has at most 999 characters */
    if (link_label_state_length(tlist) > 998) return label_to_text(ctx, last);

    if (!last->is_image && contains_link(last)) return label_to_text(ctx, last); /* no link in link text */

    c = mark_reader_peek(block);
    mark_reader_position(block, &l, &pos);
    if (c == '(') { /* an inline link */
        link = parse_link(ctx, parent, last, block);
    } else if (c == '[') { /* a reference link */
        int has_value;

        link = parse_reference_link(ctx, parent, last, block, &has_value);
        if (link == NULL && has_value) return label_to_text(ctx, last);
    }
    if (ctx->failed) return NULL;

    if (link == NULL) {
        /* maybe a shortcut reference link */
        CuStr maybe_reference;
        CuBuilder kb;
        CuStr key;
        const MarkReference *ref;

        mark_reader_set_position(block, l, pos);
        maybe_reference = mark_reader_value(block, ctx->arena, mark_segment(last->segment.stop, segment.start));
        if (maybe_reference.len > 999) return label_to_text(ctx, last);
        cu_sb_init(&kb, ctx->arena);
        mark_to_link_reference(&kb, maybe_reference);
        if (!cu_sb_finish(&kb, &key)) {
            ctx->failed = 1;
            return NULL;
        }
        ref = mark_reference(ctx, key);
        if (ref == NULL) return label_to_text(ctx, last);
        link = link_new(ctx);
        if (link == NULL) return NULL;
        process_link_label(ctx, parent, link, last);
        link->title = ref->title;
        link->has_title = ref->has_title;
        link->destination = ref->destination;
    }
    if (last->parent != NULL) mark_node_remove_child(last->parent, last);
    if (last->is_image) {
        MarkNode *image = mark_image_new(ctx->arena, link);

        if (image == NULL) ctx->failed = 1;
        return image;
    }
    return link;
}

/* linkParser.CloseBlock */
void mark_link_close_block(MarkCtx *ctx, MarkNode *parent, MarkReader *block)
{
    MarkNode *s;

    (void)parent;
    (void)block;
    ctx->link_bottom = NULL;
    ctx->link_bottom_set = 0;
    for (s = ctx->link_label_state; s != NULL;) {
        MarkNode *next = s->ll_next;
        MarkNode *t;

        remove_link_label_state(ctx, s);
        t = mark_text_new(ctx->arena, s->segment);
        if (t == NULL) {
            ctx->failed = 1;
            return;
        }
        if (s->parent != NULL) mark_node_replace_child(s->parent, s, t);
        s = next;
    }
}

/* ---- parser.DefaultInlineParsers and the extensions' by trigger ---- */

typedef struct InlineParserEntry {
    MarkInlineParse parse;
    const char *trigger;
    unsigned long flag; /* the CU_MARK_* option that registers it; 0: core */
} InlineParserEntry;

/* In priority order: task list 0, code span 100, footnote 101, link 200,
 * auto link 300, raw HTML 400, emphasis 500, strikethrough 500 (the tie
 * does not matter: the two share no trigger), linkify 999, typographer
 * 9999. The linkify trigger ' ' stands for any white space and a line
 * head, as the driver maps them. */
static const InlineParserEntry inline_parsers[] = {
    {mark_task_checkbox_parse, "[", CU_MARK_TASKLIST},
    {code_span_parse, "`", 0},
    {mark_footnote_parse, "![", CU_MARK_FOOTNOTE},
    {link_parse, "![]", 0},
    {autolink_parse, "<", 0},
    {raw_html_parse, "<", 0},
    {emphasis_parse, "*_", 0},
    {mark_strikethrough_parse, "~", CU_MARK_STRIKETHROUGH},
    {mark_linkify_parse, " *_~(", CU_MARK_LINKIFY},
    {mark_typographer_parse, "'\"-.,<>*[", CU_MARK_TYPOGRAPHER},
};

size_t mark_inline_parsers_for(const MarkCtx *ctx, unsigned char c, MarkInlineParse out[MARK_INLINE_PARSERS_MAX])
{
    size_t n = 0;
    size_t i;

    if (c == 0) return 0;
    for (i = 0; i < sizeof inline_parsers / sizeof inline_parsers[0]; i++) {
        const InlineParserEntry *e = &inline_parsers[i];

        if (e->flag != 0 && (ctx->flags & e->flag) == 0) continue;
        if (strchr(e->trigger, (int)c) == NULL) continue;
        if (n < MARK_INLINE_PARSERS_MAX) out[n++] = e->parse;
    }
    return n;
}
