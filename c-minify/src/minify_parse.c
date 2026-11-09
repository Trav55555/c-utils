/* tdewolff/parse v2.7.3 input.go, util.go, common.go and strconv/int.go,
 * minify v2.20.5 common.go, and Go's encoding/base64 as those use it. See
 * minify_parse.h. */
#include "minify_parse.h"
#include <limits.h>
#include <string.h>

MinBytes min_bytes(unsigned char *ptr, size_t len)
{
    MinBytes b;

    b.ptr = ptr;
    b.len = len;
    return b;
}

MinBytes min_bytes_nil(void)
{
    return min_bytes(NULL, 0);
}

MinBytes min_slice(MinBytes b, size_t i, size_t j)
{
    return min_bytes(b.ptr + i, j - i);
}

int min_bytes_eq(MinBytes a, const char *lit)
{
    size_t n = strlen(lit);

    return a.len == n && (n == 0 || memcmp(a.ptr, lit, n) == 0);
}

int min_has_prefix(MinBytes b, const char *lit)
{
    size_t n = strlen(lit);

    return b.len >= n && memcmp(b.ptr, lit, n) == 0;
}

int min_has_suffix(MinBytes b, const char *lit)
{
    size_t n = strlen(lit);

    return b.len >= n && memcmp(b.ptr + b.len - n, lit, n) == 0;
}

CuStr min_as_str(MinBytes b)
{
    return cu_str_n((const char *)b.ptr, b.len);
}

/* ---- parse.Input ------------------------------------------------------- */

void min_input_init(MinInput *z, unsigned char *buf, size_t len)
{
    z->buf = buf;
    z->len = len;
    z->pos = 0;
    z->start = 0;
}

/* Input.Peek */
unsigned char min_peek(const MinInput *z, size_t i)
{
    if (i > z->len - z->pos) return 0; /* guard: Go indexes past its buffer */
    return z->buf[z->pos + i];
}

/* Input.PeekErr(i) != nil */
int min_peek_eof(const MinInput *z, size_t i)
{
    return i >= z->len - z->pos;
}

/* Input.Err() != nil */
int min_eof(const MinInput *z)
{
    return z->pos >= z->len;
}

/* Input.Move */
void min_move(MinInput *z, size_t n)
{
    size_t room = z->len - z->pos;

    z->pos += n < room ? n : room; /* guard: never past the NUL */
}

/* Input.Pos */
size_t min_pos(const MinInput *z)
{
    return z->pos - z->start;
}

/* Input.Rewind */
void min_rewind(MinInput *z, size_t pos)
{
    z->pos = z->start + pos;
}

/* Input.Lexeme */
MinBytes min_lexeme(const MinInput *z)
{
    return min_bytes(z->buf + z->start, z->pos - z->start);
}

/* Input.Skip */
void min_skip(MinInput *z)
{
    z->start = z->pos;
}

/* Input.Shift */
MinBytes min_shift(MinInput *z)
{
    MinBytes b = min_lexeme(z);

    z->start = z->pos;
    return b;
}

/* ---- util.go ----------------------------------------------------------- */

/* parse.ToLower */
void min_to_lower(MinBytes b)
{
    size_t i;

    for (i = 0; i < b.len; i++) {
        if (b.ptr[i] >= 'A' && b.ptr[i] <= 'Z') b.ptr[i] = (unsigned char)(b.ptr[i] + ('a' - 'A'));
    }
}

/* parse.EqualFold */
int min_equal_fold(MinBytes s, const char *lower)
{
    size_t n = strlen(lower);
    size_t i;

    if (s.len != n) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)lower[i];
        unsigned char d = s.ptr[i];

        if (d != c && (d < 'A' || d > 'Z' || d + ('a' - 'A') != c)) return 0;
    }
    return 1;
}

/* parse.IsWhitespace: space, \n, \r, \t, \f. */
int min_is_whitespace(unsigned char c)
{
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f';
}

/* parse.IsNewline */
int min_is_newline(unsigned char c)
{
    return c == '\n' || c == '\r';
}

/* parse.IsAllWhitespace */
int min_is_all_whitespace(MinBytes b)
{
    size_t i;

    for (i = 0; i < b.len; i++) {
        if (!min_is_whitespace(b.ptr[i])) return 0;
    }
    return 1;
}

