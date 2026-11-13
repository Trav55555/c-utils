/* goldmark's inline extensions as C: extension/strikethrough.go,
 * tasklist.go, linkify.go, typographer.go and the inline parser of
 * footnote.go. Each function names the Go function it ports. The regular
 * expressions of linkify.go and tasklist.go are written out as matchers
 * that quote the expression they stand for; the linkify ones argue their
 * equivalence with RE2's leftmost-first match. */
#include "mark_parser.h"
#include "mark_unicode.h"
#include "mark_util.h"

/* The current line's bytes as one view: a padded line is copied. */
static CuStr line_bytes(MarkCtx *ctx, MarkLine line)
{
    if (line.pad == 0) return cu_str_n((const char *)line.bytes, (size_t)line.n);
    return mark_line_copy(ctx->arena, line, 0, mark_line_len(line));
}

static unsigned char byte_at(CuStr s, long i)
{
    if (i < 0 || i >= (long)s.len) return 0;
    return (unsigned char)s.ptr[i];
}

/* util.ToRune(line, pos) */
static unsigned long rune_at(CuStr s, long pos)
{
    return mark_to_rune((const unsigned char *)s.ptr, (long)s.len, pos);
}

/* ---- strikethrough.go ---- */

/* strikethroughParser.Parse: ScanDelimiter with min 2 and the
 * strikethrough processor (IsDelimiter: '~'). */
MarkNode *mark_strikethrough_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block)
{
    unsigned long before = mark_reader_preceding(block);
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(block, &segment);
    int can_open;
    int can_close;
    long j;
    MarkNode *node;

    (void)parent;
    if (mark_line_at(line, 0) != '~') return NULL;
    j = mark_scan_delimiter_flags(line, before, &can_open, &can_close);
    if (j < 2) return NULL;
    node = mark_node_new(ctx->arena, MARK_DELIMITER);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    node->can_open = can_open;
    node->can_close = can_close;
    node->length = j;
    node->original_length = j;
    node->delim_char = '~';
    node->segment = mark_segment_with_stop(segment, segment.start + j);
    mark_reader_advance(block, j);
    mark_push_delimiter(ctx, node);
    return node;
}

/* ---- tasklist.go ---- */

/* RE2's \s: [\t\n\f\r ] */
static int re_space(unsigned char c)
{
    return c == '\t' || c == '\n' || c == 0x0C || c == '\r' || c == ' ';
}

/* taskCheckBoxParser.Parse. taskListRegexp ^\[([\sxX])\]\s* : the end of
 * the match is after the greedy run of \s following "]". */
MarkNode *mark_task_checkbox_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block)
{
    MarkSegment segment;
    MarkLine line;
    long len;
    unsigned char value;
    long end;
    MarkNode *node;

    /* - List / - ListItem (parent.Parent) / - TextBlock (parent) */
    if (parent->parent == NULL || parent->parent->first != parent) return NULL;
    if (parent->first != NULL) return NULL;
    if (parent->parent->kind != MARK_LIST_ITEM) return NULL;
    line = mark_reader_peek_line(block, &segment);
    len = mark_line_len(line);
    if (len < 3 || mark_line_at(line, 0) != '[' || mark_line_at(line, 2) != ']') return NULL;
    value = mark_line_at(line, 1);
    if (!(re_space(value) || value == 'x' || value == 'X')) return NULL;
    end = 3;
    while (end < len && re_space(mark_line_at(line, end))) end++;
    node = mark_node_new(ctx->arena, MARK_TASK_CHECKBOX);
    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    mark_reader_advance(block, end);
    node->is_checked = value == 'x' || value == 'X';
    return node;
}

/* ---- linkify.go ---- */

static int is_lower(unsigned char c)
{
    return c >= 'a' && c <= 'z';
}

/* The host class of both expressions: [-a-zA-Z0-9@:%._\+~#=] */
static int host_byte(unsigned char c)
{
    return mark_byte_alnum(c) || c == '-' || c == '@' || c == ':' || c == '%' || c == '.' || c == '_' || c == '+' ||
           c == '~' || c == '#' || c == '=';
}

/* The path class. urlRegexp: [-a-zA-Z0-9@:%_+.~#$!?&/=\(\);,'">\^{}\[\]`]
 * wwwURLRegxp: the same without '$'. */
