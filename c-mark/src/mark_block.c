/* goldmark's block parsers as C: parser/setext_headings.go,
 * thematic_break.go, list.go, list_item.go, code_block.go, atx_heading.go,
 * fcode_block.go, blockquote.go, html_block.go, paragraph.go, plus
 * parser/attribute.go and parser/link_ref.go. Each function names the Go
 * function it ports. The regular expressions goldmark uses for HTML blocks
 * are written out as matchers; each one quotes the expression it stands
 * for. */
#include "mark_parser.h"
#include "mark_util.h"

static CuStr attr_name_id(void)
{
    return CU_LIT("id");
}

static CuStr attr_name_class(void)
{
    return CU_LIT("class");
}

static int option(const MarkCtx *ctx, unsigned long flag)
{
    return (ctx->flags & flag) != 0;
}

/* The source position of index i of a line peeked with segment seg:
 * seg.Start + i - seg.Padding, as the Go parsers compute it. */
static long line_pos(MarkSegment seg, long i)
{
    return seg.start + i - seg.padding;
}

/* ---- parser/attribute.go ---- */

static int parse_attributes(MarkCtx *ctx, MarkReader *r, CuVec *attrs, long depth);

/* The class values are strings (parseAttribute checks), so the merge of
 * Attributes.findUpdate is v + " " + attr.Value. */
static int merge_class(MarkCtx *ctx, CuVec *attrs, CuStr value)
{
    size_t i;

    for (i = 0; i < cu_vec_len(attrs); i++) {
        MarkAttr *a = CU_VEC_AT(attrs, MarkAttr, i);

        if (cu_str_eq(a->name, attr_name_class())) {
            CuBuilder b;
            CuStr merged;

            cu_sb_init(&b, ctx->arena);
            cu_sb_str(&b, a->value.str);
            cu_sb_char(&b, ' ');
            cu_sb_str(&b, value);
            if (!cu_sb_finish(&b, &merged)) ctx->failed = 1;
            a->value = mark_attr_string(merged);
            return 1;
        }
    }
    return 0;
}

static int name_start_byte(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
}

