#ifndef MINIFY_PARSE_H
#define MINIFY_PARSE_H
#include "cu_minify.h"

/* tdewolff/parse v2.7.3's input buffer and byte helpers (input.go, util.go,
 * common.go, strconv/int.go) and tdewolff/minify v2.20.5's common.go, as
 * the HTML and XML minifiers use them. Go's []byte is a MinBytes: a
 * mutable view, since minify lowercases, collapses and rewrites in place
 * and this port keeps every such write so that later reads see the same
 * bytes. */

typedef struct MinBytes {
    unsigned char *ptr; /* NULL only for Go's nil */
    size_t len;
} MinBytes;

MinBytes min_bytes(unsigned char *ptr, size_t len);
MinBytes min_bytes_nil(void);
/* b[i:j] */
MinBytes min_slice(MinBytes b, size_t i, size_t j);
int min_bytes_eq(MinBytes a, const char *lit);
int min_has_prefix(MinBytes b, const char *lit);
int min_has_suffix(MinBytes b, const char *lit);
CuStr min_as_str(MinBytes b);

/* parse.Input: the input bytes followed by one NUL, with a selection
 * [start, pos). Peek past the NUL returns 0 (Go would panic; the lexers
 * never do it, and this guard keeps that a certainty). */
typedef struct MinInput {
    unsigned char *buf;
    size_t len; /* bytes before the NUL */
    size_t pos;
    size_t start;
} MinInput;

void min_input_init(MinInput *z, unsigned char *buf, size_t len);
unsigned char min_peek(const MinInput *z, size_t i);
/* Input.PeekErr(i) != nil: the byte at pos+i is the NUL at the end. */
int min_peek_eof(const MinInput *z, size_t i);
/* Input.Err() != nil */
int min_eof(const MinInput *z);
/* Input.Move, never past the NUL. */
void min_move(MinInput *z, size_t n);
/* Input.Pos: offset within the selection. */
size_t min_pos(const MinInput *z);
/* Input.Rewind(pos) */
void min_rewind(MinInput *z, size_t pos);
/* Input.Lexeme: the selection. */
MinBytes min_lexeme(const MinInput *z);
/* Input.Skip */
void min_skip(MinInput *z);
/* Input.Shift: the selection, then collapse it to its end. */
MinBytes min_shift(MinInput *z);

/* util.go */
void min_to_lower(MinBytes b);
/* parse.EqualFold(s, lower): lower is ASCII lowercase. */
int min_equal_fold(MinBytes s, const char *lower);
int min_is_whitespace(unsigned char c);
int min_is_newline(unsigned char c);
int min_is_all_whitespace(MinBytes b);
MinBytes min_trim_whitespace(MinBytes b);

/* An entity map: EntitiesMap[name] and the revEntitiesMap of one byte. */
typedef struct MinEntityMaps {
    /* The replacement for name, or NULL. */
    int (*lookup)(const unsigned char *name, size_t n, const unsigned char **value, size_t *value_len);
    const char *const *rev; /* 256 entries, NULL where the map has none; or NULL */
} MinEntityMaps;

MinBytes min_replace_entities(MinBytes b, const MinEntityMaps *maps);
MinBytes min_replace_multiple_whitespace_and_entities(MinBytes b, const MinEntityMaps *maps);
/* parse.EncodeURL(b, DataURIEncodingTable) into a copy in a. */
MinBytes min_encode_data_uri(CuArena *a, MinBytes b, int *failed);
/* parse.DecodeURL, in place. */
MinBytes min_decode_url(MinBytes b);

/* common.go parse.Number: the length of the leading number, or 0. */
size_t min_number_len(MinBytes b);
/* common.go parse.Mediatype: the mimetype part of b. The parameters are
 * not needed by any ported path. */
MinBytes min_mediatype_mime(MinBytes b);
/* common.go parse.DataURI: stores the mediatype (a copy in a) and the
 * decoded data (base64 into a copy in a, URL-encoding decoded in place)
 * and returns 1; returns 0 for ErrBadDataURI or a base64 error. *failed
 * is set when a ran out of memory. */
int min_data_uri(CuArena *a, MinBytes uri, MinBytes *mediatype, MinBytes *data, int *failed);

/* minify common.go Mediatype: whitespace removed, lowercased outside
 * strings, in place. */
MinBytes min_minify_mediatype(MinBytes b);
/* minify common.go Number(num, prec) for prec < 1: the result written over
 * the start of num (as Go rewrites in place), via a copy with slack in a.
 * A result Go's in-place write would overrun (a panic there) is num
 * unchanged. *failed is set when a ran out of memory. */
MinBytes min_minify_number(CuArena *a, MinBytes num, long prec, int *failed);

/* strconv/int.go ParseInt: the value and the bytes read (0 when none or
 * on overflow). */
long min_parse_int(MinBytes b, size_t *n);
/* strconv/int.go LenInt */
size_t min_len_int(long i);

/* encoding/base64 StdEncoding as Go decodes it: '\r' and '\n' skipped,
 * padding required, other bytes outside the alphabet an error, leftover
 * bits not checked. Returns 1 with the bytes in a, 0 on an error; *failed
 * is set when a ran out of memory. */
int min_base64_decode(CuArena *a, MinBytes src, MinBytes *out, int *failed);
/* StdEncoding.EncodeToString's bytes, in a. */
MinBytes min_base64_encode(CuArena *a, MinBytes src, int *failed);

/* A growable byte buffer in an arena, Go's reusable attrByteBuffer. */
typedef struct MinBuf {
    unsigned char *ptr;
    size_t cap;
} MinBuf;

/* Room for n bytes in buf, growing it in a; NULL and *failed set on failure. */
unsigned char *min_buf_reserve(MinBuf *buf, CuArena *a, size_t n, int *failed);

/* The state one top-level cu_minify call shares with the nested calls it
 * makes: the arena, the options, the nesting depth, and the sticky
 * failure the status is made from. */
typedef struct MinCtx {
    CuArena *a;
    unsigned long flags;
    int depth;
    int failed;      /* an allocation failed */
    int limit;       /* the depth bound was passed */
    int unsupported; /* an unported minifier was reached */
    int invalid;     /* a lexer error */
    MinBuf attr_buf; /* html.go attrByteBuffer / xml.go attrByteBuffer */
} MinCtx;

/* The ported minifiers over a copy of src made in ctx->a, writing to out;
 * each returns 0 and marks ctx when it cannot finish. */
int minify_html(MinCtx *ctx, MinBytes src, CuBuilder *out);
int minify_xml(MinCtx *ctx, MinBytes src, CuBuilder *out);

/* minify.M.MinifyMimetype under Hugo's minifier set: minifies data as
 * mimetype into out and returns 1, or returns 0 for ErrNotExist (the
 * caller then writes data unchanged). An unported minifier marks ctx
 * unsupported and returns 1. */
int minify_mimetype(MinCtx *ctx, MinBytes mimetype, MinBytes data, CuBuilder *out);
/* minify.M.Bytes(mediatype, data): the mimetype is taken from mediatype
 * first. Stores the minified bytes (in ctx->a) and returns 1; returns 0 on
 * an error, when Go returns the data unchanged. */
int minify_bytes(MinCtx *ctx, MinBytes mediatype, MinBytes data, MinBytes *out);

#endif
