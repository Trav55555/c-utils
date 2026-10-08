#include <stdlib.h>
#include "cu.h"

/* Points at the n bytes at *pos, or returns NULL if fewer remain. */
static const unsigned char *take(CuStr s, size_t *pos, size_t n)
{
    const unsigned char *p;

    if (pos == NULL)
        abort();
    if (*pos > s.len || n > s.len - *pos)
        return NULL;
    p = (const unsigned char *)s.ptr + *pos;
    *pos += n;
    return p;
}

/* Each byte is widened to unsigned long before shifting: as an unsigned
 * char it would promote to int, where 0xFF << 24 overflows. */
static unsigned long le(const unsigned char *p, size_t n)
{
    unsigned long v;

    v = 0;
    while (n > 0) {
        n--;
        v = (v << 8) | (unsigned long)p[n];
    }
    return v;
}

static unsigned long be(const unsigned char *p, size_t n)
{
    unsigned long v;
    size_t i;

    v = 0;
    for (i = 0; i < n; i++)
        v = (v << 8) | (unsigned long)p[i];
    return v;
}

static int read_le(CuStr s, size_t *pos, size_t n, unsigned long *out)
{
    const unsigned char *p;

    if (out == NULL)
        abort();
    p = take(s, pos, n);
    if (p == NULL)
        return 0;
    *out = le(p, n);
    return 1;
}

static int read_be(CuStr s, size_t *pos, size_t n, unsigned long *out)
{
    const unsigned char *p;

    if (out == NULL)
        abort();
    p = take(s, pos, n);
    if (p == NULL)
        return 0;
    *out = be(p, n);
    return 1;
}

int cu_rd_u8(CuStr s, size_t *pos, unsigned long *out)
{
    return read_le(s, pos, 1, out);
}

int cu_rd_u16le(CuStr s, size_t *pos, unsigned long *out)
{
    return read_le(s, pos, 2, out);
}

int cu_rd_u16be(CuStr s, size_t *pos, unsigned long *out)
{
    return read_be(s, pos, 2, out);
}

int cu_rd_u32le(CuStr s, size_t *pos, unsigned long *out)
{
    return read_le(s, pos, 4, out);
}

int cu_rd_u32be(CuStr s, size_t *pos, unsigned long *out)
{
    return read_be(s, pos, 4, out);
}

int cu_rd_bytes(CuStr s, size_t *pos, size_t n, CuStr *out)
{
    const unsigned char *p;

    if (out == NULL)
        abort();
    p = take(s, pos, n);
    if (p == NULL)
        return 0;
    *out = cu_str_n((const char *)p, n);
    return 1;
}

/* Whether v fits in n <= 4 bytes. A table, not v >> (8 * n): that shift is
 * undefined at n == 4 where long is 32 bits. */
static int fits(unsigned long v, size_t n)
{
    static const unsigned long max[5] = {0UL, 0xFFUL, 0xFFFFUL, 0xFFFFFFUL, 0xFFFFFFFFUL};

    return v <= max[n];
}

/* Bytes go through unsigned char, so no value is converted to a possibly
 * signed char. */
static void write_le(CuBuilder *b, unsigned long v, size_t n)
{
    unsigned char buf[4];
    size_t i;

    if (!fits(v, n))
        abort();
    for (i = 0; i < n; i++)
        buf[i] = (unsigned char)(v >> (8 * i) & 0xFFUL);
    cu_sb_str(b, cu_str_n((const char *)buf, n));
}

static void write_be(CuBuilder *b, unsigned long v, size_t n)
{
    unsigned char buf[4];
    size_t i;

    if (!fits(v, n))
        abort();
    for (i = 0; i < n; i++)
        buf[n - 1 - i] = (unsigned char)(v >> (8 * i) & 0xFFUL);
    cu_sb_str(b, cu_str_n((const char *)buf, n));
}

void cu_sb_u8(CuBuilder *b, unsigned long v)
{
    write_le(b, v, 1);
}

void cu_sb_u16le(CuBuilder *b, unsigned long v)
{
    write_le(b, v, 2);
}

void cu_sb_u16be(CuBuilder *b, unsigned long v)
{
    write_be(b, v, 2);
}

void cu_sb_u32le(CuBuilder *b, unsigned long v)
{
    write_le(b, v, 4);
}

void cu_sb_u32be(CuBuilder *b, unsigned long v)
{
    write_be(b, v, 4);
}