static int name_byte(unsigned char c)
{
    return name_start_byte(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

/* parseAttributeString */
static int parse_attribute_string(MarkCtx *ctx, MarkReader *r, CuStr *out)
{
    MarkSegment seg;
    MarkLine line;
    long i = 0;
    long l;
    CuBuilder buf;

    mark_reader_advance(r, 1); /* skip " */
    line = mark_reader_peek_line(r, &seg);
    l = mark_line_len(line);
    cu_sb_init(&buf, ctx->arena);
    while (i < l) {
        unsigned char c = mark_line_at(line, i);

        if (c == '\\' && i != l - 1) {
            unsigned char n = mark_line_at(line, i + 1);

            switch (n) {
            case '"':
            case '/':
            case '\\':
                cu_sb_char(&buf, (char)n);
                i += 2;
                break;
            case 'b':
                cu_sb_char(&buf, '\b');
                i += 2;
                break;
            case 'f':
                cu_sb_char(&buf, '\f');
                i += 2;
                break;
            case 'n':
                cu_sb_char(&buf, '\n');
                i += 2;
                break;
            case 'r':
                cu_sb_char(&buf, '\r');
                i += 2;
                break;
            case 't':
                cu_sb_char(&buf, '\t');
                i += 2;
                break;
            default:
                cu_sb_char(&buf, '\\');
                i++;
                break;
            }
            continue;
        }
        if (c == '"') {
            mark_reader_advance(r, i + 1);
            if (!cu_sb_finish(&buf, out)) {
                ctx->failed = 1;
                return 0;
            }
            return 1;
        }
        cu_sb_char(&buf, (char)c);
        i++;
    }
    return 0;
}

/* scanAttributeDecimal: the digits are counted, never kept */
static long scan_attribute_decimal(MarkReader *r)
{
    long n = 0;

    for (;;) {
        unsigned char c = mark_reader_peek(r);

        if (!mark_byte_numeric(c)) return n;
        n++;
        mark_reader_advance(r, 1);
    }
}

/* parseAttributeNumber. The value is never rendered; what matters is
 * whether strconv.ParseFloat accepts the text, which it does unless an
 * exponent marker has no digits or the value overflows float64. */
static int parse_attribute_number(MarkCtx *ctx, MarkReader *r)
{
    unsigned char c = mark_reader_peek(r);
    long mantissa_start;
    MarkSegment seg;
    MarkLine line;
    long int_digits;
    long frac_digits = 0;
    long exp_sign = 1;
    long exp = 0;
    long exp_digits = 0;
    long lead_zeros = 0;
    long dec_exp;
    long sig_len = 0;
    long i;
    long k;
    char sig[18];

    (void)ctx;
    if (c == '-' || c == '+') mark_reader_advance(r, 1);
    if (!mark_byte_numeric(mark_reader_peek(r))) return 0;
    line = mark_reader_peek_line(r, &seg);
    mantissa_start = 0;
    int_digits = scan_attribute_decimal(r);
    if (int_digits == 0) return 0;
    if (mark_reader_peek(r) == '.') {
        mark_reader_advance(r, 1);
        frac_digits = scan_attribute_decimal(r);
    }
    c = mark_reader_peek(r);
    if (c == 'e' || c == 'E') {
        mark_reader_advance(r, 1);
        c = mark_reader_peek(r);
        if (c == '-' || c == '+') {
            if (c == '-') exp_sign = -1;
            mark_reader_advance(r, 1);
        }
        for (;;) {
            c = mark_reader_peek(r);
            if (!mark_byte_numeric(c)) break;
            if (exp < 100000000L) exp = exp * 10 + (c - '0');
            exp_digits++;
            mark_reader_advance(r, 1);
        }
        if (exp_digits == 0) return 0;
    }
    /* Overflow test on the digits of the mantissa as peeked: the value is
     * at least 10^dec_exp where dec_exp counts the significant integer
     * digits, so anything past 1.7976931348623157e308 is ErrRange. */
    for (i = mantissa_start; i < mantissa_start + int_digits + (frac_digits > 0 ? 1 + frac_digits : 0); i++) {
        unsigned char d = mark_line_at(line, i);

        if (d == '.') continue;
        if (sig_len == 0 && d == '0') {
            lead_zeros++;
            continue;
        }
        if (sig_len < 17) sig[sig_len++] = (char)d;
    }
    if (sig_len == 0) return 1; /* zero never overflows */
    dec_exp = int_digits - lead_zeros - 1;
    if (dec_exp < 0) dec_exp = -(lead_zeros - int_digits) - 1;
    dec_exp += exp_sign * exp;
    if (dec_exp > 308) return 0;
    if (dec_exp == 308) {
        static const char max[] = "17976931348623157";

        for (k = 0; k < 17; k++) {
            char d = k < sig_len ? sig[k] : '0';

            if (d > max[k]) return 0;
            if (d < max[k]) break;
        }
    }
    return 1;
}

/* parseAttributeOthers */
static int parse_attribute_others(MarkCtx *ctx, MarkReader *r, MarkAttrValue *out)
{
    MarkSegment seg;
    MarkLine line = mark_reader_peek_line(r, &seg);
    long i = 0;
    long l = mark_line_len(line);
    CuStr value;

    if (!name_start_byte(mark_line_at(line, 0))) return 0;
    for (; i < l; i++) {
        if (!name_byte(mark_line_at(line, i))) break;
    }
    value = mark_line_slice(ctx->arena, line, 0, i);
    mark_reader_advance(r, i);
    if (cu_str_eq(value, CU_LIT("true")) || cu_str_eq(value, CU_LIT("false"))) {
        *out = mark_attr_other(MARK_ATTR_BOOL);
        return 1;
    }
    if (cu_str_eq(value, CU_LIT("null"))) {
        *out = mark_attr_other(MARK_ATTR_NULL);
        return 1;
    }
    *out = mark_attr_string(value);
    return 1;
}

static int parse_attribute_value(MarkCtx *ctx, MarkReader *r, MarkAttrValue *out, long depth);

/* parseAttributeArray */
static int parse_attribute_array(MarkCtx *ctx, MarkReader *r, long depth)
{
    long i;

    mark_reader_advance(r, 1); /* skip [ */
    for (i = 0;; i++) {
        unsigned char c = mark_reader_peek(r);
        int comma = 0;
        MarkAttrValue v;

        if (i != 0 && c == ',') {
            mark_reader_advance(r, 1);
            comma = 1;
        }
        if (c == ']') {
            if (!comma) {
                mark_reader_advance(r, 1);
                return 1;
            }
            return 0;
        }
        {
            MarkSegment s;
            long chars;

            mark_reader_skip_spaces(r, &s, &chars);
            if (!parse_attribute_value(ctx, r, &v, depth)) return 0;
            mark_reader_skip_spaces(r, &s, &chars);
        }
    }
}

/* parseAttributeValue. depth counts nested {} and [] values; past
 * CU_MARK_MAX_DEPTH the render is refused, since Go recurses here. */
static int parse_attribute_value(MarkCtx *ctx, MarkReader *r, MarkAttrValue *out, long depth)
{
    MarkSegment s;
    long chars;
    unsigned char c;

    if (depth > CU_MARK_MAX_DEPTH) {
        ctx->limit = 1;
        return 0;
    }
    mark_reader_skip_spaces(r, &s, &chars);
    c = mark_reader_peek(r);
    switch (c) {
    case MARK_EOF:
        return 0;
    case '{': {
        CuVec nested;

        CU_VEC_INIT(&nested, ctx->arena, MarkAttr);
        if (!parse_attributes(ctx, r, &nested, depth + 1)) return 0;
        *out = mark_attr_other(MARK_ATTR_MAP);
        return 1;
    }
    case '[':
        if (!parse_attribute_array(ctx, r, depth + 1)) return 0;
        *out = mark_attr_other(MARK_ATTR_ARRAY);
        return 1;
    case '"': {
        CuStr str;

        if (!parse_attribute_string(ctx, r, &str)) return 0;
        *out = mark_attr_string(str);
        return 1;
    }
    default:
        if (c == '-' || c == '+' || mark_byte_numeric(c)) {
            if (!parse_attribute_number(ctx, r)) return 0;
            *out = mark_attr_other(MARK_ATTR_NUMBER);
            return 1;
        }
        return parse_attribute_others(ctx, r, out);
    }
}

/* parseAttribute */
static int parse_attribute(MarkCtx *ctx, MarkReader *r, MarkAttr *out, long depth)
{
    MarkSegment s;
    long chars;
    unsigned char c;
    MarkLine line;
    long i = 0;
    long l;

    mark_reader_skip_spaces(r, &s, &chars);
    c = mark_reader_peek(r);
    if (c == '#' || c == '.') {
        mark_reader_advance(r, 1);
        line = mark_reader_peek_line(r, &s);
        l = mark_line_len(line);
        /* goldmark restricts id characters to what XHTML allows. */
        for (; i < l; i++) {
            unsigned char b = mark_line_at(line, i);

            if (mark_byte_space(b)) break;
            if (mark_byte_punct(b) && b != '_' && b != '-' && b != ':' && b != '.') break;
        }
        out->name = c == '#' ? attr_name_id() : attr_name_class();
        out->value = mark_attr_string(mark_line_slice(ctx->arena, line, 0, i));
        mark_reader_advance(r, i);
        return 1;
    }
    line = mark_reader_peek_line(r, &s);
    l = mark_line_len(line);
    if (!line.valid || l == 0) return 0;
    if (!name_start_byte(mark_line_at(line, 0))) return 0;
    for (; i < l; i++) {
        if (!name_byte(mark_line_at(line, i))) break;
    }
    out->name = mark_line_slice(ctx->arena, line, 0, i);
    mark_reader_advance(r, i);
    mark_reader_skip_spaces(r, &s, &chars);
    if (mark_reader_peek(r) != '=') return 0;
    mark_reader_advance(r, 1);
    mark_reader_skip_spaces(r, &s, &chars);
    if (!parse_attribute_value(ctx, r, &out->value, depth)) return 0;
    if (cu_str_eq(out->name, attr_name_class()) && out->value.kind != MARK_ATTR_STRING) return 0;
    return 1;
}

/* ParseAttributes. attrs receives the attributes in source order with
 * class values merged into the first class. */
static int parse_attributes(MarkCtx *ctx, MarkReader *r, CuVec *attrs, long depth)
{
    long saved_line;
    MarkSegment saved_position;
    MarkSegment s;
    long chars;

    if (depth > CU_MARK_MAX_DEPTH) {
        ctx->limit = 1;
        return 0;
    }
    mark_reader_position(r, &saved_line, &saved_position);
    mark_reader_skip_spaces(r, &s, &chars);
    if (mark_reader_peek(r) != '{') {
        mark_reader_set_position(r, saved_line, saved_position);
        return 0;
    }
    mark_reader_advance(r, 1);
    for (;;) {
        MarkAttr attr;

        if (ctx->failed || ctx->limit) return 0;
        if (mark_reader_peek(r) == '}') {
            mark_reader_advance(r, 1);
            return 1;
        }
        if (!parse_attribute(ctx, r, &attr, depth)) {
            mark_reader_set_position(r, saved_line, saved_position);
            return 0;
        }
        if (cu_str_eq(attr.name, attr_name_class())) {
            if (!merge_class(ctx, attrs, attr.value.str)) CU_VEC_PUSH(attrs, &attr);
        } else {
            CU_VEC_PUSH(attrs, &attr);
        }
        if (cu_vec_failed(attrs)) ctx->failed = 1;
        mark_reader_skip_spaces(r, &s, &chars);
        if (mark_reader_peek(r) == ',') {
            mark_reader_advance(r, 1);
            mark_reader_skip_spaces(r, &s, &chars);
        }
    }
}

static void set_attributes(MarkNode *node, const CuVec *attrs)
{
    size_t i;

    for (i = 0; i < cu_vec_len(attrs); i++) {
        const MarkAttr *a = CU_VEC_AT(attrs, MarkAttr, i);

        mark_node_set_attribute(node, a->name, a->value);
    }
}

/* ---- Headings (atx_heading.go helpers shared with setext) ---- */

/* generateAutoHeadingID: the id comes from the heading's last line. */
static void generate_auto_heading_id(MarkCtx *ctx, MarkNode *node)
{
    CuStr line = cu_str_empty();
    CuStr id;
    long last_index = node->lines == NULL ? -1 : (long)cu_vec_len(node->lines) - 1;

    if (last_index > -1) {
        MarkSegment last = mark_lines_at(node->lines, last_index);
        MarkLine l = mark_segment_value(last, ctx->src, ctx->src_len);

        line = mark_line_slice(ctx->arena, l, 0, mark_line_len(l));
    }
    id = mark_ids_generate(ctx, line);
    if (ctx->failed) return;
    mark_node_set_attribute(node, attr_name_id(), mark_attr_string(id));
}

/* parseLastLineAttributes: a {...} block that ends the heading's last
 * line becomes attributes and is cut from the line. */
static void parse_last_line_attributes(MarkCtx *ctx, MarkNode *node)
{
    long last_index = node->lines == NULL ? -1 : (long)cu_vec_len(node->lines) - 1;
    MarkSegment last_line;
    MarkLine lv;
    CuStr line;
    MarkReader lr;
    CuVec attrs;
    int ok = 0;
    MarkSegment start = mark_segment(0, 0);
    long sl = 0;
    MarkSegment end = mark_segment(0, 0);

    if (last_index < 0) return; /* empty headings */
    last_line = mark_lines_at(node->lines, last_index);
    lv = mark_segment_value(last_line, ctx->src, ctx->src_len);
    line = mark_line_slice(ctx->arena, lv, 0, mark_line_len(lv));
    mark_reader_init(&lr, (const unsigned char *)line.ptr, (long)line.len);
    CU_VEC_INIT(&attrs, ctx->arena, MarkAttr);
    for (;;) {
        unsigned char c = mark_reader_peek(&lr);

        if (ctx->failed || ctx->limit) return;
        if (c == MARK_EOF) break;
        if (c == '\\') {
            mark_reader_advance(&lr, 1);
            if (mark_reader_peek(&lr) == '{') mark_reader_advance(&lr, 1);
            continue;
        }
        if (c == '{') {
            long el;

            mark_reader_position(&lr, &sl, &start);
            cu_vec_clear(&attrs);
            ok = parse_attributes(ctx, &lr, &attrs, 0);
            mark_reader_position(&lr, &el, &end);
            mark_reader_set_position(&lr, sl, start);
        }
        mark_reader_advance(&lr, 1);
    }
    if (ok && mark_is_blank((const unsigned char *)line.ptr + (end.start > (long)line.len ? (long)line.len : end.start),
                            (long)line.len - (end.start > (long)line.len ? (long)line.len : end.start))) {
        set_attributes(node, &attrs);
        last_line.stop = last_line.start + start.start;
        *CU_VEC_AT(node->lines, MarkSegment, (size_t)last_index) = last_line;
    }
}

/* The tail of atxHeadingParser.Close and setextHeadingParser.Close:
 * the Attribute and AutoHeadingID options. */
static void heading_close_options(MarkCtx *ctx, MarkNode *node, int attribute_only_without_id)
{
    if (option(ctx, CU_MARK_ATTRIBUTE)) {
        if (!attribute_only_without_id || mark_node_attribute(node, attr_name_id()) == NULL) {
            parse_last_line_attributes(ctx, node);
        }
    }
    if (option(ctx, CU_MARK_AUTO_HEADING_ID)) {
        const MarkAttr *id = mark_node_attribute(node, attr_name_id());

        if (id == NULL) generate_auto_heading_id(ctx, node);
        else if (id->value.kind == MARK_ATTR_STRING) mark_ids_put(ctx, id->value.str);
    }
}

/* ---- setext_headings.go ---- */

/* matchesSetextHeadingBar */
static int matches_setext_heading_bar(MarkLine line, unsigned char *out)
{
    long start = 0;
    long end = mark_line_len(line);
    long space = mark_line_trim_left_len(line, ' ');
    MarkLine rest;
    long level1;
    long level2 = 0;
    unsigned char c = '=';

    if (space > 3) return 0;
    start += space;
    rest = mark_line_skip(line, start);
    level1 = mark_line_trim_left_len(rest, '=');
    if (level1 == 0) {
        level2 = mark_line_trim_left_len(rest, '-');
        c = '-';
    }
    if (end > 0 && mark_byte_space(mark_line_at(line, end - 1))) end -= mark_line_trim_right_space_len(rest);
    if (!((level1 > 0 && start + level1 == end) || (level2 > 0 && start + level2 == end))) return 0;
    *out = c;
    return 1;
}

/* setextHeadingParser.Open */
static MarkNode *setext_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkNode *last = mark_last_opened_block(ctx).node;
    MarkSegment seg;
    MarkLine line;
    unsigned char c;
    MarkNode *node;

    *state = MARK_STATE_NO_CHILDREN;
    if (last == NULL) return NULL;
    if (last->kind != MARK_PARAGRAPH || last->parent != parent) return NULL;
    line = mark_reader_peek_line(r, &seg);
    if (!matches_setext_heading_bar(line, &c)) return NULL;
    node = mark_node_new(ctx->arena, MARK_HEADING);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    node->level = c == '-' ? 2 : 1;
    mark_lines_append(ctx, mark_node_lines(ctx->arena, node), seg);
    ctx->temporary_paragraph = last;
    *state = MARK_STATE_NO_CHILDREN | MARK_STATE_REQUIRE_PARAGRAPH;
    return node;
}

static unsigned close_state(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    (void)ctx;
    (void)node;
    (void)r;
    return MARK_STATE_CLOSE;
}

static void close_nothing(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    (void)ctx;
    (void)node;
    (void)r;
}

/* Segments.Unshift */
static void lines_unshift(MarkCtx *ctx, CuVec *lines, MarkSegment v)
{
    size_t len = cu_vec_len(lines);
    MarkSegment *d;

    CU_VEC_PUSH(lines, &v);
    if (cu_vec_failed(lines)) {
        ctx->failed = 1;
        return;
    }
    d = CU_VEC_DATA(lines, MarkSegment);
    memmove(d + 1, d, len * sizeof *d);
    d[0] = v;
}

/* setextHeadingParser.Close */
static void setext_close(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    MarkSegment segment = mark_lines_at(node->lines, 0);
    MarkNode *tmp = ctx->temporary_paragraph;
    CuVec *tmp_lines;

    (void)r;
    if (node->lines != NULL) cu_vec_clear(node->lines);
    ctx->temporary_paragraph = NULL;
    if (tmp == NULL) return; /* Go would dereference nil here */
    tmp_lines = mark_node_lines(ctx->arena, tmp);
    if (tmp_lines == NULL) {
        ctx->failed = 1;
        return;
    }
    if (cu_vec_len(tmp_lines) == 0) {
        MarkNode *next = node->next;

        segment = mark_segment_trim_left_space(segment, ctx->src, ctx->src_len);
        if (next == NULL || next->kind != MARK_PARAGRAPH) {
            MarkNode *para = mark_node_new(ctx->arena, MARK_PARAGRAPH);

            if (para == NULL) {
                ctx->failed = 1;
                return;
            }
            mark_lines_append(ctx, mark_node_lines(ctx->arena, para), segment);
            if (node->parent != NULL) mark_node_insert_after(node->parent, node, para);
        } else {
            lines_unshift(ctx, mark_node_lines(ctx->arena, next), segment);
        }
        if (node->parent != NULL) mark_node_remove_child(node->parent, node);
    } else {
        node->lines = tmp_lines;
        node->blank_previous_lines = tmp->blank_previous_lines;
        if (tmp->parent != NULL) mark_node_remove_child(tmp->parent, tmp);
    }
    heading_close_options(ctx, node, 0);
}

/* ---- thematic_break.go ---- */

/* isThematicBreak */
static int is_thematic_break(MarkLine line, long offset)
{
    long pos;
    long w = mark_line_indent_width(line, offset, &pos);
    unsigned char mark = 0;
    long count = 0;
    long len = mark_line_len(line);
    long i;

    if (w > 3) return 0;
    for (i = pos; i < len; i++) {
        unsigned char c = mark_line_at(line, i);

        if (mark_byte_space(c)) continue;
        if (mark == 0) {
            mark = c;
            count = 1;
            if (mark == '*' || mark == '-' || mark == '_') continue;
            return 0;
        }
        if (c != mark) return 0;
        count++;
    }
    return count > 2;
}

/* thematicBreakPraser.Open */
static MarkNode *thematic_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkSegment seg;
    MarkLine line = mark_reader_peek_line(r, &seg);
    MarkNode *node;

    (void)parent;
    *state = MARK_STATE_NO_CHILDREN;
    if (!is_thematic_break(line, mark_reader_line_offset(r))) return NULL;
    mark_reader_advance(r, mark_segment_len(seg) - 1);
    node = mark_node_new(ctx->arena, MARK_THEMATIC_BREAK);
    if (node == NULL) ctx->failed = 1;
    return node;
}

