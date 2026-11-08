/* goldmark text/segment.go and text/reader.go as C. Each function names
 * the Go function it ports. */
#include "mark_text.h"
#include "mark_util.h"

/* ---- Segments ---- */

/* text.NewSegment */
MarkSegment mark_segment(long start, long stop)
{
    MarkSegment s;

    s.start = start;
    s.stop = stop;
    s.padding = 0;
    return s;
}

/* text.NewSegmentPadding */
MarkSegment mark_segment_padding(long start, long stop, long padding)
{
    MarkSegment s;

    s.start = start;
    s.stop = stop;
    s.padding = padding;
    return s;
}

/* Segment.Len */
long mark_segment_len(MarkSegment s)
{
    return s.stop - s.start + s.padding;
}

/* Segment.IsEmpty */
int mark_segment_is_empty(MarkSegment s)
{
    return s.start >= s.stop && s.padding == 0;
}

/* Segment.Between. Go panics when the stops differ; both arguments here
 * are positions on one line, so they never do. */
MarkSegment mark_segment_between(MarkSegment s, MarkSegment other)
{
    return mark_segment_padding(s.start, other.start, s.padding - other.padding);
}

MarkSegment mark_segment_with_start(MarkSegment s, long v)
{
    return mark_segment_padding(v, s.stop, s.padding);
}

MarkSegment mark_segment_with_stop(MarkSegment s, long v)
{
    return mark_segment_padding(s.start, v, s.padding);
}

/* The source bytes [start, stop) clamped into the source, so a segment
 * that Go would index out of range reads nothing instead. */
static void clamp(MarkSegment s, long len, long *start, long *stop)
{
    long a = s.start < 0 ? 0 : s.start;
    long b = s.stop > len ? len : s.stop;

    if (a > len) a = len;
    if (b < a) b = a;
    *start = a;
    *stop = b;
}

/* Segment.Value as a view. */
MarkLine mark_segment_value(MarkSegment s, const unsigned char *src, long len)
{
    MarkLine l;
    long a;
    long b;

    clamp(s, len, &a, &b);
    l.bytes = src + a;
    l.n = b - a;
    l.pad = s.padding > 0 ? s.padding : 0;
    l.valid = 1;
    return l;
}

/* Segment.TrimRightSpace */
MarkSegment mark_segment_trim_right_space(MarkSegment s, const unsigned char *src, long len)
{
    long a;
    long b;
    long l;

    clamp(s, len, &a, &b);
    l = mark_trim_right_space_len(src + a, b - a);
    if (l == b - a) return mark_segment(s.start, s.start);
    return mark_segment_padding(s.start, s.stop - l, s.padding);
}

/* Segment.TrimLeftSpace: drops the padding too. */
MarkSegment mark_segment_trim_left_space(MarkSegment s, const unsigned char *src, long len)
{
    long a;
    long b;
    long l;

    clamp(s, len, &a, &b);
    l = mark_trim_left_space_len(src + a, b - a);
    return mark_segment(s.start + l, s.stop);
}

/* Segment.TrimLeftSpaceWidth */
MarkSegment mark_segment_trim_left_space_width(MarkSegment s, long width, const unsigned char *src, long len)
{
    long padding = s.padding;
    long start;
    long a;
    long b;
    long i;

    for (; width > 0; width--) {
        if (padding == 0) break;
        padding--;
    }
    if (width == 0) return mark_segment_padding(s.start, s.stop, padding);
    clamp(s, len, &a, &b);
    start = s.start;
    for (i = a; i < b; i++) {
        unsigned char c = src[i];

        if (start >= s.stop - 1 || width <= 0) break;
        if (c == ' ') width--;
        else if (c == '\t') width -= 4;
        else break;
        start++;
    }
    if (width < 0) padding = -width;
    return mark_segment_padding(start, s.stop, padding);
}

/* ---- Lines ---- */

static MarkLine nil_line(void)
{
    MarkLine l;

    l.bytes = NULL;
    l.n = 0;
    l.pad = 0;
    l.valid = 0;
    return l;
}

long mark_line_len(MarkLine l)
{
    return l.pad + l.n;
}

unsigned char mark_line_at(MarkLine l, long i)
{
    if (i < 0 || i >= l.pad + l.n) return 0;
    if (i < l.pad) return ' ';
    return l.bytes[i - l.pad];
}

