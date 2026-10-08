#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

/* Enough for an unsigned long in base 2. */
#define CU_ULONG_DIGITS (sizeof(unsigned long) * CHAR_BIT)

/* 0..35 for a digit or letter, 36 for anything else. */
static unsigned digit_value(char c)
{
    if (cu_ascii_is_digit(c))
        return (unsigned)(c - '0');
    if (cu_ascii_is_alpha(c))
        return (unsigned)(cu_ascii_to_lower(c) - 'a') + 10;
    return 36;
}

int cu_parse_ulong(CuStr s, unsigned base, unsigned long *out)
{
    unsigned long v;
    unsigned d;
    size_t i;

    if (base < 2 || base > 36)
        abort();
    if (s.len == 0)
        return 0;
    v = 0;
    for (i = 0; i < s.len; i++) {
        d = digit_value(s.ptr[i]);
        if (d >= base)
            return 0;
        if (v > (ULONG_MAX - d) / base)
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
    b->failed = 0;
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
        data = cu_arena_push_bytes(b->arena, cap);
        if (data == NULL) {
            b->failed = 1;
            return NULL;
        }
        if (b->len != 0)
            memcpy(data, b->data, b->len);
        b->data = data;
        b->cap = cap;
    }
    return b->data + b->len;
}

static void append(CuBuilder *b, const char *src, size_t n)
{
    char *dst;

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
    append(b, &c, 1);
}

/* Writes v in base 2..16 into the end of buf; returns the number of digits. */
static size_t to_digits(unsigned long v, unsigned base, char *buf)
{
    static const char digits[] = "0123456789abcdef";
    size_t n;

    n = 0;
    do {
        buf[CU_ULONG_DIGITS - 1 - n] = digits[v % base];
        v /= base;
        n++;
    } while (v != 0);
    return n;
}

static void append_number(CuBuilder *b, unsigned long v, unsigned base, size_t min_digits)
{
    char buf[CU_ULONG_DIGITS];
    size_t n;

    n = to_digits(v, base, buf);
    while (min_digits > n) {
        cu_sb_char(b, '0');
        min_digits--;
    }
    append(b, buf + (CU_ULONG_DIGITS - n), n);
}

void cu_sb_ulong(CuBuilder *b, unsigned long v)
{
    append_number(b, v, 10, 0);
}

void cu_sb_long(CuBuilder *b, long v)
{
    if (v < 0) {
        cu_sb_char(b, '-');
        /* Unsigned negation is defined for LONG_MIN. */
        append_number(b, 0UL - (unsigned long)v, 10, 0);
    } else {
        append_number(b, (unsigned long)v, 10, 0);
    }
}

void cu_sb_hex(CuBuilder *b, unsigned long v, size_t min_digits)
{
    append_number(b, v, 16, min_digits);
}

int cu_sb_finish(CuBuilder *b, CuStr *out)
{
    if (b->failed) {
        *out = cu_str_empty();
        return 0;
    }
    *out = cu_str_n(b->data, b->len);
    return 1;
}
