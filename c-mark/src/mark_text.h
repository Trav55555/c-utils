#ifndef MARK_TEXT_H
#define MARK_TEXT_H
#include "cu.h"

/* goldmark's text package: segments (text/segment.go) and the two readers
 * (text/reader.go). Positions are long, as Go's ints are; goldmark uses -1
 * for "none" in several places and this port keeps those values. */

/* text.Segment: bytes [start, stop) of the source, preceded by padding
 * virtual spaces (the part of a tab that an indent split). */
typedef struct MarkSegment {
    long start;
    long stop;
    long padding;
} MarkSegment;

/* A line as goldmark's PeekLine returns it: padding spaces followed by
 * n source bytes. Go builds a fresh byte slice when padding is nonzero;
 * this view indexes the same bytes without copying. valid is 0 for Go's
 * nil line. */
typedef struct MarkLine {
    const unsigned char *bytes;
    long n;
    long pad;
    int valid;
} MarkLine;

/* text.Segments: a CuVec of MarkSegment. */

MarkSegment mark_segment(long start, long stop);
MarkSegment mark_segment_padding(long start, long stop, long padding);
/* Segment.Len */
long mark_segment_len(MarkSegment s);
/* Segment.IsEmpty */
int mark_segment_is_empty(MarkSegment s);
/* Segment.Between */
MarkSegment mark_segment_between(MarkSegment s, MarkSegment other);
/* Segment.WithStart, WithStop */
MarkSegment mark_segment_with_start(MarkSegment s, long v);
MarkSegment mark_segment_with_stop(MarkSegment s, long v);
/* Segment.TrimRightSpace, TrimLeftSpace, TrimLeftSpaceWidth */
MarkSegment mark_segment_trim_right_space(MarkSegment s, const unsigned char *src, long len);
MarkSegment mark_segment_trim_left_space(MarkSegment s, const unsigned char *src, long len);
MarkSegment mark_segment_trim_left_space_width(MarkSegment s, long width, const unsigned char *src, long len);
/* Segment.Value as a line view (padding spaces then the bytes). */
MarkLine mark_segment_value(MarkSegment s, const unsigned char *src, long len);

/* Line views. Indices count the padding; out-of-range reads give 0. */
long mark_line_len(MarkLine l);
unsigned char mark_line_at(MarkLine l, long i);
/* line[i:] */
MarkLine mark_line_skip(MarkLine l, long i);
/* line[i:j] */
MarkLine mark_line_sub(MarkLine l, long i, long j);
/* util.IsBlank(line) */
int mark_line_is_blank(MarkLine l);
/* util.IndentWidth(line, pos): width and byte position of the indent */
long mark_line_indent_width(MarkLine l, long cur, long *pos);
/* util.IndentPositionPadding(line, cur, padding, width) */
long mark_line_indent_position(MarkLine l, long cur, long paddingv, long width, long *padding);
/* util.FirstNonSpacePosition */
long mark_line_first_non_space(MarkLine l);
/* util.TrimLeftSpaceLength, TrimRightSpaceLength, TrimLeftLength({c}),
 * TrimRightLength({c}) on a line */
long mark_line_trim_left_space_len(MarkLine l);
long mark_line_trim_right_space_len(MarkLine l);
long mark_line_trim_left_len(MarkLine l, unsigned char c);
long mark_line_trim_right_len(MarkLine l, unsigned char c);
/* bytes.Index(line, needle) */
long mark_line_index(MarkLine l, CuStr needle);
/* Copies line[i:j] into a (padding materialised as spaces). On an arena
 * failure the result is empty and the arena is marked. */
CU_MUST_USE CuStr mark_line_copy(CuArena *a, MarkLine l, long i, long j);
/* line[i:j] as a view into the source when it holds no padding, else a
 * copy as mark_line_copy makes. */
CU_MUST_USE CuStr mark_line_slice(CuArena *a, MarkLine l, long i, long j);

/* text.Reader (over the whole source) and text.BlockReader (over a block's
 * line segments), one struct with is_block choosing the behaviour, as the
 * Go functions shared by both (skipSpacesReader, findClosureReader, ...)
 * expect. Go's EOF byte is 0xff. */
#define MARK_EOF 0xFF

typedef struct MarkReader {
    int is_block;
    const unsigned char *source;
    long source_len;
    long line;
    MarkSegment pos;
    long head;
    long line_offset; /* -1: not computed since the last move */
    const CuVec *segments; /* block reader: the lines */
    long segments_len;
    long last; /* block reader: stop of the last segment */
} MarkReader;

/* text.NewReader */
void mark_reader_init(MarkReader *r, const unsigned char *source, long len);
/* text.NewBlockReader; segments may be NULL (then Reset before use). */
void mark_block_reader_init(MarkReader *r, const unsigned char *source, long len, const CuVec *segments);
/* BlockReader.Reset */
void mark_reader_reset(MarkReader *r, const CuVec *segments);
/* Reader.Peek */
unsigned char mark_reader_peek(const MarkReader *r);
/* Reader.PeekLine; *seg gets the position either way */
MarkLine mark_reader_peek_line(const MarkReader *r, MarkSegment *seg);
/* Reader.PrecendingCharacter */
unsigned long mark_reader_preceding(const MarkReader *r);
/* Reader.LineOffset */
long mark_reader_line_offset(MarkReader *r);
/* Reader.Position / SetPosition / SetPadding */
void mark_reader_position(const MarkReader *r, long *line, MarkSegment *pos);
void mark_reader_set_position(MarkReader *r, long line, MarkSegment pos);
void mark_reader_set_padding(MarkReader *r, long v);
/* Reader.Advance, AdvanceAndSetPadding, AdvanceLine */
void mark_reader_advance(MarkReader *r, long n);
void mark_reader_advance_and_set_padding(MarkReader *r, long n, long padding);
void mark_reader_advance_line(MarkReader *r);
/* Reader.SkipSpaces: returns 0 at EOF; *chars counts the spaces skipped */
int mark_reader_skip_spaces(MarkReader *r, MarkSegment *seg, long *chars);
/* Reader.SkipBlankLines: returns 0 at EOF; *lines counts the lines */
int mark_reader_skip_blank_lines(MarkReader *r, MarkSegment *seg, long *lines);
/* Reader.Value(segment), copied into a. For a block reader this is
 * blockReader.Value: the bytes of the lines the segment covers, each
 * line's padding included. */
CU_MUST_USE CuStr mark_reader_value(MarkReader *r, CuArena *a, MarkSegment seg);

/* text.FindClosureOptions */
typedef struct MarkClosureOptions {
    int code_span;
    int nesting;
    int newline;
    int advance;
} MarkClosureOptions;

/* Reader.FindClosure. On success returns 1 and leaves the segments in
 * *out (a CuVec of MarkSegment in a); *out is cleared otherwise. A
 * failed allocation marks a and out. */
CU_MUST_USE int mark_reader_find_closure(MarkReader *r, CuArena *a, unsigned char opener, unsigned char closer,
                                         MarkClosureOptions opts, CuVec *out);

#endif
