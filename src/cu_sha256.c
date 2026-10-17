#include <stdlib.h>
#include "cu.h"

/* Words are held in unsigned long, which may be wider than 32 bits, so
 * every sum and left shift is masked back to 32 bits. */
#define M32 0xFFFFFFFFUL
#define ROTR(x, n) ((((x) >> (n)) | ((x) << (32 - (n)))) & M32)

static const unsigned long k[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL,
    0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL,
    0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
    0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL, 0xa2bfe8a1UL, 0xa81a664bUL,
    0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL,
    0x5b9cca4fUL, 0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL};

/* Hash one 64-byte block into the state. */
static void compress(unsigned long state[8], const unsigned char *p)
{
    unsigned long w[64];
    unsigned long v[8];
    unsigned long t1;
    unsigned long t2;
    unsigned long s0;
    unsigned long s1;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (unsigned long)p[4 * i] << 24 | (unsigned long)p[4 * i + 1] << 16 |
               (unsigned long)p[4 * i + 2] << 8 | (unsigned long)p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) & M32;
    }
    for (i = 0; i < 8; i++)
        v[i] = state[i];
    for (i = 0; i < 64; i++) {
        s1 = ROTR(v[4], 6) ^ ROTR(v[4], 11) ^ ROTR(v[4], 25);
        t1 = (v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & M32 & v[6])) + k[i] + w[i]) & M32;
        s0 = ROTR(v[0], 2) ^ ROTR(v[0], 13) ^ ROTR(v[0], 22);
        t2 = (s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]))) & M32;
        v[7] = v[6];
        v[6] = v[5];
        v[5] = v[4];
        v[4] = (v[3] + t1) & M32;
        v[3] = v[2];
        v[2] = v[1];
        v[1] = v[0];
        v[0] = (t1 + t2) & M32;
    }
    for (i = 0; i < 8; i++)
        state[i] = (state[i] + v[i]) & M32;
}

static void check_open(const CuSha256 *h)
{
    if (h == NULL || h->done)
        abort();
}

void cu_sha256_init(CuSha256 *h)
{
    static const unsigned long iv[8] = {0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL,
                                        0xa54ff53aUL, 0x510e527fUL, 0x9b05688cUL,
                                        0x1f83d9abUL, 0x5be0cd19UL};
    int i;

    if (h == NULL)
        abort();
    for (i = 0; i < 8; i++)
        h->state[i] = iv[i];
    h->len = cu_u64(0, 0);
    h->used = 0;
    h->done = 0;
}

void cu_sha256_update(CuSha256 *h, CuStr data)
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

void cu_sha256_final(CuSha256 *h, unsigned char digest[32])
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
    for (i = 0; i < 32; i++)
        digest[i] = (unsigned char)(h->state[i / 4] >> (24 - 8 * (i % 4)) & 0xFFUL);
    h->done = 1;
}

void cu_sha256(CuStr data, unsigned char digest[32])
{
    CuSha256 h;

    cu_sha256_init(&h);
    cu_sha256_update(&h, data);
    cu_sha256_final(&h, digest);
}