/* line[i:] */
MarkLine mark_line_skip(MarkLine l, long i)
{
    if (i <= 0) return l;
    if (i <= l.pad) {
        l.pad -= i;
        return l;
    }
    i -= l.pad;
    l.pad = 0;
    if (i > l.n) i = l.n;
    l.bytes += i;
    l.n -= i;
    return l;
}

/* line[i:j] */
MarkLine mark_line_sub(MarkLine l, long i, long j)
{
    long len = l.pad + l.n;

    if (j > len) j = len;
    if (i < 0) i = 0;
    if (i > j) i = j;
    l = mark_line_skip(l, i);
    j -= i;
    if (j <= l.pad) {
        l.pad = j;
        l.n = 0;
    } else {
        l.n = j - l.pad;
    }
    return l;
}

/* util.IsBlank */
int mark_line_is_blank(MarkLine l)
{
    return mark_is_blank(l.bytes, l.n);
}

/* util.IndentWidth */
long mark_line_indent_width(MarkLine l, long cur, long *pos)
{
    long width = 0;
    long p = 0;
    long len = mark_line_len(l);
    long i;

    for (i = 0; i < len; i++) {
        unsigned char b = mark_line_at(l, i);

        if (b == ' ') {
            width++;
            p++;
        } else if (b == '\t') {
            width += mark_tab_width(cur + width);
            p++;
        } else {
            break;
        }
    }
    *pos = p;
    return width;
}

/* util.IndentPositionPadding (IndentPosition is the paddingv == 0 case) */
long mark_line_indent_position(MarkLine l, long cur, long paddingv, long width, long *padding)
{
    long w = 0;
    long i = 0;
    long len = mark_line_len(l);

    if (width == 0) {
        *padding = paddingv;
        return 0;
    }
    for (; i < len; i++) {
        unsigned char b = mark_line_at(l, i);

        if (b == '\t' && w < width) w += mark_tab_width(cur + w);
        else if (b == ' ' && w < width) w++;
        else break;
    }
    if (w >= width) {
        *padding = w - width;
        return i - paddingv;
    }
    *padding = -1;
    return -1;
}

/* util.FirstNonSpacePosition */
long mark_line_first_non_space(MarkLine l)
{
    long len = mark_line_len(l);
    long i;

    for (i = 0; i < len; i++) {
        unsigned char c = mark_line_at(l, i);

        if (c == ' ' || c == '\t') continue;
        if (c == '\n') return -1;
        return i;
    }
    return -1;
}

long mark_line_trim_left_space_len(MarkLine l)
{
    long len = mark_line_len(l);
    long i = 0;

    for (; i < len; i++) {
        if (!mark_byte_space(mark_line_at(l, i))) break;
    }
    return i;
}

long mark_line_trim_right_space_len(MarkLine l)
{
    long len = mark_line_len(l);
    long i = len - 1;

    for (; i >= 0; i--) {
        if (!mark_byte_space(mark_line_at(l, i))) break;
    }
    if (i < 0) return len;
    return len - 1 - i;
}

long mark_line_trim_left_len(MarkLine l, unsigned char c)
{
    long len = mark_line_len(l);
    long i = 0;

    for (; i < len && mark_line_at(l, i) == c; i++) {
    }
    return i;
}

long mark_line_trim_right_len(MarkLine l, unsigned char c)
{
    long len = mark_line_len(l);
    long i = len - 1;

    for (; i >= 0 && mark_line_at(l, i) == c; i--) {
    }
    return len - 1 - i;
}

/* bytes.Index(line, needle); needles here are a few bytes long. */
long mark_line_index(MarkLine l, CuStr needle)
{
    long len = mark_line_len(l);
    long i;

    if (needle.len == 0) return 0;
    for (i = 0; i + (long)needle.len <= len; i++) {
        size_t k;

        for (k = 0; k < needle.len; k++) {
            if (mark_line_at(l, i + (long)k) != (unsigned char)needle.ptr[k]) break;
        }
        if (k == needle.len) return i;
    }
    return -1;
}