/* ---- list.go and list_item.go ---- */

#define NOT_LIST 0
#define BULLET_LIST 1
#define ORDERED_LIST 2

/* parseListItem: ^(([ ]*)([\-\*\+]))(\s+.*)?\n?$ or
 * ^(([ ]*)(\d{1,9}[\.\)]))(\s+.*)?\n?$ as submatch indices in m. */
static int parse_list_item(MarkLine line, long m[6])
{
    long i = 0;
    long l = mark_line_len(line);
    int typ;

    m[0] = m[1] = m[2] = m[3] = m[4] = m[5] = 0;
    for (; i < l && mark_line_at(line, i) == ' '; i++) {
    }
    if (i > 3) return NOT_LIST;
    m[0] = 0;
    m[1] = i;
    m[2] = i;
    if (i < l && (mark_line_at(line, i) == '-' || mark_line_at(line, i) == '*' || mark_line_at(line, i) == '+')) {
        i++;
        m[3] = i;
        typ = BULLET_LIST;
    } else if (i < l) {
        for (; i < l && mark_byte_numeric(mark_line_at(line, i)); i++) {
        }
        m[3] = i;
        if (m[3] == m[2] || m[3] - m[2] > 9) return NOT_LIST;
        if (i < l && (mark_line_at(line, i) == '.' || mark_line_at(line, i) == ')')) {
            i++;
            m[3] = i;
        } else {
            return NOT_LIST;
        }
        typ = ORDERED_LIST;
    } else {
        return NOT_LIST;
    }
    if (i < l && mark_line_at(line, i) != '\n') {
        long pos;
        long w = mark_line_indent_width(mark_line_skip(line, i), 0, &pos);

        if (w == 0) return NOT_LIST;
    }
    if (i >= l) {
        m[4] = -1;
        m[5] = -1;
        return typ;
    }
    m[4] = i;
    m[5] = l;
    if (mark_line_at(line, m[5] - 1) == '\n' && mark_line_at(line, i) != '\n') m[5]--;
    return typ;
}

/* matchesListItem */
static int matches_list_item(MarkLine line, int strict, long m[6])
{
    int typ = parse_list_item(line, m);

    if (typ != NOT_LIST && (!strict || m[1] < 4)) return typ;
    return NOT_LIST;
}

/* calcListOffset */
static long calc_list_offset(MarkLine line, const long m[6])
{
    long offset;
    long pos;

    if (m[4] < 0 || mark_line_is_blank(mark_line_skip(line, m[4]))) {
        offset = 1; /* the item starts with a blank line */
    } else {
        offset = mark_line_indent_width(mark_line_skip(line, m[4]), m[4], &pos);
        if (offset > 4) offset = 1; /* an indented code block follows */
    }
    return offset;
}