static int path_byte(unsigned char c, int dollar)
{
    if (mark_byte_alnum(c)) return 1;
    switch (c) {
    case '-':
    case '@':
    case ':':
    case '%':
    case '_':
    case '+':
    case '.':
    case '~':
    case '#':
    case '!':
    case '?':
    case '&':
    case '/':
    case '=':
    case '(':
    case ')':
    case ';':
    case ',':
    case '\'':
    case '"':
    case '>':
    case '^':
    case '{':
    case '}':
    case '[':
    case ']':
    case '`':
        return 1;
    case '$':
        return dollar;
    default:
        return 0;
    }
}

/* The part the two expressions share after their prefix, from i:
 *   [HOST]{1,256}\.[a-z]+(?::\d+)?(?:[/#?][PATH]*)?
 * with the port group only in urlRegexp (port) and '$' only in its path
 * class (dollar). Returns the end of the match, or -1.
 *
 * RE2 finds the leftmost-first match: the match a backtracking matcher
 * reaches first, trying each greedy quantifier at its longest first. The
 * host run is tried from min(256, run) down; for a length L the next
 * element needs line[i+L] == '.' and a lower-case letter after it, and once
 * that holds the rest of the pattern cannot fail (every later group is
 * optional and takes the greedy length, since no shorter choice lets a
 * later element match: [a-z]+ is followed by ':' or [/#?] or the end, \d+
 * by [/#?] or the end, and [PATH]* ends the pattern). So the first L from
 * the top with a ".[a-z]" at its end gives the match, and the rest is a
 * greedy scan. Every class member is ASCII, so bytes stand for runes: a
 * multi-byte or invalid sequence never matches. */
static long match_url_rest(CuStr s, long i, int port, int dollar)
{
    long len = (long)s.len;
    long run = 0;
    long l;
    long j;

    while (i + run < len && run < 257 && host_byte(byte_at(s, i + run))) run++;
    for (l = run > 256 ? 256 : run; l >= 1; l--) {
        if (byte_at(s, i + l) == '.' && i + l + 1 < len && is_lower(byte_at(s, i + l + 1))) break;
    }
    if (l < 1) return -1;
    j = i + l + 1;
    while (j < len && is_lower(byte_at(s, j))) j++;
    if (port && byte_at(s, j) == ':' && mark_byte_numeric(byte_at(s, j + 1))) {
        j++;
        while (j < len && mark_byte_numeric(byte_at(s, j))) j++;
    }
    if (j < len && (byte_at(s, j) == '/' || byte_at(s, j) == '#' || byte_at(s, j) == '?')) {
        j++;
        while (j < len && path_byte(byte_at(s, j), dollar)) j++;
    }
    return j;
}

static int has_prefix(CuStr s, const char *p)
{
    return cu_str_starts_with(s, cu_str_cstr(p));
}

/* urlRegexp ^(?:http|https|ftp)://... : the alternation is followed by the
 * literal "://", so the branch is forced by the input. */
static long match_url(CuStr s)
{
    long i;

    if (has_prefix(s, "http://")) i = 7;
    else if (has_prefix(s, "https://")) i = 8;
    else if (has_prefix(s, "ftp://")) i = 6;
    else return -1;
    return match_url_rest(s, i, 1, 1);
}

/* wwwURLRegxp ^www\.... */
static long match_www(CuStr s)
{
    if (!has_prefix(s, "www.")) return -1;
    return match_url_rest(s, 4, 0, 0);
}

/* linkifyParser.Parse with the default configuration (no allowed-protocol
 * list, the default URL and www expressions, util.FindEmailIndex). */