CuStr mark_line_copy(CuArena *a, MarkLine l, long i, long j)
{
    MarkLine sub = mark_line_sub(l, i, j);
    size_t n = (size_t)(sub.pad + sub.n);
    char *p = cu_arena_push_bytes(a, n);
    long k;

    if (p == NULL) return cu_str_empty();
    for (k = 0; k < sub.pad; k++) p[k] = ' ';
    if (sub.n > 0) memcpy(p + sub.pad, sub.bytes, (size_t)sub.n);
    return cu_str_n(p, n);
}

CuStr mark_line_slice(CuArena *a, MarkLine l, long i, long j)
{
    MarkLine sub;

    if (i < l.pad && j > i) return mark_line_copy(a, l, i, j);
    sub = mark_line_sub(l, i, j);
    return cu_str_n((const char *)sub.bytes, (size_t)sub.n);
}

/* ---- Readers ---- */

/* Segments.At, returning Go's invalid segment for an index Go would panic
 * on; the parsers never ask for one unless an allocation already failed. */
static MarkSegment seg_at(const CuVec *v, long i)
{
    if (v == NULL || i < 0 || (size_t)i >= cu_vec_len(v)) return mark_segment(-1, -1);
    return *CU_VEC_AT(v, MarkSegment, (size_t)i);
}

/* reader.ResetPosition */
static void reader_reset_position(MarkReader *r)
{
    r->line = -1;
    r->head = 0;
    r->line_offset = -1;
    r->pos = mark_segment(0, 0);
    mark_reader_advance_line(r);
}

/* text.NewReader */
void mark_reader_init(MarkReader *r, const unsigned char *source, long len)
{
    r->is_block = 0;
    r->source = source;
    r->source_len = len;
    r->segments = NULL;
    r->segments_len = 0;
    r->last = 0;
    reader_reset_position(r);
}

/* blockReader.ResetPosition */
static void block_reset_position(MarkReader *r)
{
    r->line = -1;
    r->head = 0;
    r->last = 0;
    r->line_offset = -1;
    r->pos = mark_segment(-1, -1);
    if (r->segments_len > 0) r->last = seg_at(r->segments, r->segments_len - 1).stop;
    mark_reader_advance_line(r);
}

/* text.NewBlockReader */
void mark_block_reader_init(MarkReader *r, const unsigned char *source, long len, const CuVec *segments)
{
    r->is_block = 1;
    r->source = source;
    r->source_len = len;
    r->segments = NULL;
    r->segments_len = 0;
    r->last = 0;
    r->line = 0;
    r->head = 0;
    r->line_offset = -1;
    r->pos = mark_segment(-1, -1);
    if (segments != NULL) mark_reader_reset(r, segments);
}

/* blockReader.Reset */
void mark_reader_reset(MarkReader *r, const CuVec *segments)
{
    r->segments = segments;
    r->segments_len = segments == NULL ? 0 : (long)cu_vec_len(segments);
    block_reset_position(r);
}

/* Whether PeekLine returns a line (reader.PeekLine / blockReader.PeekLine). */
static int has_line(const MarkReader *r)
{
    if (r->is_block) return r->line < r->segments_len && r->pos.start >= 0 && r->pos.start < r->last;
    return r->pos.start >= 0 && r->pos.start < r->source_len;
}

/* reader.Peek / blockReader.Peek */
unsigned char mark_reader_peek(const MarkReader *r)
{
    if (!has_line(r)) return MARK_EOF;
    if (r->pos.padding != 0) return ' ';
    return r->source[r->pos.start];
}

/* reader.PeekLine / blockReader.PeekLine */
MarkLine mark_reader_peek_line(const MarkReader *r, MarkSegment *seg)
{
    *seg = r->pos;
    if (!has_line(r)) return nil_line();
    return mark_segment_value(r->pos, r->source, r->source_len);
}

/* The rune ending just before source[start]; '\n' when there is none. */
static unsigned long rune_before(const unsigned char *source, long len, long start)
{
    long i = start - 1;
    long size;

    for (; i < len && i >= 0; i--) {
        if (mark_rune_start(source[i])) break;
    }
    if (i < 0 || i >= len) return '\n';
    return mark_decode_rune(source + i, len - i, &size);
}