/* lastOffset */
static long last_offset(const MarkNode *node)
{
    if (node->last != NULL) return node->last->offset;
    return 0;
}

/* strconv.Atoi on at most 9 digits */
static long atoi_digits(MarkLine line, long from, long to)
{
    long v = 0;
    long i;

    for (i = from; i < to; i++) v = v * 10 + (mark_line_at(line, i) - '0');
    return v;
}

/* listParser.Open */
static MarkNode *list_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkNode *last = mark_last_opened_block(ctx).node;
    MarkSegment seg;
    MarkLine line;
    long m[6];
    int typ;
    long start = -1;
    MarkNode *node;

    *state = MARK_STATE_NO_CHILDREN;
    if ((last != NULL && last->kind == MARK_LIST) || ctx->skip_list_parser) {
        ctx->skip_list_parser = 0;
        return NULL;
    }
    line = mark_reader_peek_line(r, &seg);
    typ = matches_list_item(line, 1, m);
    if (typ == NOT_LIST) return NULL;
    if (typ == ORDERED_LIST) start = atoi_digits(line, m[2], m[3] - 1);
    if (last != NULL && last->kind == MARK_PARAGRAPH && last->parent == parent) {
        /* only lists starting with 1 may interrupt a paragraph */
        if (typ == ORDERED_LIST && start != 1) return NULL;
        /* and an empty list item cannot */
        if (m[4] < 0 || mark_line_is_blank(mark_line_sub(line, m[4], m[5]))) return NULL;
    }
    node = mark_node_new(ctx->arena, MARK_LIST);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    node->marker = mark_line_at(line, m[3] - 1);
    if (start > -1) node->start = start;
    ctx->empty_list_item_with_blank_lines = 0;
    *state = MARK_STATE_HAS_CHILDREN;
    return node;
}

/* listParser.Continue */
static unsigned list_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    MarkSegment seg;
    MarkLine line = mark_reader_peek_line(r, &seg);
    long offset;
    int last_is_empty;
    long indent;
    long pos;

    if (mark_line_is_blank(line)) {
        if (node->last == NULL || node->last->child_count == 0) ctx->empty_list_item_with_blank_lines = 1;
        return MARK_STATE_CONTINUE | MARK_STATE_HAS_CHILDREN;
    }
    offset = last_offset(node);
    last_is_empty = node->last == NULL || node->last->child_count == 0;
    indent = mark_line_indent_width(line, mark_reader_line_offset(r), &pos);

    if (indent < offset || last_is_empty) {
        if (indent < 4) {
            long m[6];
            int typ = matches_list_item(line, 0, m); /* may have more than 3 leading spaces */

            if (typ != NOT_LIST && m[1] - offset < 4) {
                unsigned char marker = mark_line_at(line, m[3] - 1);

                if (!(marker == node->marker && (typ == ORDERED_LIST) == mark_list_is_ordered(node))) {
                    return MARK_STATE_CLOSE;
                }
                /* thematic breaks take precedence over lists */
                if (is_thematic_break(mark_line_skip(line, m[3] - 1), 0)) {
                    int is_heading = 0;
                    MarkNode *last = mark_last_opened_block(ctx).node;

                    if (last != NULL && last->kind == MARK_PARAGRAPH) {
                        unsigned char c;

                        if (matches_setext_heading_bar(mark_line_skip(line, m[3] - 1), &c) && c == '-') is_heading = 1;
                    }
                    if (!is_heading) return MARK_STATE_CLOSE;
                }
                return MARK_STATE_CONTINUE | MARK_STATE_HAS_CHILDREN;
            }
        }
        if (!last_is_empty) return MARK_STATE_CLOSE;
    }

    if (last_is_empty && indent < offset) return MARK_STATE_CLOSE;

    /* a non-empty item cannot follow an empty item with blank lines */
    if (ctx->empty_list_item_with_blank_lines) return MARK_STATE_CLOSE;
    return MARK_STATE_CONTINUE | MARK_STATE_HAS_CHILDREN;
}

/* listParser.Close: decides tightness and turns tight paragraphs into
 * text blocks. */
static void list_close(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    MarkNode *c;

    (void)r;
    for (c = node->first; c != NULL && node->is_tight; c = c->next) {
        if (c->first != NULL && c->first != c->last) {
            MarkNode *c1;

            for (c1 = c->first->next; c1 != NULL; c1 = c1->next) {
                if (c1->blank_previous_lines) {
                    node->is_tight = 0;
                    break;
                }
            }
        }
        if (c != node->first) {
            if (c->blank_previous_lines) node->is_tight = 0;
        }
    }
    if (node->is_tight) {
        MarkNode *child;

        for (child = node->first; child != NULL; child = child->next) {
            MarkNode *gc = child->first;

            while (gc != NULL) {
                MarkNode *paragraph = gc;

                gc = gc->next;
                if (paragraph->kind == MARK_PARAGRAPH) {
                    MarkNode *text_block = mark_node_new(ctx->arena, MARK_TEXT_BLOCK);

                    if (text_block == NULL) {
                        ctx->failed = 1;
                        return;
                    }
                    text_block->lines = paragraph->lines;
                    mark_node_replace_child(child, paragraph, text_block);
                }
            }
        }
    }
}

/* listItemParser.Open */
static MarkNode *list_item_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    long offset;
    MarkSegment seg;
    MarkLine line;
    long m[6];
    int typ;
    long item_offset;
    MarkNode *node;
    long pos;
    long padding;

    *state = MARK_STATE_NO_CHILDREN;
    if (parent->kind != MARK_LIST) return NULL; /* an item must be a list's child */
    offset = last_offset(parent);
    line = mark_reader_peek_line(r, &seg);
    typ = matches_list_item(line, 0, m);
    if (typ == NOT_LIST) return NULL;
    if (m[1] - offset > 3) return NULL;

    ctx->empty_list_item_with_blank_lines = 0;

    item_offset = calc_list_offset(line, m);
    node = mark_node_new(ctx->arena, MARK_LIST_ITEM);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    node->offset = m[3] + item_offset;
    if (m[4] < 0 || mark_line_is_blank(mark_line_sub(line, m[4], m[5]))) return node;

    pos = mark_line_indent_position(mark_line_skip(line, m[4]), m[4], 0, item_offset, &padding);
    mark_reader_advance_and_set_padding(r, m[3] + pos, padding);
    *state = MARK_STATE_HAS_CHILDREN;
    return node;
}

/* listItemParser.Continue */
static unsigned list_item_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    MarkSegment seg;
    MarkLine line = mark_reader_peek_line(r, &seg);
    long offset;
    int is_empty;
    long indent;
    long pos;
    long padding;

    if (mark_line_is_blank(line)) {
        mark_reader_advance(r, mark_line_len(line) - 1);
        return MARK_STATE_CONTINUE | MARK_STATE_HAS_CHILDREN;
    }

    offset = node->parent == NULL ? 0 : last_offset(node->parent);
    is_empty = node->child_count == 0;
    indent = mark_line_indent_width(line, mark_reader_line_offset(r), &pos);
    if ((is_empty || indent < offset) && indent < 4) {
        long m[6];

        if (matches_list_item(line, 1, m) != NOT_LIST) { /* a new list item */
            ctx->skip_list_parser = 1;
            return MARK_STATE_CLOSE;
        }
        if (!is_empty) return MARK_STATE_CLOSE;
    }
    pos = mark_line_indent_position(line, mark_reader_line_offset(r), 0, offset, &padding);
    mark_reader_advance_and_set_padding(r, pos, padding);
    return MARK_STATE_CONTINUE | MARK_STATE_HAS_CHILDREN;
}

/* ---- code_block.go ---- */

/* preserveLeadingTabInCodeBlock */
static void preserve_leading_tab(MarkSegment *segment, MarkReader *r, long indent)
{
    long offset_with_padding = mark_reader_line_offset(r) + indent;
    long sl;
    MarkSegment ss;

    mark_reader_position(r, &sl, &ss);
    mark_reader_set_position(r, sl, mark_segment(ss.start - 1, ss.stop));
    if (offset_with_padding == mark_reader_line_offset(r)) {
        segment->padding = 0;
        segment->start--;
    }
    mark_reader_set_position(r, sl, ss);
}

