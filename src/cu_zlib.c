#include <stdlib.h>
#include "cu.h"

/* Adler-32 sums are kept modulo the largest prime below 2^16. RUN is the
 * most bytes the sums can take before b would pass 32 bits (zlib's NMAX),
 * so they are reduced once per run, not once per byte. */
#define ADLER_MOD 65521UL
#define ADLER_RUN 5552
/* A stored block holds at most this many bytes. */
#define STORED_MAX 65535UL
/* The longest code in any deflate Huffman code. */
#define MAX_BITS 15
/* Output starts with this much room and doubles, up to the caller's cap. */
#define FIRST_ROOM 4096

unsigned long cu_adler32(unsigned long adler, CuStr data)
{
    const unsigned char *p;
    unsigned long a;
    unsigned long b;
    size_t i;
    size_t n;

    if ((data.ptr == NULL && data.len != 0) || adler > 0xFFFFFFFFUL ||
        (adler & 0xFFFF) >= ADLER_MOD || adler >> 16 >= ADLER_MOD)
        abort();
    p = (const unsigned char *)data.ptr;
    a = adler & 0xFFFF;
    b = adler >> 16;
    i = 0;
    while (i < data.len) {
        n = data.len - i;
        if (n > ADLER_RUN)
            n = ADLER_RUN;
        for (; n > 0; n--, i++) {
            a += p[i];
            b += a;
        }
        a %= ADLER_MOD;
        b %= ADLER_MOD;
    }
    return b << 16 | a;
}

void cu_sb_zlib(CuBuilder *b, CuStr data)
{
    size_t at;
    size_t n;

    if (b == NULL || (data.ptr == NULL && data.len != 0))
        abort();
    /* 0x78: deflate with a 32 KiB window. 0x01: no preset dictionary, the
     * "fastest" level, and check bits that make 0x7801 a multiple of 31;
     * zlib writes the same two bytes at level 0. */
    cu_sb_u8(b, 0x78);
    cu_sb_u8(b, 0x01);
    at = 0;
    do {
        n = data.len - at;
        if (n > STORED_MAX)
            n = STORED_MAX;
        /* BFINAL on the last block, BTYPE 00 (stored), then the length and
         * its complement on the byte boundary that follows. */
        cu_sb_u8(b, at + n == data.len ? 1 : 0);
        cu_sb_u16le(b, (unsigned long)n);
        cu_sb_u16le(b, (unsigned long)n ^ 0xFFFF);
        cu_sb_str(b, cu_str_n(data.ptr + at, n));
        at += n;
    } while (at < data.len);
    cu_sb_u32be(b, cu_adler32(1, data));
}

/* The input, read a bit at a time from the low bit of each byte up. Fewer
 * than 8 bits are ever held between reads, so dropping them reaches the
 * next byte boundary. */
typedef struct Bits {
    const unsigned char *p;
    size_t len;
    size_t at; /* next byte to load */
    unsigned long held;
    int n; /* bits in held */
} Bits;

/* The next count (0 to 16) bits as a number, first bit lowest. Returns 0
 * when the input ends first. */
static int take(Bits *s, int count, unsigned long *v)
{
    while (s->n < count) {
        if (s->at == s->len)
            return 0;
        s->held |= (unsigned long)s->p[s->at++] << s->n;
        s->n += 8;
    }
    *v = s->held & ((1UL << count) - 1);
    s->held >>= count;
    s->n -= count;
    return 1;
}

static void to_byte(Bits *s)
{
    s->held = 0;
    s->n = 0;
}

/* A canonical Huffman code: how many codes have each length, and the
 * symbols ordered by code. */
typedef struct Code {
    int count[MAX_BITS + 1];
    int symbol[288];
} Code;

enum { CODE_BAD, CODE_COMPLETE, CODE_INCOMPLETE };

/* Builds the code for n symbols with lengths len[0..n). An over-subscribed
 * set is bad. An incomplete one leaves bit strings that are no code. */
static int build(Code *c, const unsigned char *len, int n)
{
    int offset[MAX_BITS + 1];
    int left;
    int i;

    for (i = 0; i <= MAX_BITS; i++)
        c->count[i] = 0;
    for (i = 0; i < n; i++)
        c->count[len[i]]++;
    left = 1;
    for (i = 1; i <= MAX_BITS; i++) {
        left = left * 2 - c->count[i];
        if (left < 0)
            return CODE_BAD;
    }
    offset[1] = 0;
    for (i = 1; i < MAX_BITS; i++)
        offset[i + 1] = offset[i] + c->count[i];
    for (i = 0; i < n; i++)
        if (len[i] != 0)
            c->symbol[offset[len[i]]++] = i;
    return left == 0 ? CODE_COMPLETE : CODE_INCOMPLETE;
}

