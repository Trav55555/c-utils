#include "death.h"
#include <assert.h>
#include <string.h>
#include "cu.h"

/* Digests are compared as lowercase hex. The NIST vectors come from FIPS
 * 180-2 and NIST's examples; the pattern vectors were computed with
 * Python's hashlib, independently of this code. */
static int digest_is(const unsigned char d[20], const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    char got[41];
    size_t i;

    for (i = 0; i < 20; i++) {
        got[2 * i] = digits[d[i] >> 4];
        got[2 * i + 1] = digits[d[i] & 0xF];
    }
    got[40] = '\0';
    return strcmp(got, hex) == 0;
}

static void test_nist_vectors(void)
{
    unsigned char d[20];

    cu_sha1(cu_str_empty(), d);
    assert(digest_is(d, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    cu_sha1(CU_LIT("abc"), d);
    assert(digest_is(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    cu_sha1(CU_LIT("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), d);
    assert(digest_is(d, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    cu_sha1(CU_LIT("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                     "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
              d);
    assert(digest_is(d, "a49b2446a02c645bf419f995b67091253a04a259"));
}

/* Lengths around each padding boundary: 55 and 56 bytes decide whether the
 * length fits in the last block, 63 to 65 cross a block. */
static const struct {
    size_t len;
    const char *hex;
} pattern[] = {
    {55, "ddf57317ef34bfee3b6df83d359098930eb278bc"},
    {56, "a0d492bb0fc889d0eca3bc137066ab6f4f74f369"},
    {57, "11a02dcf95859677a62e75024067c22b165d890f"},
    {63, "c55856749bef509bdfe6bfebfc7bf4e793e82132"},
    {64, "bede92be29c3874e1b54ddc77988d606fc857a8e"},
    {65, "b05a80522b053d6dc7e0a517d0e70212c7dad11f"},
    {119, "504e27376a6e0f0dba8295b85cb25dc4dfa17d23"},
    {120, "82134b02fb3f702491be9bed581eeab59334acb2"},
    {128, "a09133e6730ffe899efb70204cb5646cd5dc24ee"},
    {1000, "4231a8a50a10fa9758db8ec71fdef855b751048a"},
};

static unsigned char bytes[1000];

static void fill_pattern(void)
{
    size_t i;

    for (i = 0; i < sizeof bytes; i++)
        bytes[i] = (unsigned char)((i * 7 + 3) & 0xFF);
}

static void test_padding_boundaries(void)
{
    unsigned char d[20];
    size_t i;

    for (i = 0; i < sizeof pattern / sizeof pattern[0]; i++) {
        cu_sha1(cu_str_n((const char *)bytes, pattern[i].len), d);
        assert(digest_is(d, pattern[i].hex));
    }
}

/* Streaming in pieces of every size from 1 to 70 bytes gives the one-shot
 * digest, so buffered and whole-block paths agree. Cerberus interprets
 * each block compression slowly enough that this would pass dcheck's
 * semantics time limit, so under it a 65-byte message is split three ways,
 * which still crosses a block boundary from a partly filled buffer. */
static void test_streaming_splits(void)
{
#ifdef __cerb__
    static const size_t steps[] = {1, 7, 64};
    const size_t which = 5;
#else
    static const size_t steps[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
                                   15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                                   29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
                                   43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56,
                                   57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70};
    const size_t which = 9;
#endif
    CuSha1 h;
    unsigned char d[20];
    size_t len;
    size_t i;
    size_t at;
    size_t n;

    len = pattern[which].len;
    for (i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        cu_sha1_init(&h);
        for (at = 0; at < len; at += n) {
            n = len - at < steps[i] ? len - at : steps[i];
            cu_sha1_update(&h, cu_str_n((const char *)bytes + at, n));
        }
        cu_sha1_final(&h, d);
        assert(digest_is(d, pattern[which].hex));
    }
}

/* One million 'a's, fed 1000 at a time: the NIST long-message vector.
 * Not run under Cerberus, where 15625 block compressions take too long. */
static void test_million_a(void)
{
    char block[1000];
    CuSha1 h;
    unsigned char d[20];
    size_t i;

    for (i = 0; i < sizeof block; i++)
        block[i] = 'a';
    cu_sha1_init(&h);
    for (i = 0; i < 1000; i++)
        cu_sha1_update(&h, cu_str_n(block, sizeof block));
    cu_sha1_final(&h, d);
    assert(digest_is(d, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

/* An empty update changes nothing, and a hasher can be reused after
 * cu_sha1_init. */
static void test_empty_update_and_reuse(void)
{
    CuSha1 h;
    unsigned char d[20];

    cu_sha1_init(&h);
    cu_sha1_update(&h, cu_str_empty());
    cu_sha1_update(&h, CU_LIT("abc"));
    cu_sha1_update(&h, cu_str_empty());
    cu_sha1_final(&h, d);
    assert(digest_is(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    cu_sha1_init(&h);
    cu_sha1_final(&h, d);
    assert(digest_is(d, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
}

/* Lengths of 2^29 bytes and more, whose bit counts carry past 32 bits,
 * are too slow to hash in every test configuration. These tests set the
 * hasher's public byte count directly instead, as if that much had been
 * hashed; the digests come from a pure-Python SHA-1, checked against
 * hashlib, given the same claimed length. */
static void test_long_length_field(void)
{
    CuSha1 h;
    unsigned char d[20];

    cu_sha1_init(&h);
    h.len = cu_u64(0UL, 0x20000000UL);
    cu_sha1_final(&h, d);
    assert(digest_is(d, "e1f400733164ed1642437ffce12ecd024ac97ef2"));
    cu_sha1_init(&h);
    h.len = cu_u64(1UL, 0x20000000UL);
    cu_sha1_update(&h, CU_LIT("abc"));
    cu_sha1_final(&h, d);
    assert(digest_is(d, "68d1eaadc3040f83eac504c0d1bccce369771fdd"));
}

#ifdef CU_DEATH_TESTS
static void update_after_final(void)
{
    CuSha1 h;
    unsigned char d[20];

    cu_sha1_init(&h);
    cu_sha1_final(&h, d);
    cu_sha1_update(&h, CU_LIT("x"));
}

static void final_twice(void)
{
    CuSha1 h;
    unsigned char d[20];

    cu_sha1_init(&h);
    cu_sha1_final(&h, d);
    cu_sha1_final(&h, d);
}

static void null_hasher(void)
{
    cu_sha1_update(NULL, CU_LIT("x"));
}

static void null_digest(void)
{
    CuSha1 h;

    cu_sha1_init(&h);
    cu_sha1_final(&h, NULL);
}

/* 2^61 bytes is the first length whose bit count needs 65 bits. */
static void message_too_long(void)
{
    CuSha1 h;

    cu_sha1_init(&h);
    h.len = cu_u64(0x1FFFFFFFUL, 0xFFFFFFFFUL);
    cu_sha1_update(&h, CU_LIT("x"));
}

/* Contract violations abort. */
static void death_tests(void)
{
    expect_abort(update_after_final);
    expect_abort(final_twice);
    expect_abort(null_hasher);
    expect_abort(null_digest);
    expect_abort(message_too_long);
}
#endif

int main(void)
{
    fill_pattern();
    test_nist_vectors();
    test_padding_boundaries();
    test_streaming_splits();
#ifndef __cerb__
    test_million_a();
#endif
    test_empty_update_and_reuse();
    test_long_length_field();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