/* codeBlockParser.Open */
static MarkNode *code_block_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkSegment seg;
    MarkLine line = mark_reader_peek_line(r, &seg);
    long padding;
    long pos = mark_line_indent_position(line, mark_reader_line_offset(r), 0, 4, &padding);
    MarkNode *node;

    (void)parent;
    *state = MARK_STATE_NO_CHILDREN;
    if (pos < 0 || mark_line_is_blank(line)) return NULL;
    node = mark_node_new(ctx->arena, MARK_CODE_BLOCK);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    mark_reader_advance_and_set_padding(r, pos, padding);
    mark_reader_peek_line(r, &seg);
    /* a line that starts with a tab keeps the tab */
    if (seg.padding != 0) preserve_leading_tab(&seg, r, 0);
    mark_lines_append(ctx, mark_node_lines(ctx->arena, node), seg);
    mark_reader_advance(r, mark_segment_len(seg) - 1);
    return node;
}

/* codeBlockParser.Continue */
static unsigned code_block_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    MarkSegment seg;
    MarkLine line = mark_reader_peek_line(r, &seg);
    long padding;
    long pos;

    if (mark_line_is_blank(line)) {
        mark_lines_append(ctx, mark_node_lines(ctx->arena, node),
                          mark_segment_trim_left_space_width(seg, 4, ctx->src, ctx->src_len));
        return MARK_STATE_CONTINUE | MARK_STATE_NO_CHILDREN;
    }
    pos = mark_line_indent_position(line, mark_reader_line_offset(r), 0, 4, &padding);
    if (pos < 0) return MARK_STATE_CLOSE;
    mark_reader_advance_and_set_padding(r, pos, padding);
    mark_reader_peek_line(r, &seg);
    if (seg.padding != 0) preserve_leading_tab(&seg, r, 0);
    mark_lines_append(ctx, mark_node_lines(ctx->arena, node), seg);
    mark_reader_advance(r, mark_segment_len(seg) - 1);
    return MARK_STATE_CONTINUE | MARK_STATE_NO_CHILDREN;
}

/* codeBlockParser.Close: drops trailing blank lines */
static void code_block_close(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    CuVec *lines = node->lines;
    long length;

    (void)r;
    if (lines == NULL) return;
    length = (long)cu_vec_len(lines) - 1;
    while (length >= 0) {
        MarkLine l = mark_segment_value(mark_lines_at(lines, length), ctx->src, ctx->src_len);

        if (mark_line_is_blank(l)) length--;
        else break;
    }
    mark_lines_truncate(lines, (size_t)(length + 1));
}

/* ---- atx_heading.go ---- */

/* atxHeadingParser.Open */
static MarkNode *atx_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(r, &segment);
    long len = mark_line_len(line);
    long pos = ctx->block_offset;
    long i;
    long level;
    long l;
    long start;
    long origstart;
    long stop;
    MarkNode *node;
    int parsed = 0;

    (void)parent;
    *state = MARK_STATE_NO_CHILDREN;
    if (pos < 0) return NULL;
    i = pos;
    for (; i < len && mark_line_at(line, i) == '#'; i++) {
    }
    level = i - pos;
    if (i == pos || level > 6) return NULL;
    node = mark_node_new(ctx->arena, MARK_HEADING);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    node->level = level;
    if (i == len) return node; /* a lone '#' without a newline */
    l = mark_line_trim_left_space_len(mark_line_skip(line, i));
    if (l == 0) return NULL;
    start = i + l;
    if (start >= len) start = len - 1;
    origstart = start;
    stop = len - mark_line_trim_right_space_len(line);

    if (option(ctx, CU_MARK_ATTRIBUTE)) { /* ### heading ### {#id} */
        long closure_close = -1;
        long closure_open = -1;
        long j;

        start--;
        for (j = start; j < stop;) {
            unsigned char c = mark_line_at(line, j);

            if (c == '\\' && j < len - 1 && mark_byte_punct(mark_line_at(line, j + 1))) {
                j += 2;
            } else if (mark_byte_space(c) && j < stop - 1 && mark_line_at(line, j + 1) == '#') {
                long k = j + 1;

                closure_open = j + 1;
                for (; k < stop && mark_line_at(line, k) == '#'; k++) {
                }
                closure_close = k;
                break;
            } else {
                j++;
            }
        }
        if (closure_close > 0) {
            CuVec attrs;
            int ok;
            MarkSegment rest_seg;
            MarkLine rest;

            CU_VEC_INIT(&attrs, ctx->arena, MarkAttr);
            mark_reader_advance(r, closure_close);
            ok = parse_attributes(ctx, r, &attrs, 0);
            if (ctx->failed || ctx->limit) return NULL;
            rest = mark_reader_peek_line(r, &rest_seg);
            parsed = ok && (!rest.valid || mark_line_is_blank(rest));
            if (parsed) {
                set_attributes(node, &attrs);
                mark_lines_append(ctx, mark_node_lines(ctx->arena, node),
                                  mark_segment(line_pos(segment, start + 1), line_pos(segment, closure_open)));
            }
        }
    }
    if (!parsed) {
        start = origstart;
        stop = len - mark_line_trim_right_space_len(line);
        if (stop <= start) { /* empty headings like '##[space]' */
            stop = start;
        } else {
            i = stop - 1;
            for (; i >= start && mark_line_at(line, i) == '#'; i--) {
            }
            if (i != stop - 1 && !mark_byte_space(mark_line_at(line, i))) i = stop - 1;
            i++;
            stop = i;
        }
        /* empty headings like '### ###' get no line */
        if (stop - start - mark_line_trim_right_len(mark_line_sub(line, start, stop), '#') != 0) {
            mark_lines_append(ctx, mark_node_lines(ctx->arena, node),
                              mark_segment(line_pos(segment, start), line_pos(segment, stop)));
        }
    }
    return node;
}

/* atxHeadingParser.Close */
static void atx_close(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    (void)r;
    heading_close_options(ctx, node, 1);
}

/* ---- fcode_block.go ---- */

/* fencedCodeBlockParser.Open */
static MarkNode *fenced_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(r, &segment);
    long len = mark_line_len(line);
    long pos = ctx->block_offset;
    long findent;
    unsigned char fence_char;
    long i;
    long fence_length;
    MarkNode *info = NULL;
    MarkNode *node;

    (void)parent;
    *state = MARK_STATE_NO_CHILDREN;
    if (pos < 0 || (mark_line_at(line, pos) != '`' && mark_line_at(line, pos) != '~')) return NULL;
    findent = pos;
    fence_char = mark_line_at(line, pos);
    i = pos;
    for (; i < len && mark_line_at(line, i) == fence_char; i++) {
    }
    fence_length = i - pos;
    if (fence_length < 3) return NULL;
    if (i < len - 1) {
        MarkLine rest = mark_line_skip(line, i);
        long left = mark_line_trim_left_space_len(rest);
        long right = mark_line_trim_right_space_len(rest);

        if (left < mark_line_len(rest) - right) {
            long info_start = segment.start - segment.padding + i + left;
            long info_stop = segment.stop - right;
            MarkLine value = mark_line_sub(rest, left, mark_line_len(rest) - right);
            long vlen = mark_line_len(value);
            long k;

            if (fence_char == '`') {
                for (k = 0; k < vlen; k++) {
                    if (mark_line_at(value, k) == '`') return NULL;
                }
            }
            if (info_start != info_stop) {
                info = mark_text_new(ctx->arena, mark_segment(info_start, info_stop));
                if (info == NULL) {
                    ctx->failed = 1;
                    return NULL;
                }
            }
        }
    }
    node = mark_node_new(ctx->arena, MARK_FENCED_CODE_BLOCK);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    node->info = info;
    ctx->fence_set = 1;
    ctx->fence_char = fence_char;
    ctx->fence_indent = findent;
    ctx->fence_length = fence_length;
    ctx->fence_node = node;
    return node;
}

/* fencedCodeBlockParser.Continue */
static unsigned fenced_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(r, &segment);
    long len = mark_line_len(line);
    long pos;
    long w;
    long padding;
    MarkSegment seg;

    if (!ctx->fence_set) return MARK_STATE_CLOSE; /* Go would dereference nil */
    w = mark_line_indent_width(line, mark_reader_line_offset(r), &pos);
    if (w < 4) {
        long i = pos;
        long length;

        for (; i < len && mark_line_at(line, i) == ctx->fence_char; i++) {
        }
        length = i - pos;
        if (length >= ctx->fence_length && mark_line_is_blank(mark_line_skip(line, i))) {
            long newline = 1;

            if (len > 0 && mark_line_at(line, len - 1) != '\n') newline = 0;
            mark_reader_advance(r, segment.stop - segment.start - newline + segment.padding);
            return MARK_STATE_CLOSE;
        }
    }
    pos = mark_line_indent_position(line, mark_reader_line_offset(r), segment.padding, ctx->fence_indent, &padding);
    if (pos < 0) {
        pos = mark_line_first_non_space(line);
        if (pos < 0) pos = 0;
        padding = 0;
    }
    seg = mark_segment_padding(segment.start + pos, segment.stop, padding);
    /* a line that starts with a tab keeps the tab */
    if (padding != 0) preserve_leading_tab(&seg, r, ctx->fence_indent);
    mark_lines_append(ctx, mark_node_lines(ctx->arena, node), seg);
    mark_reader_advance_and_set_padding(r, segment.stop - segment.start - pos - 1, padding);
    return MARK_STATE_CONTINUE | MARK_STATE_NO_CHILDREN;
}

