#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

/* An unsigned long's bits: room for its hex digits, or for its decimal
 * digits and a sign. */
#define CU_ULONG_DIGITS (sizeof(unsigned long) * CHAR_BIT)

/* 0..35 for a digit or letter, 36 for anything else. Ranges are compared
 * here rather than through cu_ascii_*, which are calls into another unit. */
static unsigned digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'z')
        return (unsigned)(c - 'a') + 10;
    if (c >= 'A' && c <= 'Z')
        return (unsigned)(c - 'A') + 10;
    return 36;
}

int cu_parse_ulong(CuStr s, unsigned base, unsigned long *out)
{
    unsigned long v;
    unsigned long lim;
    unsigned long rem;
    unsigned d;
    size_t i;

    if (base < 2 || base > 36 || out == NULL)
        abort();
    if (s.len == 0)
        return 0;
    /* v * base + d overflows exactly when v > lim, or v == lim and
     * d > rem: the divisions happen once, not per digit. */
    lim = ULONG_MAX / base;
    rem = ULONG_MAX % base;
    v = 0;
    for (i = 0; i < s.len; i++) {
        d = digit_value(s.ptr[i]);
        if (d >= base)
            return 0;
        if (v > lim || (v == lim && d > rem))
            return 0;
        v = v * base + d;
    }
    *out = v;
    return 1;
}

int cu_parse_long(CuStr s, unsigned base, long *out)
{
    unsigned long mag;
    int neg;

    if (out == NULL)
        abort();
    neg = 0;
    if (s.len > 0 && (s.ptr[0] == '-' || s.ptr[0] == '+')) {
        neg = s.ptr[0] == '-';
        s = cu_str_skip(s, 1);
    }
    if (!cu_parse_ulong(s, base, &mag))
        return 0;
    if (neg) {
        if (mag > (unsigned long)LONG_MAX + 1)
            return 0;
        /* -(mag - 1) - 1 reaches LONG_MIN without overflowing. */
        *out = mag == 0 ? 0 : -(long)(mag - 1) - 1;
    } else {
        if (mag > (unsigned long)LONG_MAX)
            return 0;
        *out = (long)mag;
    }
    return 1;
}

void cu_sb_init(CuBuilder *b, CuArena *a)
{
    if (b == NULL || a == NULL)
        abort();
    b->arena = a;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->scope = cu_arena_scope(a);
    b->failed = 0;
}

/* Every append starts here. Growing under any scope but the builder's own
 * could leave it pointing at memory that scope's end frees. */
static void check_builder(const CuBuilder *b)
{
    if (b == NULL || b->arena->scope != b->scope)
        abort();
}

/* Room for n > 0 more bytes, or NULL after marking the builder failed. */
static char *reserve(CuBuilder *b, size_t n)
{
    size_t need;
    size_t cap;
    char *data;

    if (b->failed)
        return NULL;
    if (!cu_size_add(b->len, n, &need)) {
        b->failed = 1;
        return NULL;
    }
    if (need > b->cap) {
        cap = b->cap != 0 ? b->cap : 64;
        while (cap < need) {
            if (!cu_size_mul(cap, 2, &cap)) {
                cap = need;
                break;
            }
        }
        /* In place, or a new buffer and a copy; never a realloc, so views
         * from cu_sb_finish and appends of the builder's own bytes stay
         * valid. */
        if (b->data == NULL || !cu_arena_extend(b->arena, b->data, b->cap, cap)) {
            data = cu_arena_push_bytes(b->arena, cap);
            if (data == NULL) {
                b->failed = 1;
                return NULL;
            }
            if (b->len != 0)
                memcpy(data, b->data, b->len);
            b->data = data;
        }
        b->cap = cap;
    }
    return b->data + b->len;
}

static void append(CuBuilder *b, const char *src, size_t n)
{
    char *dst;

    check_builder(b);
    if (n == 0)
        return;
    dst = reserve(b, n);
    if (dst == NULL)
        return;
    memcpy(dst, src, n);
    b->len += n;
}

void cu_sb_str(CuBuilder *b, CuStr s)
{
    append(b, s.ptr, s.len);
}

void cu_sb_char(CuBuilder *b, char c)
{
    check_builder(b);
    if (!b->failed && b->len < b->cap) {
        b->data[b->len++] = c;
        return;
    }
    append(b, &c, 1);
}

/* Writes v's decimal digits, after a '-' if negative is set, so that they
 * end at buf + CU_ULONG_DIGITS, and returns how many bytes that is. Each
 * division by the constant 100 yields two digits from a table, halving the
 * chain of dependent divisions. An unsigned long has fewer decimal digits
 * than bits, so the sign always fits. */
static size_t to_decimal(unsigned long v, int negative, char *buf)
{
    static const char pairs[] =
        "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
        "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";
    char *p;
    size_t r;

    p = buf + CU_ULONG_DIGITS;
    while (v >= 100) {
        r = (size_t)(v % 100) * 2;
        v /= 100;
        *--p = pairs[r + 1];
        *--p = pairs[r];
    }
    if (v >= 10) {
        r = (size_t)v * 2;
        *--p = pairs[r + 1];
        *--p = pairs[r];
    } else {
        *--p = (char)('0' + v);
    }
    if (negative)
        *--p = '-';
    return (size_t)(buf + CU_ULONG_DIGITS - p);
}

/* Writes v's lowercase hex digits so that they end at buf +
 * CU_ULONG_DIGITS; returns how many. */
static size_t to_hex(unsigned long v, char *buf)
{
    static const char digits[] = "0123456789abcdef";
    char *p;

    p = buf + CU_ULONG_DIGITS;
    do {
        *--p = digits[v & 15];
        v >>= 4;
    } while (v != 0);
    return (size_t)(buf + CU_ULONG_DIGITS - p);
}

void cu_sb_ulong(CuBuilder *b, unsigned long v)
{
    char buf[CU_ULONG_DIGITS];
    size_t n;

    n = to_decimal(v, 0, buf);
    append(b, buf + (CU_ULONG_DIGITS - n), n);
}

/* The sign and the digits go in one append. */
void cu_sb_long(CuBuilder *b, long v)
{
    char buf[CU_ULONG_DIGITS];
    size_t n;

    /* Unsigned negation is defined for LONG_MIN. */
    if (v < 0)
        n = to_decimal(0UL - (unsigned long)v, 1, buf);
    else
        n = to_decimal((unsigned long)v, 0, buf);
    append(b, buf + (CU_ULONG_DIGITS - n), n);
}

/* Padding and digits are reserved together, so a width too large to fit
 * fails once instead of appending zeros until memory runs out. */
void cu_sb_hex(CuBuilder *b, unsigned long v, size_t min_digits)
{
    char buf[CU_ULONG_DIGITS];
    size_t n;
    size_t pad;
    size_t total;
    char *dst;

    check_builder(b);
    n = to_hex(v, buf);
    pad = min_digits > n ? min_digits - n : 0;
    /* pad + n is at most max(min_digits, n), so it cannot overflow. */
    total = pad + n;
    dst = reserve(b, total);
    if (dst == NULL)
        return;
    memset(dst, '0', pad);
    memcpy(dst + pad, buf + (CU_ULONG_DIGITS - n), n);
    b->len += total;
}

int cu_sb_finish(CuBuilder *b, CuStr *out)
{
    if (b == NULL || out == NULL)
        abort();
    if (b->failed) {
        *out = cu_str_empty();
        return 0;
    }
    *out = cu_str_n(b->data, b->len);
    return 1;
}
