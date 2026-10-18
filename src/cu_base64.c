#include <stdlib.h>
#include "cu.h"

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void cu_sb_base64(CuBuilder *b, CuStr bytes)
{
    const unsigned char *p;
    unsigned long group;
    size_t i;
    size_t left;

    if (b == NULL || (bytes.ptr == NULL && bytes.len != 0))
        abort();
    p = (const unsigned char *)bytes.ptr;
    for (i = 0; i < bytes.len; i += 3) {
        left = bytes.len - i;
        group = (unsigned long)p[i] << 16;
        if (left > 1)
            group |= (unsigned long)p[i + 1] << 8;
        if (left > 2)
            group |= p[i + 2];
        cu_sb_char(b, alphabet[group >> 18 & 0x3F]);
        cu_sb_char(b, alphabet[group >> 12 & 0x3F]);
        cu_sb_char(b, left > 1 ? alphabet[group >> 6 & 0x3F] : '=');
        cu_sb_char(b, left > 2 ? alphabet[group & 0x3F] : '=');
    }
}

/* The 6-bit value of c, or -1 if c is not in the alphabet. */
static int sextet(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

int cu_base64_decode(CuArena *a, CuStr text, CuStr *out)
{
    unsigned char *dst;
    unsigned long group;
    size_t pad;
    size_t n;
    size_t i;
    size_t k;
    int v;

    if (a == NULL || out == NULL || (text.ptr == NULL && text.len != 0))
        abort();
    *out = cu_str_empty();
    if (text.len % 4 != 0)
        return 0;
    if (text.len == 0)
        return 1;
    pad = 0;
    if (text.ptr[text.len - 1] == '=')
        pad = text.ptr[text.len - 2] == '=' ? 2 : 1;
    n = text.len / 4 * 3 - pad;
    dst = (unsigned char *)cu_arena_push_bytes(a, n);
    if (dst == NULL)
        return 0;
    k = 0;
    for (i = 0; i < text.len; i += 4) {
        group = 0;
        for (v = 0; v < 4; v++) {
            /* '=' stands only in the last pad places, and counts as zero. */
            if (i + (size_t)v >= text.len - pad) {
                group <<= 6;
                continue;
            }
            if (sextet(text.ptr[i + (size_t)v]) < 0)
                return 0;
            group = group << 6 | (unsigned long)sextet(text.ptr[i + (size_t)v]);
        }
        /* Bits that padding leaves over must be zero, so each byte string
         * has exactly one text. */
        if ((pad == 1 && i + 4 == text.len && (group & 0xFF) != 0) ||
            (pad == 2 && i + 4 == text.len && (group & 0xFFFF) != 0))
            return 0;
        dst[k++] = (unsigned char)(group >> 16 & 0xFF);
        if (k < n)
            dst[k++] = (unsigned char)(group >> 8 & 0xFF);
        if (k < n)
            dst[k++] = (unsigned char)(group & 0xFF);
    }
    *out = cu_str_n((const char *)dst, n);
    return 1;
}