/* fencedCodeBlockParser.Close */
static void fenced_close(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    (void)r;
    if (ctx->fence_set && ctx->fence_node == node) ctx->fence_set = 0;
}

/* ---- blockquote.go ---- */

/* blockquoteParser.process */
static int blockquote_process(MarkReader *r)
{
    MarkSegment seg;
    MarkLine line = mark_reader_peek_line(r, &seg);
    long len = mark_line_len(line);
    long pos;
    long w = mark_line_indent_width(line, mark_reader_line_offset(r), &pos);

    if (w > 3 || pos >= len || mark_line_at(line, pos) != '>') return 0;
    pos++;
    if (pos >= len || mark_line_at(line, pos) == '\n') {
        mark_reader_advance(r, pos);
        return 1;
    }
    if (mark_line_at(line, pos) == ' ' || mark_line_at(line, pos) == '\t') pos++;
    mark_reader_advance(r, pos);
    /* goldmark pads 2 for a tab after '>' whatever column the tab is at */
    if (mark_line_at(line, pos - 1) == '\t') mark_reader_set_padding(r, 2);
    return 1;
}

/* blockquoteParser.Open */
static MarkNode *blockquote_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkNode *node;

    (void)parent;
    *state = MARK_STATE_NO_CHILDREN;
    if (!blockquote_process(r)) return NULL;
    node = mark_node_new(ctx->arena, MARK_BLOCKQUOTE);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    *state = MARK_STATE_HAS_CHILDREN;
    return node;
}

/* blockquoteParser.Continue */
static unsigned blockquote_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    (void)ctx;
    (void)node;
    if (blockquote_process(r)) return MARK_STATE_CONTINUE | MARK_STATE_HAS_CHILDREN;
    return MARK_STATE_CLOSE;
}

/* ---- html_block.go ---- */

/* allowedBlockTags, compared in lower case */
static const char *const allowed_block_tags[] = {
    "address", "article", "aside", "base", "basefont", "blockquote", "body", "caption", "center", "col",
    "colgroup", "dd", "details", "dialog", "dir", "div", "dl", "dt", "fieldset", "figcaption",
    "figure", "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5",
    "h6", "head", "header", "hr", "html", "iframe", "legend", "li", "link", "main",
    "menu", "menuitem", "meta", "nav", "noframes", "ol", "optgroup", "option", "p", "param",
    "section", "source", "summary", "table", "tbody", "td", "tfoot", "th", "thead", "title",
    "tr", "track", "ul"};

static int tag_lower_eq(MarkLine line, long from, long to, const char *tag)
{
    long i;

    for (i = from; i < to; i++) {
        if (cu_ascii_to_lower((char)mark_line_at(line, i)) != tag[i - from]) return 0;
    }
    return tag[to - from] == '\0';
}

static int is_allowed_block_tag(MarkLine line, long from, long to)
{
    size_t i;

    for (i = 0; i < sizeof allowed_block_tags / sizeof allowed_block_tags[0]; i++) {
        if (tag_lower_eq(line, from, to, allowed_block_tags[i])) return 1;
    }
    return 0;
}

static int is_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* ^[ ]{0,3}< : the position after '<', or -1 */
static long html_open_angle(MarkLine line)
{
    long i = 0;

    while (i < 3 && mark_line_at(line, i) == ' ') i++;
    if (mark_line_at(line, i) != '<') return -1;
    return i + 1;
}

/* Whether the rest of a line from i matches (?:\r\n|\n)?$: only a final
 * newline may remain. */
static int only_newline_left(MarkLine line, long i)
{
    long len = mark_line_len(line);

    if (i == len) return 1;
    if (i == len - 1) return mark_line_at(line, i) == '\n';
    if (i == len - 2) return mark_line_at(line, i) == '\r' && mark_line_at(line, i + 1) == '\n';
    return 0;
}

/* Whether .*(?:\r\n|\n)?$ matches from i: . never matches '\n', so the
 * rest may hold a newline only as its last byte. */
static int rest_is_one_line(MarkLine line, long i)
{
    long len = mark_line_len(line);
    long k;

    for (k = i; k < len; k++) {
        if (mark_line_at(line, k) == '\n' && k != len - 1) return 0;
    }
    return 1;
}

/* htmlBlockType1OpenRegexp:
 * (?i)^[ ]{0,3}<(script|pre|style|textarea)(?:\s.*|>.*|/>.*|)(?:\r\n|\n)?$ */
static int html_type1_open(MarkLine line)
{
    static const char *const tags[] = {"script", "pre", "style", "textarea"};
    long i = html_open_angle(line);
    size_t t;

    if (i < 0) return 0;
    for (t = 0; t < 4; t++) {
        long n = (long)strlen(tags[t]);
        unsigned char c;

        if (!tag_lower_eq(line, i, i + n, tags[t])) continue;
        if (i + n > mark_line_len(line)) continue;
        if (i + n == mark_line_len(line)) return 1;
        c = mark_line_at(line, i + n);
        if (c == ' ' || c == '\t' || c == '\n' || c == 0x0C || c == '\r' || c == '>') return rest_is_one_line(line, i + n);
        if (c == '/' && mark_line_at(line, i + n + 1) == '>') return rest_is_one_line(line, i + n);
        if (only_newline_left(line, i + n)) return 1;
    }
    return 0;
}

/* htmlBlockType1CloseRegexp: (?i)^.*</(?:script|pre|style|textarea)>.* */
static int html_type1_close(MarkLine line)
{
    static const char *const closers[] = {"</script>", "</pre>", "</style>", "</textarea>"};
    long len = mark_line_len(line);
    long i;

    for (i = 0; i < len; i++) {
        size_t t;

        if (mark_line_at(line, i) == '\n') return 0;
        for (t = 0; t < 4; t++) {
            long n = (long)strlen(closers[t]);

            if (i + n <= len && tag_lower_eq(line, i, i + n, closers[t])) return 1;
        }
    }
    return 0;
}

/* htmlBlockType2OpenRegexp ^[ ]{0,3}<!\-\-, Type3 ^[ ]{0,3}<\?,
 * Type5 ^[ ]{0,3}<\!\[CDATA\[ : '<' followed by the literal. */
static int html_open_literal(MarkLine line, const char *lit)
{
    long i = html_open_angle(line);
    size_t k;

    if (i < 0) return 0;
    for (k = 0; lit[k] != '\0'; k++) {
        if (mark_line_at(line, i + (long)k) != (unsigned char)lit[k]) return 0;
    }
    return 1;
}

/* htmlBlockType4OpenRegexp: ^[ ]{0,3}<![A-Z]+.*(?:\r\n|\n)?$ */
static int html_type4_open(MarkLine line)
{
    long i = html_open_angle(line);
    long n = 0;

    if (i < 0 || mark_line_at(line, i) != '!') return 0;
    i++;
    while (mark_line_at(line, i + n) >= 'A' && mark_line_at(line, i + n) <= 'Z') n++;
    if (n == 0) return 0;
    return rest_is_one_line(line, i + n);
}

/* [a-zA-Z]+[a-zA-Z0-9\-]* from i: the end, or -1 */
static long html_tag_name(MarkLine line, long i)
{
    long j = i;

    if (!is_alpha(mark_line_at(line, i))) return -1;
    while (is_alpha(mark_line_at(line, j)) || mark_byte_numeric(mark_line_at(line, j)) || mark_line_at(line, j) == '-') j++;
    return j;
}

/* htmlBlockType6Regexp:
 * ^[ ]{0,3}<(?:/[ ]*)?([a-zA-Z]+[a-zA-Z0-9\-]*)(?:[ ].*|>.*|/>.*|)(?:\r\n|\n)?$
 * from/to get the tag name. */