/* reader.PrecendingCharacter / blockReader.PrecendingCharacter */
unsigned long mark_reader_preceding(const MarkReader *r)
{
    if (r->is_block) {
        MarkSegment first;

        if (r->pos.padding != 0) return ' ';
        if (r->segments_len < 1) return '\n';
        first = seg_at(r->segments, 0);
        if (r->line == 0 && r->pos.start <= first.start) return '\n';
        return rune_before(r->source, r->source_len, r->pos.start);
    }
    if (r->pos.start <= 0) {
        if (r->pos.padding != 0) return ' ';
        return '\n';
    }
    return rune_before(r->source, r->source_len, r->pos.start);
}

/* reader.LineOffset / blockReader.LineOffset */
long mark_reader_line_offset(MarkReader *r)
{
    if (r->line_offset < 0) {
        long v = 0;
        long i;

        for (i = r->head; i < r->pos.start; i++) {
            if (i >= 0 && i < r->source_len && r->source[i] == '\t') v += mark_tab_width(v);
            else v++;
        }
        r->line_offset = v - r->pos.padding;
    }
    return r->line_offset;
}

void mark_reader_position(const MarkReader *r, long *line, MarkSegment *pos)
{
    *line = r->line;
    *pos = r->pos;
}

/* reader.SetPosition / blockReader.SetPosition */
void mark_reader_set_position(MarkReader *r, long line, MarkSegment pos)
{
    r->line_offset = -1;
    r->line = line;
    if (!r->is_block) {
        r->pos = pos;
        return;
    }
    if (pos.start == -1) {
        if (r->line < r->segments_len) {
            MarkSegment s = seg_at(r->segments, line);

            r->head = s.start;
            r->pos = s;
        }
    } else {
        r->pos = pos;
        if (r->line < r->segments_len) r->head = seg_at(r->segments, line).start;
    }
}

/* reader.SetPadding keeps a cached LineOffset; blockReader.SetPadding
 * drops it. */
void mark_reader_set_padding(MarkReader *r, long v)
{
    if (r->is_block) r->line_offset = -1;
    r->pos.padding = v;
}

/* reader.AdvanceLine / blockReader.AdvanceLine */
void mark_reader_advance_line(MarkReader *r)
{
    long i;

    if (r->is_block) {
        mark_reader_set_position(r, r->line + 1, mark_segment(-1, -1));
        r->head = r->pos.start;
        return;
    }
    r->line_offset = -1;
    r->pos.start = r->pos.stop;
    r->head = r->pos.start;
    if (r->pos.start < 0) return;
    r->pos.stop = r->source_len;
    for (i = r->pos.start; i < r->source_len; i++) {
        if (r->source[i] == '\n') {
            r->pos.stop = i + 1;
            break;
        }
    }
    r->line++;
    r->pos.padding = 0;
}

/* reader.Advance / blockReader.Advance. Go's reader takes its fast path
 * when n is under the length of the line it last peeked; that line always
 * ends at the current line's end, so the test is n < stop - start. */
void mark_reader_advance(MarkReader *r, long n)
{
    r->line_offset = -1;
    if (n < r->pos.stop - r->pos.start && r->pos.padding == 0) {
        r->pos.start += n;
        return;
    }
    if (r->is_block) {
        for (; n > 0; n--) {
            if (r->pos.padding != 0) {
                r->pos.padding--;
                continue;
            }
            if (r->pos.start >= r->pos.stop - 1 && r->pos.stop < r->last) {
                mark_reader_advance_line(r);
                continue;
            }
            r->pos.start++;
        }
        return;
    }
    for (; n > 0 && r->pos.start < r->source_len; n--) {
        if (r->pos.padding != 0) {
            r->pos.padding--;
            continue;
        }
        if (r->pos.start >= 0 && r->source[r->pos.start] == '\n') {
            mark_reader_advance_line(r);
            continue;
        }
        r->pos.start++;
    }
}

/* Reader.AdvanceAndSetPadding */
void mark_reader_advance_and_set_padding(MarkReader *r, long n, long padding)
{
    mark_reader_advance(r, n);
    if (padding > r->pos.padding) mark_reader_set_padding(r, padding);
}

/* skipSpacesReader */
int mark_reader_skip_spaces(MarkReader *r, MarkSegment *seg, long *chars)
{
    *chars = 0;
    for (;;) {
        MarkLine line = mark_reader_peek_line(r, seg);
        long len;
        long i;

        if (!line.valid) return 0;
        len = mark_line_len(line);
        for (i = 0; i < len; i++) {
            if (mark_byte_space(mark_line_at(line, i))) {
                (*chars)++;
                mark_reader_advance(r, 1);
                continue;
            }
            *seg = mark_segment_with_start(*seg, seg->start + i + 1);
            return 1;
        }
    }
}

