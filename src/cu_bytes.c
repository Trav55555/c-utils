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

/* take() for a reader that stores an integer in *out. */
static const unsigned char *take_int(CuStr s, size_t *pos, size_t n, const unsigned long *out)
{
    if (out == NULL)
        abort();
    return take(s, pos, n);
}

/* Each reader assembles its width with one expression, which compilers turn
 * into a single load where the host allows. Each byte is widened to
 * unsigned long before shifting: as an unsigned char it would promote to
 * int, where 0xFF << 24 overflows. */
int cu_rd_u8(CuStr s, size_t *pos, unsigned long *out)
{
    const unsigned char *p;

    p = take_int(s, pos, 1, out);
    if (p == NULL)
        return 0;
    *out = (unsigned long)p[0];
    return 1;
}

int cu_rd_u16le(CuStr s, size_t *pos, unsigned long *out)
{
    const unsigned char *p;

    p = take_int(s, pos, 2, out);
    if (p == NULL)
        return 0;
    *out = (unsigned long)p[0] | (unsigned long)p[1] << 8;
    return 1;
}

int cu_rd_u16be(CuStr s, size_t *pos, unsigned long *out)
{
    const unsigned char *p;

    p = take_int(s, pos, 2, out);
    if (p == NULL)
        return 0;
    *out = (unsigned long)p[0] << 8 | (unsigned long)p[1];
    return 1;
}

int cu_rd_u32le(CuStr s, size_t *pos, unsigned long *out)
{
    const unsigned char *p;

    p = take_int(s, pos, 4, out);
    if (p == NULL)
        return 0;
    *out = (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 |
           (unsigned long)p[3] << 24;
    return 1;
}

int cu_rd_u32be(CuStr s, size_t *pos, unsigned long *out)
{
    const unsigned char *p;

    p = take_int(s, pos, 4, out);
    if (p == NULL)
        return 0;
    *out = (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 |
           (unsigned long)p[3];
    return 1;
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

/* The 32-bit value of four bytes at p, in either order. */
static unsigned long get32(const unsigned char *p, int big)
{
    if (big)
        return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 |
               (unsigned long)p[3];
    return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 |
           (unsigned long)p[3] << 24;
}

/* The high half comes first in big-endian order and last in little-endian. */
static int read_u64(CuStr s, size_t *pos, CuU64 *out, int big)
{
    const unsigned char *p;

    if (out == NULL)
        abort();
    p = take(s, pos, 8);
    if (p == NULL)
        return 0;
    if (big)
        *out = cu_u64(get32(p, 1), get32(p + 4, 1));
    else
        *out = cu_u64(get32(p + 4, 0), get32(p, 0));
    return 1;
}

int cu_rd_u64le(CuStr s, size_t *pos, CuU64 *out)
{
    return read_u64(s, pos, out, 0);
}

int cu_rd_u64be(CuStr s, size_t *pos, CuU64 *out)
{
    return read_u64(s, pos, out, 1);
}

void cu_sb_u64le(CuBuilder *b, CuU64 v)
{
    write_le(b, v.lo, 4);
    write_le(b, v.hi, 4);
}

void cu_sb_u64be(CuBuilder *b, CuU64 v)
{
    write_be(b, v.hi, 4);
    write_be(b, v.lo, 4);
}
