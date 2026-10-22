#include <stdlib.h>
#include "cu.h"

/* Words are held in unsigned long, which may be wider than 32 bits, so
 * every sum and left shift is masked back to 32 bits. */
#define M32 0xFFFFFFFFUL
#define ROTL(x, n) ((((x) << (n)) | ((x) >> (32 - (n)))) & M32)

/* Hash one 64-byte block into the state. */
static void compress(unsigned long state[5], const unsigned char *p)
{
    unsigned long w[80];
    unsigned long v[5];
    unsigned long f;
    unsigned long k;
    unsigned long t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (unsigned long)p[4 * i] << 24 | (unsigned long)p[4 * i + 1] << 16 |
               (unsigned long)p[4 * i + 2] << 8 | (unsigned long)p[4 * i + 3];
    for (i = 16; i < 80; i++)
        w[i] = ROTL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 5; i++)
        v[i] = state[i];
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (v[1] & v[2]) | (~v[1] & M32 & v[3]);
            k = 0x5A827999UL;
        } else if (i < 40) {
            f = v[1] ^ v[2] ^ v[3];
            k = 0x6ED9EBA1UL;
        } else if (i < 60) {
            f = (v[1] & v[2]) | (v[1] & v[3]) | (v[2] & v[3]);
            k = 0x8F1BBCDCUL;
        } else {
            f = v[1] ^ v[2] ^ v[3];
            k = 0xCA62C1D6UL;
        }
        t = (ROTL(v[0], 5) + f + v[4] + k + w[i]) & M32;
        v[4] = v[3];
        v[3] = v[2];
        v[2] = ROTL(v[1], 30);
        v[1] = v[0];
        v[0] = t;
    }
    for (i = 0; i < 5; i++)
        state[i] = (state[i] + v[i]) & M32;
}

static void check_open(const CuSha1 *h)
{
    if (h == NULL || h->done)
        abort();
}

void cu_sha1_init(CuSha1 *h)
{
    static const unsigned long iv[5] = {0x67452301UL, 0xEFCDAB89UL, 0x98BADCFEUL, 0x10325476UL,
                                        0xC3D2E1F0UL};
    int i;

    if (h == NULL)
        abort();
    for (i = 0; i < 5; i++)
        h->state[i] = iv[i];
    h->len = cu_u64(0, 0);
    h->used = 0;
    h->done = 0;
}

void cu_sha1_update(CuSha1 *h, CuStr data)
{
    const unsigned char *p;
    size_t n;
    size_t take;

    check_open(h);
    /* The bit length must fit 64 bits, so the byte count stays below 2^61. */
    if (!cu_u64_add(h->len, cu_u64_from_size(data.len), &h->len) || h->len.hi > 0x1FFFFFFFUL)
        abort();
    /* An empty view may have a NULL pointer, which memcpy must not see. */
    if (data.len == 0)
        return;
    p = (const unsigned char *)data.ptr;
    n = data.len;
    if (h->used > 0) {
        take = 64 - h->used < n ? 64 - h->used : n;
        memcpy(h->block + h->used, p, take);
        h->used += take;
        p += take;
        n -= take;
        if (h->used < 64)
            return;
        compress(h->state, h->block);
        h->used = 0;
    }
    for (; n >= 64; p += 64, n -= 64)
        compress(h->state, p);
    if (n > 0)
        memcpy(h->block, p, n);
    h->used = n;
}

void cu_sha1_final(CuSha1 *h, unsigned char digest[20])
{
    unsigned long hi;
    unsigned long lo;
    int i;

    check_open(h);
    if (digest == NULL)
        abort();
    /* The message length in bits, as two 32-bit halves of bytes * 8. */
    hi = (h->len.hi << 3 | h->len.lo >> 29) & M32;
    lo = (h->len.lo << 3) & M32;
    h->block[h->used++] = 0x80;
    /* The zero bytes are padding the standard specifies, not a reset. */
    if (h->used > 56) {
        while (h->used < 64)
            h->block[h->used++] = 0;
        compress(h->state, h->block);
        h->used = 0;
    }
    while (h->used < 56)
        h->block[h->used++] = 0;
    for (i = 0; i < 4; i++) {
        h->block[56 + i] = (unsigned char)(hi >> (24 - 8 * i) & 0xFFUL);
        h->block[60 + i] = (unsigned char)(lo >> (24 - 8 * i) & 0xFFUL);
    }
    compress(h->state, h->block);
    for (i = 0; i < 20; i++)
        digest[i] = (unsigned char)(h->state[i / 4] >> (24 - 8 * (i % 4)) & 0xFFUL);
    h->done = 1;
}

void cu_sha1(CuStr data, unsigned char digest[20])
{
    CuSha1 h;

    cu_sha1_init(&h);
    cu_sha1_update(&h, data);
    cu_sha1_final(&h, digest);
}