static int html_type6(MarkLine line, long *from, long *to)
{
    long i = html_open_angle(line);
    long j;
    unsigned char c;

    if (i < 0) return 0;
    if (mark_line_at(line, i) == '/') {
        i++;
        while (mark_line_at(line, i) == ' ') i++;
    }
    j = html_tag_name(line, i);
    if (j < 0) return 0;
    *from = i;
    *to = j;
    if (j == mark_line_len(line)) return 1;
    c = mark_line_at(line, j);
    if (c == ' ' || c == '>') return rest_is_one_line(line, j);
    if (c == '/' && mark_line_at(line, j + 1) == '>') return rest_is_one_line(line, j);
    return only_newline_left(line, j);
}

static int attr_ws(unsigned char c)
{
    return c == '\r' || c == '\n' || c == ' ' || c == '\t';
}

/* One attribute of html_block.go's attributePattern on a line:
 * (?:[\r\n \t]+[a-zA-Z_:][a-zA-Z0-9:._-]*(?:[\r\n \t]*=[\r\n \t]*(?:[^"'=<>`\x00-\x20]+|'[^']*'|"[^"]*"))?)
 * Returns the end, or -1 when none starts at i. The value part is
 * optional, so when it fails the attribute ends after the name, which is
 * what a backtracking match of the pattern does too. */
static long html_attribute(MarkLine line, long i)
{
    long len = mark_line_len(line);
    long j = i;
    long name_end;
    unsigned char c;

    while (j < len && attr_ws(mark_line_at(line, j))) j++;
    if (j == i) return -1;
    if (!name_start_byte(mark_line_at(line, j))) return -1;
    while (j < len && name_byte(mark_line_at(line, j))) j++;
    name_end = j;
    while (j < len && attr_ws(mark_line_at(line, j))) j++;
    if (mark_line_at(line, j) != '=') return name_end;
    j++;
    while (j < len && attr_ws(mark_line_at(line, j))) j++;
    c = mark_line_at(line, j);
    if (c == '\'' || c == '"') {
        long k = j + 1;

        while (k < len && mark_line_at(line, k) != c) k++;
        if (k >= len) return name_end;
        return k + 1;
    }
    {
        long k = j;

        while (k < len) {
            c = mark_line_at(line, k);
            if (c == '"' || c == '\'' || c == '=' || c == '<' || c == '>' || c == '`' || c <= 0x20) break;
            k++;
        }
        if (k == j) return name_end;
        return k;
    }
}

/* htmlBlockType7Regexp:
 * ^[ ]{0,3}<(/[ ]*)?([a-zA-Z]+[a-zA-Z0-9\-]*)(ATTR*)[ ]*(?:>|/>)[ ]*(?:\r\n|\n)?$
 * *is_close_tag: group 1 is exactly "/"; *has_attr: group 3 is non-empty;
 * from/to: the tag name. */
static int html_type7(MarkLine line, int *is_close_tag, int *has_attr, long *from, long *to)
{
    long i = html_open_angle(line);
    long j;
    long attrs_start;

    if (i < 0) return 0;
    *is_close_tag = 0;
    if (mark_line_at(line, i) == '/') {
        long k = i + 1;

        while (mark_line_at(line, k) == ' ') k++;
        *is_close_tag = k == i + 1;
        i = k;
    }
    j = html_tag_name(line, i);
    if (j < 0) return 0;
    *from = i;
    *to = j;
    attrs_start = j;
    for (;;) {
        long e = html_attribute(line, j);

        if (e < 0) break;
        j = e;
    }
    *has_attr = j != attrs_start;
    while (mark_line_at(line, j) == ' ') j++;
    if (mark_line_at(line, j) == '/') j++;
    if (mark_line_at(line, j) != '>') return 0;
    j++;
    while (mark_line_at(line, j) == ' ') j++;
    return only_newline_left(line, j);
}

/* htmlBlockParser.Open */
static MarkNode *html_block_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkNode *node = NULL;
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(r, &segment);
    MarkNode *last = mark_last_opened_block(ctx).node;
    long pos = ctx->block_offset;
    int typ = 0;
    long from;
    long to;

    (void)parent;
    *state = MARK_STATE_NO_CHILDREN;
    if (pos < 0 || mark_line_at(line, pos) != '<') return NULL;

    if (html_type1_open(line)) {
        typ = 1;
    } else if (html_open_literal(line, "!--")) {
        typ = 2;
    } else if (html_open_literal(line, "?")) {
        typ = 3;
    } else if (html_type4_open(line)) {
        typ = 4;
    } else if (html_open_literal(line, "![CDATA[")) {
        typ = 5;
    } else {
        int is_close_tag;
        int has_attr;

        if (html_type7(line, &is_close_tag, &has_attr, &from, &to)) {
            if (is_allowed_block_tag(line, from, to)) {
                typ = 6;
            } else if (!tag_lower_eq(line, from, to, "script") && !tag_lower_eq(line, from, to, "style") &&
                       !tag_lower_eq(line, from, to, "pre") && !(last != NULL && last->kind == MARK_PARAGRAPH) &&
                       !(is_close_tag && has_attr)) { /* type 7 cannot interrupt a paragraph */
                typ = 7;
            }
        }
    }
    if (typ == 0) {
        if (html_type6(line, &from, &to) && is_allowed_block_tag(line, from, to)) typ = 6;
    }
    if (typ == 0) return NULL;
    node = mark_node_new(ctx->arena, MARK_HTML_BLOCK);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    node->html_block_type = typ;
    mark_reader_advance(r, mark_segment_len(segment) - mark_line_trim_right_space_len(line));
    mark_lines_append(ctx, mark_node_lines(ctx->arena, node), segment);
    return node;
}

/* htmlBlockParser.Continue */
static unsigned html_block_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    CuVec *lines = mark_node_lines(ctx->arena, node);
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(r, &segment);
    CuStr closure = cu_str_empty();

    if (lines == NULL) {
        ctx->failed = 1;
        return MARK_STATE_CLOSE;
    }
    switch (node->html_block_type) {
    case 1:
        if (cu_vec_len(lines) == 1) {
            MarkLine first = mark_segment_value(mark_lines_at(lines, 0), ctx->src, ctx->src_len);

            if (html_type1_close(first)) return MARK_STATE_CLOSE;
        }
        if (html_type1_close(line)) {
            node->closure_line = segment;
            mark_reader_advance(r, mark_segment_len(segment) - mark_line_trim_right_space_len(line));
            return MARK_STATE_CLOSE;
        }
        break;
    case 2:
    case 3:
    case 4:
    case 5:
        /* htmlBlockType2Close .. Type5Close; Go reaches them by falling
         * through the cases */
        if (node->html_block_type == 2) closure = CU_LIT("-->");
        else if (node->html_block_type == 3) closure = CU_LIT("?>");
        else if (node->html_block_type == 4) closure = CU_LIT(">");
        else closure = CU_LIT("]]>");
        if (cu_vec_len(lines) == 1) {
            MarkLine first = mark_segment_value(mark_lines_at(lines, 0), ctx->src, ctx->src_len);

            if (mark_line_index(first, closure) >= 0) return MARK_STATE_CLOSE;
        }
        if (mark_line_index(line, closure) >= 0) {
            node->closure_line = segment;
            mark_reader_advance(r, mark_segment_len(segment));
            return MARK_STATE_CLOSE;
        }
        break;
    case 6:
    case 7:
        if (mark_line_is_blank(line)) return MARK_STATE_CLOSE;
        break;
    default:
        break;
    }
    mark_lines_append(ctx, lines, segment);
    mark_reader_advance(r, mark_segment_len(segment) - mark_line_trim_right_space_len(line));
    return MARK_STATE_CONTINUE | MARK_STATE_NO_CHILDREN;
}

/* ---- paragraph.go ---- */

/* paragraphParser.Open */
static MarkNode *paragraph_open(MarkCtx *ctx, MarkNode *parent, MarkReader *r, unsigned *state)
{
    MarkSegment segment;
    MarkNode *node;

    (void)parent;
    *state = MARK_STATE_NO_CHILDREN;
    mark_reader_peek_line(r, &segment);
    segment = mark_segment_trim_left_space(segment, ctx->src, ctx->src_len);
    if (mark_segment_is_empty(segment)) return NULL;
    node = mark_node_new(ctx->arena, MARK_PARAGRAPH);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    mark_lines_append(ctx, mark_node_lines(ctx->arena, node), segment);
    mark_reader_advance(r, mark_segment_len(segment) - 1);
    return node;
}

/* paragraphParser.Continue */
static unsigned paragraph_continue(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(r, &segment);

    if (mark_line_is_blank(line)) return MARK_STATE_CLOSE;
    mark_lines_append(ctx, mark_node_lines(ctx->arena, node), segment);
    mark_reader_advance(r, mark_segment_len(segment) - 1);
    return MARK_STATE_CONTINUE | MARK_STATE_NO_CHILDREN;
}