/* zlib's rule for the literal/length and distance codes: an incomplete
 * code is accepted only if it has no codes, or one code of one bit. */
static int usable(const Code *c, int n, int kind)
{
    return kind == CODE_COMPLETE ||
           (kind == CODE_INCOMPLETE && c->count[0] + c->count[1] == n);
}

/* Reads one symbol, its code's bits first to last. Fails on a bit string
 * that is no code and when the input ends. */
static int decode(Bits *s, const Code *c, int *sym)
{
    unsigned long bit;
    int code;
    int first;
    int index;
    int len;

    code = 0;
    first = 0;
    index = 0;
    for (len = 1; len <= MAX_BITS; len++) {
        if (!take(s, 1, &bit))
            return 0;
        code |= (int)bit;
        if (code - first < c->count[len]) {
            *sym = c->symbol[index + code - first];
            return 1;
        }
        index += c->count[len];
        first = (first + c->count[len]) * 2;
        code *= 2;
    }
    return 0;
}

/* The output, which is also the window that back-references copy from. */
typedef struct Out {
    CuArena *a;
    unsigned char *p;
    size_t len;
    size_t cap;
    size_t max;
} Out;

/* Makes room for n more bytes, so that o->p is never NULL after it
 * succeeds, even for none. Fails past the cap or out of memory. */
static int room(Out *o, size_t n)
{
    unsigned char *p;
    size_t want;
    size_t cap;

    if (n > o->max - o->len)
        return 0;
    want = o->len + n;
    if (want <= o->cap && o->p != NULL)
        return 1;
    cap = o->cap < FIRST_ROOM ? FIRST_ROOM : o->cap;
    while (cap < want)
        cap = cap > o->max / 2 ? o->max : cap * 2;
    if (cap > o->max)
        cap = o->max;
    p = (unsigned char *)cu_arena_grow(o->a, o->p, o->cap, cap);
    if (p == NULL)
        return 0;
    o->p = p;
    o->cap = cap;
    return 1;
}

static const unsigned short len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,
                                            15, 17, 19, 23, 27, 31, 35, 43,  51,  59,
                                            67, 83, 99, 115, 131, 163, 195, 227, 258};
static const unsigned char len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short dist_base[30] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const unsigned char dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                             6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* Decodes one Huffman block's symbols into o, through end of block. */
static int codes(Bits *s, Out *o, const Code *lit, const Code *dist)
{
    unsigned long extra;
    size_t length;
    size_t back;
    int sym;

    for (;;) {
        if (!decode(s, lit, &sym))
            return 0;
        if (sym < 256) {
            if (!room(o, 1))
                return 0;
            o->p[o->len++] = (unsigned char)sym;
            continue;
        }
        if (sym == 256)
            return 1;
        /* Fixed codes have symbols 286 and 287, which mean nothing. */
        sym -= 257;
        if (sym >= 29 || !take(s, len_extra[sym], &extra))
            return 0;
        length = len_base[sym] + (size_t)extra;
        if (!decode(s, dist, &sym) || sym >= 30 || !take(s, dist_extra[sym], &extra))
            return 0;
        back = dist_base[sym] + (size_t)extra;
        if (back > o->len || !room(o, length))
            return 0;
        /* Byte by byte, since the copy may overlap what it writes. */
        for (; length > 0; length--, o->len++)
            o->p[o->len] = o->p[o->len - back];
    }
}

static int stored(Bits *s, Out *o)
{
    size_t n;

    to_byte(s);
    if (s->len - s->at < 4)
        return 0;
    n = (size_t)s->p[s->at] | (size_t)s->p[s->at + 1] << 8;
    if ((s->p[s->at + 2] ^ 0xFF) != s->p[s->at] || (s->p[s->at + 3] ^ 0xFF) != s->p[s->at + 1])
        return 0;
    s->at += 4;
    if (s->len - s->at < n || !room(o, n))
        return 0;
    for (; n > 0; n--)
        o->p[o->len++] = s->p[s->at++];
    return 1;
}

/* The fixed codes, built on a stream's first fixed block and kept for the
 * rest, so a run of tiny fixed blocks costs no more per bit than any other
 * data. */
typedef struct Fixed {
    Code lit;
    Code dist;
    int built;
} Fixed;