MarkNode *mark_linkify_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block)
{
    MarkSegment segment;
    MarkLine line;
    CuStr s;
    long consumes = 0;
    long start;
    unsigned char c;
    long m = -1;
    CuStr protocol = cu_str_empty();
    int typ = MARK_AUTOLINK_URL;
    long i;
    MarkNode *n;
    MarkNode *link;

    if (ctx->link_label_state != NULL) return NULL; /* pc.IsInLinkLabel() */
    line = mark_reader_peek_line(block, &segment);
    start = segment.start;
    c = mark_line_at(line, 0);
    /* advance if current position is not a line head */
    if (c == ' ' || c == '*' || c == '_' || c == '~' || c == '(') {
        consumes++;
        start++;
        line = mark_line_skip(line, 1);
    }
    s = line_bytes(ctx, line);
    if (ctx->failed) return NULL;
    if (has_prefix(s, "http:") || has_prefix(s, "https:") || has_prefix(s, "ftp:")) m = match_url(s);
    if (m < 0 && has_prefix(s, "www.")) {
        m = match_www(s);
        protocol = CU_LIT("http");
    }
    if (m >= 0) {
        unsigned char last_char = byte_at(s, m - 1);

        if (last_char == '.') {
            m--;
        } else if (last_char == ')') {
            long closing = 0;

            for (i = m - 1; i >= 0; i--) {
                if (byte_at(s, i) == ')') closing++;
                else if (byte_at(s, i) == '(') closing--;
            }
            if (closing > 0) m -= closing;
        } else if (last_char == ';') {
            i = m - 2;
            for (; i >= 0; i--) {
                if (!mark_byte_alnum(byte_at(s, i))) break;
            }
            /* i is -1 only when everything before ';' is alphanumeric,
             * which "://" and "www." rule out; Go would index line[-1] */
            if (i != m - 2 && i >= 0) {
                if (byte_at(s, i) == '&') m = i;
            }
        }
    }
    if (m < 0) {
        long stop;
        long at;
        long k;
        int dot = 0;

        if (s.len > 0 && mark_byte_punct(byte_at(s, 0))) return NULL;
        typ = MARK_AUTOLINK_EMAIL;
        stop = mark_find_email_index((const unsigned char *)s.ptr, (long)s.len);
        if (stop < 0) return NULL;
        for (at = 0; at < (long)s.len && byte_at(s, at) != '@'; at++) {
        }
        /* bytes.IndexByte(line[at:stop-1], '.'): FindEmailIndex puts the
         * '@' before a domain of at least one byte, so at < stop - 1 */
        for (k = at; k < stop - 1; k++) {
            if (byte_at(s, k) == '.') dot = 1;
        }
        if (!dot) return NULL;
        if (byte_at(s, stop - 1) == '.') stop--;
        if (stop < (long)s.len) {
            unsigned char next_char = byte_at(s, stop);

            if (next_char == '-' || next_char == '_') return NULL;
        }
        m = stop;
    }
    if (consumes != 0) {
        if (!mark_merge_or_append_text(ctx->arena, parent, mark_segment_with_stop(segment, segment.start + 1))) {
            ctx->failed = 1;
            return NULL;
        }
    }
    i = m - 1;
    for (; i > 0; i--) {
        switch (byte_at(s, i)) {
        case '?':
        case '!':
        case '.':
        case ',':
        case ':':
        case '*':
        case '_':
        case '~':
            continue;
        default:
            break;
        }
        break;
    }
    i++;
    consumes += i;
    mark_reader_advance(block, consumes);
    n = mark_text_new(ctx->arena, mark_segment(start, start + i));
    link = mark_node_new(ctx->arena, MARK_AUTO_LINK);
    if (n == NULL || link == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    link->autolink_type = typ;
    link->autolink_value = n;
    link->protocol = protocol;
    return link;
}

/* ---- typographer.go ---- */

/* newDefaultSubstitutions (Hugo's defaults are the same entities) */
static const char *const sub_left_single = "&lsquo;";
static const char *const sub_right_single = "&rsquo;";
static const char *const sub_left_double = "&ldquo;";
static const char *const sub_right_double = "&rdquo;";
static const char *const sub_en_dash = "&ndash;";
static const char *const sub_em_dash = "&mdash;";
static const char *const sub_ellipsis = "&hellip;";
static const char *const sub_left_angle = "&laquo;";
static const char *const sub_right_angle = "&raquo;";
static const char *const sub_apostrophe = "&rsquo;";

/* gast.NewString(substitution) with SetCode(true), advancing n bytes */
static MarkNode *substitution(MarkCtx *ctx, MarkReader *block, const char *value, long n)
{
    MarkNode *node = mark_node_new(ctx->arena, MARK_STRING);

    if (node == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    node->value = cu_str_cstr(value);
    node->flags |= MARK_TEXT_CODE;
    mark_reader_advance(block, n);
    return node;
}

/* typographerParser.Parse. Byte tests use util.IsPunct/IsSpace (the ASCII
 * tables); rune tests use Go's unicode package, as the Go mixes them. */
MarkNode *mark_typographer_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block)
{
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(block, &segment);
    CuStr s;
    long len;
    unsigned char c;
    unsigned long before;
    int can_open;
    int can_close;

    (void)parent;
    s = line_bytes(ctx, line);
    if (ctx->failed) return NULL;
    len = (long)s.len;
    c = byte_at(s, 0);
    if (len > 2) {
        if (c == '-') {
            if (byte_at(s, 1) == '-' && byte_at(s, 2) == '-') return substitution(ctx, block, sub_em_dash, 3); /* --- */
        } else if (c == '.') {
            if (byte_at(s, 1) == '.' && byte_at(s, 2) == '.') return substitution(ctx, block, sub_ellipsis, 3); /* ... */
            return NULL;
        }
    }
    if (len > 1) {
        if (c == '<') {
            if (byte_at(s, 1) == '<') return substitution(ctx, block, sub_left_angle, 2); /* << */
            return NULL;
        } else if (c == '>') {
            if (byte_at(s, 1) == '>') return substitution(ctx, block, sub_right_angle, 2); /* >> */
            return NULL;
        } else if (c == '-' && byte_at(s, 1) == '-') {
            return substitution(ctx, block, sub_en_dash, 2); /* -- */
        }
    }
    if (c != '\'' && c != '"') return NULL;
    before = mark_reader_preceding(block);
    /* ScanDelimiter(line, before, 1, ...) over the padded line: a run of
     * at least one always exists, so it never returns nil */
    mark_scan_delimiter_flags(line, before, &can_open, &can_close);
    if (c == '\'') {
        /* Handle decade abbrevations such as '90s */
        if (can_open && !can_close && len > 3 && mark_byte_numeric(byte_at(s, 1)) && mark_byte_numeric(byte_at(s, 2)) &&
            byte_at(s, 3) == 's') {
            unsigned long after = ' ';

            if (len > 4) after = rune_at(s, 4);
            if (len == 3 || mark_rune_space(after) || mark_rune_punct(after)) {
                return substitution(ctx, block, sub_apostrophe, 1);
            }
        }
        /* special cases: 'twas, 'em, 'net */
        if (len > 1 && (mark_is_punct(before) || mark_is_space(before)) &&
            (byte_at(s, 1) == 't' || byte_at(s, 1) == 'e' || byte_at(s, 1) == 'n' || byte_at(s, 1) == 'l')) {
            return substitution(ctx, block, sub_apostrophe, 1);
        }
        /* any apostrophe in between two alphanumerics */
        if (len > 1 && (mark_is_digit(before) || mark_is_letter(before)) && mark_is_letter(rune_at(s, 1))) {
            return substitution(ctx, block, sub_apostrophe, 1);
        }
        if (can_open && !can_close) {
            const char *nt = sub_left_single;
            unsigned char c1 = byte_at(s, 1);
            unsigned char c2 = byte_at(s, 2);

            /* special cases: Alice's, I'm, Don't, You'd */
            if (len > 1 && (c1 == 's' || c1 == 'm' || c1 == 't' || c1 == 'd') &&
                (len < 3 || mark_byte_punct(c2) || mark_byte_space(c2))) {
                nt = sub_right_single;
            }
            /* special cases: I've, I'll, You're */
            if (len > 2 && ((c1 == 'v' && c2 == 'e') || (c1 == 'l' && c2 == 'l') || (c1 == 'r' && c2 == 'e')) &&
                (len < 4 || mark_byte_punct(byte_at(s, 3)) || mark_byte_space(byte_at(s, 3)))) {
                nt = sub_right_single;
            }
            if (nt == sub_left_single) ctx->typo_single++;
            return substitution(ctx, block, nt, 1);
        }
        /* plural possesive and abbreviations: Smiths', doin'. Go's
         * condition is (len > 1 && IsSpace(rune0)) || (IsPunct(rune0) &&
         * len > 2 && !IsDigit(rune1)); rune0 is the apostrophe, a
         * punctuation and not a space. */
        if (len > 2 && !mark_is_digit(rune_at(s, 1))) return substitution(ctx, block, sub_right_single, 1);
        if (ctx->typo_single > 0) {
            int is_close = can_close && !can_open;
            int maybe_close = can_close && can_open && len > 1 && mark_is_punct(rune_at(s, 1)) &&
                              (len == 2 || ((len > 2 && mark_byte_punct(byte_at(s, 2))) || mark_byte_space(byte_at(s, 2))));

            if (is_close || maybe_close) {
                ctx->typo_single--;
                return substitution(ctx, block, sub_right_single, 1);
            }
        }
        return NULL;
    }
    /* c == '"' */
    if (can_open && !can_close) {
        ctx->typo_double++;
        return substitution(ctx, block, sub_left_double, 1);
    }
    if (ctx->typo_double > 0) {
        int is_close = can_close && !can_open;
        int maybe_close = can_close && can_open && len > 1 && mark_is_punct(rune_at(s, 1)) &&
                          (len == 2 || ((len > 2 && mark_byte_punct(byte_at(s, 2))) || mark_byte_space(byte_at(s, 2))));

        if (is_close || maybe_close) {
            /* special case: "Monitor 21"" */
            if (len > 1 && byte_at(s, 1) == '"' && mark_is_digit(before)) return NULL;
            ctx->typo_double--;
            return substitution(ctx, block, sub_right_double, 1);
        }
    }
    return NULL;
}

/* ---- footnote.go (inlines) ---- */

/* util.FindClosure with codeSpan and allowNesting false, over the line
 * from index from: the index (relative to from) of the closer, or -1. */
static long find_closure(MarkLine line, long from, unsigned char opener, unsigned char closer)
{
    long len = mark_line_len(line) - from;
    long i = 0;
    long opened = 1;

    while (i < len) {
        unsigned char ch = mark_line_at(line, from + i);

        if (ch == '\\' && i < len - 1 && mark_byte_punct(mark_line_at(line, from + i + 1))) {
            i += 2;
            continue;
        }
        if (ch == closer) {
            opened--;
            if (opened == 0) return i;
        } else if (ch == opener) {
            return -1; /* !allowNesting */
        }
        i++;
    }
    return -1;
}

/* footnoteParser.Parse. The label is read at segment.Start plus an index
 * into the padded line, as Go does (off by the padding when there is any). */
MarkNode *mark_footnote_parse(MarkCtx *ctx, MarkNode *parent, MarkReader *block)
{
    MarkSegment segment;
    MarkLine line = mark_reader_peek_line(block, &segment);
    long len = mark_line_len(line);
    long pos = 1;
    long open;
    long closure;
    long closes;
    CuStr value;
    MarkNode *list;
    MarkNode *def;
    long index = 0;
    MarkNode *fnlink;

    if (len > 0 && mark_line_at(line, 0) == '!') pos++;
    if (pos >= len || mark_line_at(line, pos) != '^') return NULL;
    pos++;
    if (pos >= len) return NULL;
    open = pos;
    closure = find_closure(line, pos, '[', ']');
    if (closure < 0) return NULL;
    closes = pos + closure;
    value = mark_reader_value(block, ctx->arena, mark_segment(segment.start + open, segment.start + closes));
    if (cu_arena_failed(ctx->arena)) {
        ctx->failed = 1;
        return NULL;
    }
    mark_reader_advance(block, closes + 1);

    list = ctx->footnote_list;
    if (list == NULL) return NULL;
    for (def = list->first; def != NULL; def = def->next) {
        if (cu_str_eq(def->ref, value)) {
            if (def->index < 0) {
                list->count++;
                def->index = list->count;
            }
            index = def->index;
            break;
        }
    }
    if (index == 0) return NULL;

    fnlink = mark_node_new(ctx->arena, MARK_FOOTNOTE_LINK);
    if (fnlink == NULL) {
        ctx->failed = 1;
        return NULL;
    }
    fnlink->index = index;
    CU_VEC_PUSH(&ctx->footnote_links, &fnlink);
    if (cu_vec_failed(&ctx->footnote_links)) {
        ctx->failed = 1;
        return NULL;
    }
    if (mark_line_at(line, 0) == '!') {
        MarkNode *t = mark_text_new(ctx->arena, mark_segment(segment.start, segment.start + 1));

        if (t == NULL) {
            ctx->failed = 1;
            return NULL;
        }
        mark_node_append_child(parent, t);
    }
    return fnlink;
}