/* parse.TrimWhitespace */
MinBytes min_trim_whitespace(MinBytes b)
{
    size_t n = b.len;
    size_t start = n;
    size_t end = n;
    size_t i;

    for (i = 0; i < n; i++) {
        if (!min_is_whitespace(b.ptr[i])) {
            start = i;
            break;
        }
    }
    for (i = n; i > start; i--) {
        if (!min_is_whitespace(b.ptr[i - 1])) {
            end = i;
            break;
        }
    }
    return min_slice(b, start, end);
}

static int is_hex(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static unsigned hex_value(unsigned char c)
{
    if (c <= '9') return c - '0';
    if (c <= 'F') return c - 'A' + 10;
    return c - 'a' + 10;
}

static int is_alnum(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* util.go replaceEntities: b[i] is '&' and i+3 < len. Stores the index of
 * the entity's last byte in *last (the caller's loop then steps past it). */
static MinBytes replace_entity_at(MinBytes b, size_t i, const MinEntityMaps *maps, size_t *last)
{
    const unsigned char *r;
    size_t rlen;
    unsigned char rbuf[8];
    size_t j = i + 1;
    size_t n;

    if (b.ptr[j] == '#') {
        j++;
        if (b.ptr[j] == 'x') {
            /* Go accumulates c in a 64-bit int that wraps on overflow; the
             * value is kept as two 32-bit halves so every platform matches
             * the 64-bit oracle. */
            unsigned long hi = 0;
            unsigned long lo = 0;
            int negative;

            j++;
            for (; j < b.len && is_hex(b.ptr[j]); j++) {
                hi = ((hi << 4) | (lo >> 28)) & 0xffffffffUL;
                lo = ((lo << 4) | hex_value(b.ptr[j])) & 0xffffffffUL;
            }
            negative = (hi & 0x80000000UL) != 0;
            if (j <= i + 3 || (!negative && (hi != 0 || lo >= 10000))) {
                *last = j - 1;
                return b;
            }
            if (negative || lo < 128) {
                rbuf[0] = (unsigned char)(lo & 0xff);
                r = rbuf;
                rlen = 1;
            } else {
                unsigned long v = lo;
                size_t k = sizeof rbuf;

                rbuf[--k] = ';';
                do {
                    rbuf[--k] = (unsigned char)('0' + v % 10);
                    v /= 10;
                } while (v != 0);
                rbuf[--k] = '#';
                rbuf[--k] = '&';
                r = rbuf + k;
                rlen = sizeof rbuf - k;
            }
        } else {
            unsigned c = 0;

            for (; j < b.len && c < 128 && b.ptr[j] >= '0' && b.ptr[j] <= '9'; j++) c = c * 10 + (b.ptr[j] - '0');
            if (j <= i + 2 || c >= 128) {
                *last = j - 1;
                return b;
            }
            rbuf[0] = (unsigned char)c;
            r = rbuf;
            rlen = 1;
        }
    } else {
        for (; j < b.len && j - i - 1 <= 31 && b.ptr[j] != ';'; j++) {
        }
        if (j <= i + 1 || b.len <= j) {
            *last = j - 1;
            return b;
        }
        if (!maps->lookup(b.ptr + i + 1, j - i - 1, &r, &rlen)) {
            *last = j;
            return b;
        }
    }

    /* j is at the semicolon */
    n = j + 1 - i;
    if (j < b.len && b.ptr[j] == ';' && n > 2) {
        if (rlen == 1) {
            const char *q = maps->rev != NULL ? maps->rev[r[0]] : NULL;

            if (q != NULL) {
                size_t qlen = strlen(q);

                if (qlen == n && memcmp(q, b.ptr + i, n) == 0) {
                    *last = j;
                    return b;
                }
                r = (const unsigned char *)q;
                rlen = qlen;
            } else if (r[0] == '&') {
                /* &amp; followed by something that could continue an entity */
                size_t k = j + 1;

                if (k < b.len && (is_alnum(b.ptr[k]) || b.ptr[k] == '#')) {
                    *last = k;
                    return b;
                }
            }
        }
        if (rlen > n) { /* guard: no entity's replacement is longer than its text */
            *last = j;
            return b;
        }
        memmove(b.ptr + i, r, rlen);
        memmove(b.ptr + i + rlen, b.ptr + j + 1, b.len - (j + 1));
        b.len = b.len - n + rlen;
        *last = i + rlen - 1;
        return b;
    }
    *last = i;
    return b;
}

/* parse.ReplaceEntities */
MinBytes min_replace_entities(MinBytes b, const MinEntityMaps *maps)
{
    size_t i;

    for (i = 0; i < b.len; i++) {
        if (b.ptr[i] == '&' && i + 3 < b.len) b = replace_entity_at(b, i, maps, &i);
    }
    return b;
}

/* parse.ReplaceMultipleWhitespaceAndEntities. j is the write position, k
 * the start of the next text section, both as in Go, including the
 * special case for a run at the very start (j == 1). */
MinBytes min_replace_multiple_whitespace_and_entities(MinBytes b, const MinEntityMaps *maps)
{
    size_t i;
    size_t j = 0;
    size_t k = 0;

    for (i = 0; i < b.len; i++) {
        if (min_is_whitespace(b.ptr[i])) {
            size_t start = i;
            int newline = min_is_newline(b.ptr[i]);

            i++;
            for (; i < b.len && min_is_whitespace(b.ptr[i]); i++) {
                if (min_is_newline(b.ptr[i])) newline = 1;
            }
            b.ptr[start] = newline ? '\n' : ' ';
            if (i - start > 1) {
                if (j == 0) {
                    j = start + 1;
                } else {
                    memmove(b.ptr + j, b.ptr + k, start + 1 - k);
                    j += start + 1 - k;
                }
                k = i;
            }
        }
        if (i + 3 < b.len && b.ptr[i] == '&') b = replace_entity_at(b, i, maps, &i);
    }
    if (j == 0) return b;
    if (j == 1) {
        b.ptr[k - 1] = b.ptr[0];
        return min_slice(b, k - 1, b.len);
    }
    if (k < b.len) {
        memmove(b.ptr + j, b.ptr + k, b.len - k);
        j += b.len - k;
    }
    return min_slice(b, 0, j);
}

/* parse.DataURIEncodingTable: non-printable, space, ", #, %, &, <, >, [,
 * \, ], ^, `, {, |, }, DEL and non-ASCII. */
static int data_uri_escape(unsigned char c)
{
    if (c <= ' ' || c >= 0x7f) return 1;
    switch (c) {
    case '"':
    case '#':
    case '%':
    case '&':
    case '<':
    case '>':
    case '[':
    case '\\':
    case ']':
    case '^':
    case '`':
    case '{':
    case '|':
    case '}':
        return 1;
    default:
        return 0;
    }
}

/* parse.EncodeURL with DataURIEncodingTable */
MinBytes min_encode_data_uri(CuArena *a, MinBytes b, int *failed)
{
    size_t i;
    size_t n = b.len;
    unsigned char *out;
    size_t j = 0;

    for (i = 0; i < b.len; i++) {
        if (data_uri_escape(b.ptr[i])) n += 2;
    }
    out = (unsigned char *)cu_arena_push_bytes(a, n);
    if (out == NULL) {
        *failed = 1;
        return min_bytes_nil();
    }
    for (i = 0; i < b.len; i++) {
        unsigned char c = b.ptr[i];

        if (data_uri_escape(c)) {
            out[j++] = '%';
            out[j++] = (unsigned char)"0123456789ABCDEF"[c >> 4];
            out[j++] = (unsigned char)"0123456789ABCDEF"[c & 15];
        } else {
            out[j++] = c;
        }
    }
    return min_bytes(out, n);
}

/* parse.DecodeURL */
MinBytes min_decode_url(MinBytes b)
{
    size_t i;

    for (i = 0; i < b.len; i++) {
        if (b.ptr[i] == '%' && i + 2 < b.len) {
            size_t j = i + 1;
            unsigned c = 0;

            for (; j < i + 3 && is_hex(b.ptr[j]); j++) c = (c << 4) + hex_value(b.ptr[j]);
            if (j == i + 3 && c < 128) {
                b.ptr[i] = (unsigned char)c;
                memmove(b.ptr + i + 1, b.ptr + i + 3, b.len - (i + 3));
                b.len -= 2;
            }
        } else if (b.ptr[i] == '+') {
            b.ptr[i] = ' ';
        }
    }
    return b;
}

/* ---- common.go --------------------------------------------------------- */

static int is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

/* parse.Number */
size_t min_number_len(MinBytes b)
{
    size_t i = 0;
    size_t old;
    int first_digit;

    if (b.len == 0) return 0;
    if (b.ptr[i] == '+' || b.ptr[i] == '-') {
        i++;
        if (i >= b.len) return 0;
    }
    first_digit = is_digit(b.ptr[i]);
    if (first_digit) {
        i++;
        while (i < b.len && is_digit(b.ptr[i])) i++;
    }
    if (i < b.len && b.ptr[i] == '.') {
        i++;
        if (i < b.len && is_digit(b.ptr[i])) {
            i++;
            while (i < b.len && is_digit(b.ptr[i])) i++;
        } else if (first_digit) {
            /* . could belong to the next token */
            return i - 1;
        } else {
            return 0;
        }
    } else if (!first_digit) {
        return 0;
    }
    old = i;
    if (i < b.len && (b.ptr[i] == 'e' || b.ptr[i] == 'E')) {
        i++;
        if (i < b.len && (b.ptr[i] == '+' || b.ptr[i] == '-')) i++;
        if (i >= b.len || !is_digit(b.ptr[i])) return old; /* e could belong to the next token */
        while (i < b.len && is_digit(b.ptr[i])) i++;
    }
    return i;
}

/* parse.Mediatype, the mimetype: leading spaces dropped, then up to the
 * first ';' or ' ' at index 3 or later. */
MinBytes min_mediatype_mime(MinBytes b)
{
    size_t i = 0;

    while (i < b.len && b.ptr[i] == ' ') i++;
    b = min_slice(b, i, b.len);
    for (i = 3; i < b.len; i++) {
        if (b.ptr[i] == ';' || b.ptr[i] == ' ') return min_slice(b, 0, i);
    }
    return b;
}

/* parse.DataURI */
int min_data_uri(CuArena *a, MinBytes uri, MinBytes *mediatype, MinBytes *data, int *failed)
{
    unsigned char *mt;
    size_t mtlen = 0;
    int in_base64 = 0;
    size_t i;
    size_t j;

    if (uri.len <= 5 || memcmp(uri.ptr, "data:", 5) != 0) return 0;
    uri = min_slice(uri, 5, uri.len);
    /* The mediatype is built by appends; it never exceeds the URI, and
     * minify.DataURI later appends ";base64" or substitutes "text/plain". */
    mt = (unsigned char *)cu_arena_push_bytes(a, uri.len + 32);
    if (mt == NULL) {
        *failed = 1;
        return 0;
    }
    i = 0;
    for (j = 0; j < uri.len; j++) {
        unsigned char c = uri.ptr[j];

        if (c == '=' || c == ';' || c == ',') {
            MinBytes seg = min_trim_whitespace(min_slice(uri, i, j));

            if (c != '=' && min_bytes_eq(seg, "base64")) {
                if (mtlen > 0) mtlen--;
                in_base64 = 1;
                i = j;
            } else if (c != ',') {
                memcpy(mt + mtlen, seg.ptr, seg.len);
                mtlen += seg.len;
                mt[mtlen++] = c;
                i = j + 1;
            } else {
                memcpy(mt + mtlen, seg.ptr, seg.len);
                mtlen += seg.len;
            }
            if (c == ',') {
                MinBytes rest = min_slice(uri, j + 1, uri.len);

                if (mtlen == 0 || mt[0] == ';') {
                    memcpy(mt, "text/plain", 10);
                    mtlen = 10;
                }
                if (in_base64) {
                    if (!min_base64_decode(a, rest, data, failed)) return 0;
                } else {
                    *data = min_decode_url(rest);
                }
                *mediatype = min_bytes(mt, mtlen);
                return 1;
            }
        }
    }
    return 0;
}

/* ---- minify common.go -------------------------------------------------- */

/* minify.Mediatype. i walks the original bytes while j writes compacted
 * ones; Go mixes the two in lastString, and so does this. */
MinBytes min_minify_mediatype(MinBytes b)
{
    size_t j = 0;
    int in_string = 0;
    size_t start = 0;
    size_t last_string = 0;
    size_t i;

    for (i = 0; i < b.len; i++) {
        unsigned char c = b.ptr[i];

        if (!in_string && min_is_whitespace(c)) {
            if (start != 0) {
                memmove(b.ptr + j, b.ptr + start, i - start);
                j += i - start;
            } else {
                j += i;
            }
            start = i + 1;
        } else if (c == '"') {
            in_string = !in_string;
            if (in_string) {
                if (last_string <= i && i - last_string < 1024) min_to_lower(min_slice(b, last_string, i));
            } else {
                last_string = j + (i + 1 - start);
            }
        }
    }
    if (start != 0) {
        memmove(b.ptr + j, b.ptr + start, b.len - start);
        j += b.len - start;
        if (last_string <= j) min_to_lower(min_slice(b, last_string, j));
        return min_slice(b, 0, j);
    }
    if (last_string <= b.len) min_to_lower(min_slice(b, last_string, b.len));
    return b;
}

/* minify.Number with prec < 1 (the precision step belongs to the CSS and
 * SVG minifiers, which are not in this slice). Go rewrites num in place
 * and may write a few bytes past the digits it keeps; the work is done on
 * a copy with that slack and copied back, which gives the same bytes
 * since no result is longer than its input. int is long here: 64 bits on
 * the targets that matter, as in the oracle. */
MinBytes min_minify_number(CuArena *a, MinBytes orig, long prec, int *failed)
{
    unsigned char *num;
    size_t start = 0;
    size_t end = orig.len;
    size_t dot;
    long orig_exp = 0;
    int neg = 0;
    int has_dot = 0;
    size_t i;
    long n = 0;
    long norm_exp = 0;
    long int_exp;
    long len_int_exp;
    long len_norm_exp;
    long ti;
    size_t len;

    (void)prec; /* always -1 in this slice */
    if (orig.len <= 1) return orig;
    num = (unsigned char *)cu_arena_push_bytes(a, orig.len + 32);
    if (num == NULL) {
        *failed = 1;
        return orig;
    }
    memcpy(num, orig.ptr, orig.len);
    len = orig.len;

    /* omit first + and register mantissa start and end, whether it's negative and the exponent */
    if (num[0] == '+' || num[0] == '-') {
        if (num[0] == '-') neg = 1;
        start++;
    }
    for (i = start; i < len; i++) {
        if (num[i] == '.') {
            dot = i;
            has_dot = 1;
        } else if (num[i] == 'e' || num[i] == 'E') {
            size_t k = i + 1;
            size_t read;
            long e;

            end = i;
            if (k < len && num[k] == '+') k++;
            e = min_parse_int(min_bytes(num + k, len - k), &read);
            if (read == 0) return orig;
            orig_exp = e;
            break;
        }
    }
    if (!has_dot) dot = end;

    /* trim leading zeros but leave at least one digit */
    while (start + 1 < end && num[start] == '0') start++;
    /* trim trailing zeros (Go's index is an int: it may pass below zero) */
    ti = (long)end - 1;
    for (; (long)dot < ti; ti--) {
        if (num[ti] != '0') {
            end = (size_t)ti + 1;
            break;
        }
    }
    if (ti == (long)dot) {
        end = dot;
        if (start == end) {
            orig.ptr[0] = '0';
            return min_slice(orig, 0, 1);
        }
    } else if (end > 0 && start == end - 1 && num[start] == '0') {
        memcpy(orig.ptr, num + start, 1);
        return min_slice(orig, 0, 1);
    }

    /* n is the number of significant digits; normExp the exponent if it
     * were normalised (0.1 <= f < 1) */
    if (dot == start) {
        for (i = dot + 1; i < end; i++) {
            if (num[i] != '0') {
                n = (long)(end - i);
                norm_exp = (long)dot - (long)i + 1;
                break;
            }
        }
    } else if (dot == end) {
        norm_exp = (long)(end - start);
        for (i = end; i > start; i--) {
            if (num[i - 1] != '0') {
                n = (long)(i - start);
                end = i;
                break;
            }
        }
    } else {
        n = (long)(end - start) - 1;
        norm_exp = (long)(dot - start);
    }

    if ((orig_exp < 0 && (norm_exp < LONG_MIN - orig_exp || norm_exp - n < LONG_MIN - orig_exp)) ||
        (orig_exp > 0 && (LONG_MAX - orig_exp < norm_exp || LONG_MAX - orig_exp < norm_exp - n))) {
        return orig; /* exponent overflow */
    }
    norm_exp += orig_exp;

    /* intExp would be the exponent if it were an integer */
    int_exp = norm_exp - n;
    len_int_exp = (long)min_len_int(int_exp);
    len_norm_exp = (long)min_len_int(norm_exp);

    if (n <= norm_exp) {
        /* case 1: without decimals and with a positive exponent (large numbers: 5e4) */
        if (dot < end) {
            /* remove dot, either from the front or copy the smallest part */
            if (dot == start) {
                start = end - (size_t)n;
            } else if (dot - start < end - dot - 1) {
                memmove(num + start + 1, num + start, dot - start);
                start++;
            } else {
                memmove(num + dot, num + dot + 1, end - dot - 1);
                end--;
            }
        }
        if (n + 3 <= norm_exp) {
            long e = int_exp;
            long k;

            num[end] = 'e';
            end++;
            for (k = len_int_exp - 1; k >= 0; k--) {
                num[end + (size_t)k] = (unsigned char)('0' + e % 10);
                e /= 10;
            }
            end += (size_t)len_int_exp;
        } else if (n + 2 == norm_exp) {
            num[end] = '0';
            num[end + 1] = '0';
            end += 2;
        } else if (n + 1 == norm_exp) {
            num[end] = '0';
            end++;
        }
    } else if (norm_exp < -3 && len_norm_exp < len_int_exp && dot < end) {
        /* case 2: print normalized number (0.1 <= f < 1) */
        long zeroes = -norm_exp + orig_exp;
        long e = norm_exp;
        long k;

        if (zeroes > 0) {
            if (start + 1 + (size_t)zeroes > end) return orig; /* guard: Go's slice would panic */
            memmove(num + start + 1, num + start + 1 + (size_t)zeroes, end - (start + 1 + (size_t)zeroes));
            end -= (size_t)zeroes;
        } else if (zeroes < 0) {
            memmove(num + start + 1, num + start, dot - start);
            num[start] = '.';
        }
        num[end] = 'e';
        num[end + 1] = '-';
        end += 2;
        for (k = len_norm_exp - 1; k >= 0; k--) {
            num[end + (size_t)k] = (unsigned char)('0' - e % 10);
            e /= 10;
        }
        end += (size_t)len_norm_exp;
    } else if (-len_int_exp - 1 <= norm_exp) {
        /* case 3: print number without exponent */
        long zeroes = -norm_exp;

        if (zeroes > 0) {
            /* dot placed at the front and negative exponent, adding zeroes */
            long new_dot = (long)end - n - zeroes - 1;

            if (new_dot != (long)dot) {
                long d = (long)start - new_dot;
                long k;

                if (d > 0 && (size_t)d + end + 1 > len + 32) return orig; /* guard: past the slack */
                if (d > 0) {
                    if (dot < end) {
                        /* copy original digits after the dot towards the end */
                        memmove(num + dot + 1 + (size_t)d, num + dot + 1, end - (dot + 1));
                        if (start < dot) {
                            /* copy original digits before the dot towards the end */
                            memmove(num + start + (size_t)d + 1, num + start, dot - start);
                        }
                    } else if (start < dot) {
                        /* copy original digits before the dot towards the end */
                        memmove(num + start + (size_t)d, num + start, dot - start);
                    }
                    new_dot = (long)start;
                    end += (size_t)d;
                } else {
                    start += (size_t)(-d);
                }
                num[new_dot] = '.';
                for (k = 0; k < zeroes; k++) num[new_dot + 1 + k] = '0';
            }
        } else {
            /* dot placed in the middle of the number */
            size_t new_dot;

            if (dot == start) {
                /* when there are zeroes after the dot */
                dot = end - (size_t)n - 1;
                start = dot;
            } else if (end <= dot) {
                /* when input has no dot in it */
                dot = end;
                end++;
            }
            new_dot = start + (size_t)norm_exp;
            /* move digits between dot and newDot towards the end */
            if (dot < new_dot) {
                memmove(num + dot, num + dot + 1, new_dot - dot);
            } else if (new_dot < dot) {
                memmove(num + new_dot + 1, num + new_dot, dot - new_dot);
            }
            num[new_dot] = '.';
        }
    } else {
        /* case 4: print number with negative exponent */
        /* find new end, considering moving numbers to the front, removing the dot and increasing the length of the exponent */
        size_t new_end = end;
        long exp = int_exp;
        long len_exp = len_int_exp;
        long k;

        if (dot == start) {
            new_end = start + (size_t)n;
        } else {
            new_end--;
        }
        new_end += 2 + (size_t)len_int_exp;

        if (new_end < len) {
            /* it saves space to convert the decimal to an integer and decrease the exponent */
            if (dot < end) {
                if (dot == start) {
                    memmove(num + start, num + end - (size_t)n, (size_t)n);
                    end = start + (size_t)n;
                } else {
                    memmove(num + dot, num + dot + 1, end - (dot + 1));
                    end--;
                }
            }
        } else {
            /* it does not save space and will panic, so we revert to the original representation */
            exp = orig_exp;
            len_exp = 1;
            if (orig_exp <= -10 || orig_exp >= 10) len_exp = (long)min_len_int(orig_exp);
        }
        num[end] = 'e';
        num[end + 1] = '-';
        end += 2;
        for (k = len_exp - 1; k >= 0; k--) {
            num[end + (size_t)k] = (unsigned char)('0' - exp % 10);
            exp /= 10;
        }
        end += (size_t)len_exp;
    }

    if (neg) {
        start--;
        num[start] = '-';
    }
    if (end - start > orig.len) return orig; /* guard: Go's in-place write would panic here */
    memcpy(orig.ptr, num + start, end - start);
    return min_slice(orig, 0, end - start);
}

/* ---- strconv/int.go ---------------------------------------------------- */

/* strconv.ParseInt */
long min_parse_int(MinBytes b, size_t *n)
{
    size_t i = 0;
    size_t start;
    int neg = 0;
    unsigned long v = 0;
    const unsigned long max_mag = (unsigned long)LONG_MAX + 1UL; /* -MinInt */

    *n = 0;
    if (b.len > 0 && (b.ptr[0] == '+' || b.ptr[0] == '-')) {
        neg = b.ptr[0] == '-';
        i++;
    }
    start = i;
    while (i < b.len) {
        unsigned char c = b.ptr[i];

        if (!is_digit(c)) break;
        if (max_mag / 10 < v || max_mag - (c - '0') < v * 10) return 0;
        v = v * 10 + (c - '0');
        i++;
    }
    if (i == start) return 0;
    if (!neg && v > (unsigned long)LONG_MAX) return 0;
    *n = i;
    if (neg) {
        if (v == max_mag) return LONG_MIN;
        return -(long)v;
    }
    return (long)v;
}

/* strconv.LenInt */
size_t min_len_int(long i)
{
    unsigned long v;
    size_t n = 1;

    if (i < 0) {
        if (i == LONG_MIN) return sizeof(long) >= 8 ? 19 : 10;
        i = -i;
    }
    v = (unsigned long)i;
    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

/* ---- encoding/base64 --------------------------------------------------- */

static const char b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Encoding.Decode: quanta of four symbols, newlines skipped, '=' padding
 * only in the third and fourth places and then nothing but newlines. */
int min_base64_decode(CuArena *a, MinBytes src, MinBytes *out, int *failed)
{
    unsigned char *dst = (unsigned char *)cu_arena_push_bytes(a, src.len / 4 * 3 + 3);
    size_t n = 0;
    size_t si = 0;

    if (dst == NULL) {
        *failed = 1;
        return 0;
    }
    while (si < src.len) {
        unsigned long dbuf = 0;
        size_t j;
        size_t dlen = 4;

        for (j = 0; j < 4; j++) {
            unsigned char c;
            int v;

            while (si < src.len && (src.ptr[si] == '\n' || src.ptr[si] == '\r')) si++;
            if (si == src.len) {
                if (j == 0) goto done;
                return 0; /* incomplete quantum: CorruptInputError */
            }
            c = src.ptr[si++];
            if (c == '=') {
                if (j < 2) return 0;
                if (j == 2) {
                    while (si < src.len && (src.ptr[si] == '\n' || src.ptr[si] == '\r')) si++;
                    if (si == src.len || src.ptr[si] != '=') return 0;
                    si++;
                }
                while (si < src.len && (src.ptr[si] == '\n' || src.ptr[si] == '\r')) si++;
                if (si < src.len) return 0; /* trailing garbage */
                dlen = j;
                break;
            }
            v = b64_value(c);
            if (v < 0) return 0;
            dbuf |= (unsigned long)v << (6 * (3 - j));
        }
        if (dlen >= 2) dst[n++] = (unsigned char)(dbuf >> 16);
        if (dlen >= 3) dst[n++] = (unsigned char)((dbuf >> 8) & 0xff);
        if (dlen >= 4) dst[n++] = (unsigned char)(dbuf & 0xff);
    }
done:
    *out = min_bytes(dst, n);
    return 1;
}

/* Encoding.Encode with padding */
MinBytes min_base64_encode(CuArena *a, MinBytes src, int *failed)
{
    size_t n = (src.len + 2) / 3 * 4;
    unsigned char *dst = (unsigned char *)cu_arena_push_bytes(a, n);
    size_t i;
    size_t j = 0;

    if (dst == NULL) {
        *failed = 1;
        return min_bytes_nil();
    }
    for (i = 0; i + 3 <= src.len; i += 3) {
        unsigned long v = ((unsigned long)src.ptr[i] << 16) | ((unsigned long)src.ptr[i + 1] << 8) | src.ptr[i + 2];

        dst[j++] = (unsigned char)b64_alphabet[(v >> 18) & 63];
        dst[j++] = (unsigned char)b64_alphabet[(v >> 12) & 63];
        dst[j++] = (unsigned char)b64_alphabet[(v >> 6) & 63];
        dst[j++] = (unsigned char)b64_alphabet[v & 63];
    }
    if (i < src.len) {
        unsigned long v = (unsigned long)src.ptr[i] << 16;

        if (i + 1 < src.len) v |= (unsigned long)src.ptr[i + 1] << 8;
        dst[j++] = (unsigned char)b64_alphabet[(v >> 18) & 63];
        dst[j++] = (unsigned char)b64_alphabet[(v >> 12) & 63];
        dst[j++] = i + 1 < src.len ? (unsigned char)b64_alphabet[(v >> 6) & 63] : '=';
        dst[j++] = '=';
    }
    return min_bytes(dst, n);
}

/* ---- buffers ----------------------------------------------------------- */

unsigned char *min_buf_reserve(MinBuf *buf, CuArena *a, size_t n, int *failed)
{
    size_t cap;
    unsigned char *p;

    if (n <= buf->cap) return buf->ptr;
    cap = buf->cap < 64 ? 64 : buf->cap;
    while (cap < n) {
        if (cap > ((size_t)-1) / 2) {
            *failed = 1;
            return NULL;
        }
        cap *= 2;
    }
    /* A fresh push rather than cu_arena_grow: the doubling bounds what the
     * abandoned buffers add up to, and the arena's scope frees them all. */
    p = (unsigned char *)cu_arena_push_bytes(a, cap);
    if (p == NULL) {
        *failed = 1;
        return NULL;
    }
    buf->ptr = p;
    buf->cap = cap;
    return p;
}

int min_copy_input(CuArena *a, CuStr src, MinBytes *out, int *failed)
{
    unsigned char *p = (unsigned char *)cu_arena_push_bytes(a, src.len + 1);

    if (p == NULL) {
        *failed = 1;
        return 0;
    }
    if (src.len > 0) memcpy(p, src.ptr, src.len);
    p[src.len] = 0;
    *out = min_bytes(p, src.len);
    return 1;
}