static int fixed(Bits *s, Out *o, Fixed *f)
{
    unsigned char len[288];
    int i;

    if (!f->built) {
        for (i = 0; i < 288; i++)
            len[i] = (unsigned char)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        (void)build(&f->lit, len, 288);
        /* 32 five-bit codes, of which 30 and 31 are never valid distances. */
        for (i = 0; i < 32; i++)
            len[i] = 5;
        (void)build(&f->dist, len, 32);
        f->built = 1;
    }
    return codes(s, o, &f->lit, &f->dist);
}

static int dynamic(Bits *s, Out *o)
{
    static const unsigned char order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                            11, 4,  12, 3, 13, 2, 14, 1, 15};
    unsigned char len[286 + 30];
    unsigned long v;
    Code lencode;
    Code lit;
    Code dist;
    int nlen;
    int ndist;
    int ncode;
    int index;
    int sym;
    int repeat;
    int kind;
    unsigned char fill;

    if (!take(s, 5, &v))
        return 0;
    nlen = (int)v + 257;
    if (!take(s, 5, &v))
        return 0;
    ndist = (int)v + 1;
    if (!take(s, 4, &v))
        return 0;
    ncode = (int)v + 4;
    if (nlen > 286 || ndist > 30)
        return 0;
    for (index = 0; index < 19; index++)
        len[order[index]] = 0;
    for (index = 0; index < ncode; index++) {
        if (!take(s, 3, &v))
            return 0;
        len[order[index]] = (unsigned char)v;
    }
    if (build(&lencode, len, 19) != CODE_COMPLETE)
        return 0;
    index = 0;
    while (index < nlen + ndist) {
        if (!decode(s, &lencode, &sym))
            return 0;
        if (sym < 16) {
            len[index++] = (unsigned char)sym;
            continue;
        }
        /* 16 repeats the previous length 3 to 6 times; 17 and 18 write
         * runs of zeros. A run may cross from one code into the next. */
        if (sym == 16) {
            if (index == 0 || !take(s, 2, &v))
                return 0;
            fill = len[index - 1];
            repeat = 3 + (int)v;
        } else if (sym == 17) {
            if (!take(s, 3, &v))
                return 0;
            fill = 0;
            repeat = 3 + (int)v;
        } else {
            if (!take(s, 7, &v))
                return 0;
            fill = 0;
            repeat = 11 + (int)v;
        }
        if (repeat > nlen + ndist - index)
            return 0;
        for (; repeat > 0; repeat--)
            len[index++] = fill;
    }
    kind = build(&lit, len, nlen);
    if (!usable(&lit, nlen, kind))
        return 0;
    kind = build(&dist, len + nlen, ndist);
    if (!usable(&dist, ndist, kind))
        return 0;
    return codes(s, o, &lit, &dist);
}

int cu_zlib_inflate(CuArena *a, CuStr in, size_t max_out, CuStr *out, size_t *used)
{
    unsigned long last;
    unsigned long type;
    unsigned long sum;
    Bits s;
    Out o;
    Fixed f;
    int ok;

    if (a == NULL || out == NULL || used == NULL || (in.ptr == NULL && in.len != 0))
        abort();
    *out = cu_str_empty();
    s.p = (const unsigned char *)in.ptr;
    s.len = in.len;
    s.at = 2;
    s.held = 0;
    s.n = 0;
    /* Deflate, a window of at most 32 KiB, the check bits, and no preset
     * dictionary, which git never uses. */
    if (in.len < 2 || (s.p[0] & 0x0F) != 8 || s.p[0] >> 4 > 7 ||
        ((unsigned long)s.p[0] * 256 + s.p[1]) % 31 != 0 || (s.p[1] & 0x20) != 0)
        return 0;
    o.a = a;
    o.p = NULL;
    o.len = 0;
    o.cap = 0;
    o.max = max_out;
    f.built = 0;
    do {
        if (!take(&s, 1, &last) || !take(&s, 2, &type))
            return 0;
        if (type == 0)
            ok = stored(&s, &o);
        else if (type == 1)
            ok = fixed(&s, &o, &f);
        else if (type == 2)
            ok = dynamic(&s, &o);
        else
            ok = 0;
        if (!ok)
            return 0;
    } while (!last);
    to_byte(&s);
    if (s.len - s.at < 4)
        return 0;
    sum = (unsigned long)s.p[s.at] << 24 | (unsigned long)s.p[s.at + 1] << 16 |
          (unsigned long)s.p[s.at + 2] << 8 | (unsigned long)s.p[s.at + 3];
    *out = cu_str_n((const char *)o.p, o.len);
    if (cu_adler32(1, *out) != sum) {
        *out = cu_str_empty();
        return 0;
    }
    *used = s.at + 4;
    return 1;
}