/* paragraphParser.Close: trims every line's left and the last line's
 * right. */
static void paragraph_close(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    CuVec *lines = mark_node_lines(ctx->arena, node);
    size_t len;
    size_t i;

    (void)r;
    if (lines == NULL) {
        ctx->failed = 1;
        return;
    }
    len = cu_vec_len(lines);
    if (len != 0) {
        MarkSegment *d = CU_VEC_DATA(lines, MarkSegment);

        for (i = 0; i < len; i++) d[i] = mark_segment_trim_left_space(d[i], ctx->src, ctx->src_len);
        d[len - 1] = mark_segment_trim_right_space(d[len - 1], ctx->src, ctx->src_len);
    }
    if (len == 0 && node->parent != NULL) mark_node_remove_child(node->parent, node);
}

/* ---- link_ref.go ---- */

/* The bytes of FindClosure's segments joined (link_ref.go's label and
 * title loops). */
static CuStr join_segments(MarkCtx *ctx, MarkReader *block, const CuVec *segments)
{
    size_t n = cu_vec_len(segments);

    if (n == 1) return mark_reader_value(block, ctx->arena, *CU_VEC_AT(segments, MarkSegment, 0));
    {
        CuBuilder b;
        CuStr out;
        size_t i;

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

static MarkReference reference(CuStr label, CuStr destination, CuStr title, int has_title)
{
    MarkReference ref;

    ref.label = label;
    ref.destination = destination;
    ref.title = title;
    ref.has_title = has_title;
    return ref;
}

/* parseLinkReferenceDefinition: the lines [start, end) it covered, or
 * start -1. */
static void parse_link_reference_definition(MarkCtx *ctx, MarkReader *block, long *start_out, long *end_out)
{
    MarkSegment seg;
    MarkLine line;
    long start_line;
    long width;
    long pos;
    CuVec segments;
    CuStr label;
    CuStr destination;
    int is_new_line;
    long end_line;
    long spaces;
    unsigned char opener;
    unsigned char closer;
    CuStr title;
    long chars;

    *start_out = -1;
    *end_out = -1;
    mark_reader_skip_spaces(block, &seg, &chars);
    line = mark_reader_peek_line(block, &seg);
    if (!line.valid) return;
    mark_reader_position(block, &start_line, &seg);
    width = mark_line_indent_width(line, 0, &pos);
    if (width > 3) return;
    if (width != 0) pos++;
    if (mark_line_at(line, pos) != '[') return;
    mark_reader_advance(block, pos + 1);
    if (!mark_reader_find_closure(block, ctx->arena, '[', ']', link_closure_options(), &segments)) {
        if (cu_vec_failed(&segments)) ctx->failed = 1;
        return;
    }
    label = join_segments(ctx, block, &segments);
    if (mark_is_blank((const unsigned char *)label.ptr, (long)label.len)) return;
    if (mark_reader_peek(block) != ':') return;
    mark_reader_advance(block, 1);
    mark_reader_skip_spaces(block, &seg, &chars);
    if (!mark_parse_link_destination(ctx, block, &destination)) return;
    line = mark_reader_peek_line(block, &seg);
    is_new_line = !line.valid || mark_line_is_blank(line);

    mark_reader_position(block, &end_line, &seg);
    mark_reader_skip_spaces(block, &seg, &spaces);
    opener = mark_reader_peek(block);
    if (opener != '"' && opener != '\'' && opener != '(') {
        if (!is_new_line) return;
        mark_add_reference(ctx, reference(label, destination, cu_str_empty(), 0));
        *start_out = start_line;
        *end_out = end_line + 1;
        return;
    }
    if (spaces == 0) return;
    mark_reader_advance(block, 1);
    closer = opener == '(' ? ')' : opener;
    if (!mark_reader_find_closure(block, ctx->arena, opener, closer, link_closure_options(), &segments)) {
        if (cu_vec_failed(&segments)) ctx->failed = 1;
        if (!is_new_line) return;
        mark_add_reference(ctx, reference(label, destination, cu_str_empty(), 0));
        mark_reader_advance_line(block);
        *start_out = start_line;
        *end_out = end_line + 1;
        return;
    }
    title = join_segments(ctx, block, &segments);

    line = mark_reader_peek_line(block, &seg);
    if (line.valid && !mark_line_is_blank(line)) {
        if (!is_new_line) return;
        /* goldmark keeps the title although text follows it on its line */
        mark_add_reference(ctx, reference(label, destination, title, 1));
        *start_out = start_line;
        *end_out = end_line;
        return;
    }

    mark_reader_position(block, &end_line, &seg);
    mark_add_reference(ctx, reference(label, destination, title, 1));
    *start_out = start_line;
    *end_out = end_line + 1;
}

/* Removes lines [a, b) of the vector (Segments.Sliced + SetSliced +
 * AppendAll in link_ref.go). */
static void lines_remove_range(CuVec *lines, size_t a, size_t b)
{
    size_t len = cu_vec_len(lines);
    MarkSegment *d;

    if (b > len) b = len;
    if (a >= b) return;
    d = CU_VEC_DATA(lines, MarkSegment);
    memmove(d + a, d + b, (len - b) * sizeof *d);
    mark_lines_truncate(lines, len - (b - a));
}

/* LinkReferenceParagraphTransformer.Transform */
void mark_link_reference_transform(MarkCtx *ctx, MarkNode *node, MarkReader *r)
{
    CuVec *lines = mark_node_lines(ctx->arena, node);
    MarkReader block;
    CuVec removes; /* pairs of long */
    long offset = 0;
    size_t i;

    (void)r;
    if (lines == NULL) {
        ctx->failed = 1;
        return;
    }
    mark_block_reader_init(&block, ctx->src, ctx->src_len, lines);
    CU_VEC_INIT(&removes, ctx->arena, long);
    for (;;) {
        long start;
        long end;

        if (ctx->failed) return;
        parse_link_reference_definition(ctx, &block, &start, &end);
        if (start > -1) {
            if (start == end) end++;
            CU_VEC_PUSH(&removes, &start);
            CU_VEC_PUSH(&removes, &end);
            if (cu_vec_failed(&removes)) {
                ctx->failed = 1;
                return;
            }
            continue;
        }
        break;
    }

    for (i = 0; i + 1 < cu_vec_len(&removes); i += 2) {
        long a = *CU_VEC_AT(&removes, long, i);
        long b = *CU_VEC_AT(&removes, long, i + 1);

        if (cu_vec_len(lines) == 0) break;
        lines_remove_range(lines, (size_t)(a - offset < 0 ? 0 : a - offset), (size_t)(b - offset < 0 ? 0 : b - offset));
        offset = b;
    }

    if (cu_vec_len(lines) == 0) {
        MarkNode *t = mark_node_new(ctx->arena, MARK_TEXT_BLOCK);

        if (t == NULL) {
            ctx->failed = 1;
            return;
        }
        t->blank_previous_lines = node->blank_previous_lines;
        if (node->parent != NULL) mark_node_replace_child(node->parent, node, t);
        return;
    }
    node->lines = lines;
}

/* ---- parser.DefaultBlockParsers and the extensions' block parsers ---- */

const MarkBlockParser mark_block_parsers[MARK_BP_COUNT] = {
    {setext_open, close_state, setext_close, 1, 0, "-=", 0},
    {mark_definition_list_open, mark_definition_list_continue, close_nothing, 1, 0, ":", CU_MARK_DEFINITION_LIST},
    {mark_definition_description_open, mark_definition_description_continue, mark_definition_description_close, 1,
     0, ":", CU_MARK_DEFINITION_LIST},
    {thematic_open, close_state, close_nothing, 1, 0, "-*_", 0},
    {list_open, list_continue, list_close, 1, 0, "-+*0123456789", 0},
    {list_item_open, list_item_continue, close_nothing, 1, 0, "-+*0123456789", 0},
    {code_block_open, code_block_continue, code_block_close, 0, 1, NULL, 0},
    {atx_open, close_state, atx_close, 1, 0, "#", 0},
    {fenced_open, fenced_continue, fenced_close, 1, 0, "~`", 0},
    {blockquote_open, blockquote_continue, close_nothing, 1, 0, ">", 0},
    {html_block_open, html_block_continue, close_nothing, 1, 0, "<", 0},
    {mark_footnote_block_open, mark_footnote_block_continue, mark_footnote_block_close, 1, 0, "[", CU_MARK_FOOTNOTE},
    {paragraph_open, paragraph_continue, paragraph_close, 0, 0, NULL, 0},
};