/* skipBlankLinesReader */
int mark_reader_skip_blank_lines(MarkReader *r, MarkSegment *seg, long *lines)
{
    *lines = 0;
    for (;;) {
        MarkLine line = mark_reader_peek_line(r, seg);

        if (!line.valid) return 0;
        if (mark_line_is_blank(line)) {
            (*lines)++;
            mark_reader_advance_line(r);
        } else {
            return 1;
        }
    }
}

/* reader.Value / blockReader.Value */
CuStr mark_reader_value(MarkReader *r, CuArena *a, MarkSegment seg)
{
    CuBuilder b;
    CuStr out;
    long line;
    long i;

    if (!r->is_block) {
        MarkLine l = mark_segment_value(seg, r->source, r->source_len);

        return mark_line_copy(a, l, 0, mark_line_len(l));
    }
    cu_sb_init(&b, a);
    line = r->segments_len - 1;
    for (; line >= 0; line--) {
        if (seg.start >= seg_at(r->segments, line).start) break;
    }
    if (line < 0) line = 0; /* Go would index At(-1) here */
    i = seg.start;
    for (; line < r->segments_len; line++) {
        MarkSegment s = seg_at(r->segments, line);
        long k;

        if (i < 0) i = s.start;
        for (k = 0; k < s.padding; k++) cu_sb_char(&b, ' ');
        for (; i < seg.stop && i < s.stop; i++) {
            if (i >= 0 && i < r->source_len) cu_sb_char(&b, (char)r->source[i]);
        }
        i = -1;
        if (s.stop > seg.stop) break;
    }
    if (!cu_sb_finish(&b, &out)) return cu_str_empty();
    return out;
}

/* findClosureReader */
int mark_reader_find_closure(MarkReader *r, CuArena *a, unsigned char opener, unsigned char closer,
                             MarkClosureOptions opts, CuVec *out)
{
    long opened = 1;
    long code_span_opener = 0;
    int closed = 0;
    long orgline;
    MarkSegment orgpos;

    CU_VEC_INIT(out, a, MarkSegment);
    mark_reader_position(r, &orgline, &orgpos);
    for (;;) {
        MarkSegment seg;
        MarkLine bs = mark_reader_peek_line(r, &seg);
        long len;
        long i = 0;

        if (!bs.valid) break;
        len = mark_line_len(bs);
        while (i < len) {
            unsigned char c = mark_line_at(bs, i);

            if (opts.code_span && code_span_opener != 0 && c == '`') {
                long code_span_closer = 0;

                for (; i < len; i++) {
                    if (mark_line_at(bs, i) == '`') {
                        code_span_closer++;
                    } else {
                        i--;
                        break;
                    }
                }
                if (code_span_closer == code_span_opener) code_span_opener = 0;
            } else if (code_span_opener == 0 && c == '\\' && i < len - 1 && mark_byte_punct(mark_line_at(bs, i + 1))) {
                i += 2;
                continue;
            } else if (opts.code_span && code_span_opener == 0 && c == '`') {
                for (; i < len; i++) {
                    if (mark_line_at(bs, i) == '`') {
                        code_span_opener++;
                    } else {
                        i--;
                        break;
                    }
                }
            } else if ((opts.code_span && code_span_opener == 0) || !opts.code_span) {
                if (c == closer) {
                    opened--;
                    if (opened == 0) {
                        MarkSegment last = mark_segment_with_stop(seg, seg.start + i);

                        CU_VEC_PUSH(out, &last);
                        mark_reader_advance(r, i + 1);
                        closed = 1;
                        goto end;
                    }
                } else if (c == opener) {
                    if (!opts.nesting) goto end;
                    opened++;
                }
            }
            i++;
        }
        if (!opts.newline) break;
        mark_reader_advance_line(r);
        CU_VEC_PUSH(out, &seg);
    }
end:
    if (!opts.advance) mark_reader_set_position(r, orgline, orgpos);
    if (closed && !cu_vec_failed(out)) return 1;
    cu_vec_clear(out);
    return 0;
}
