#include "death.h"
#include <assert.h>
#include <string.h>
#include "cu.h"

/* Digests are compared as lowercase hex. The NIST vectors come from FIPS
 * 180-2 and NIST's examples; the pattern vectors were computed with
 * Python's hashlib, independently of this code. */
static int digest_is(const unsigned char d[32], const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    char got[65];
    size_t i;

    for (i = 0; i < 32; i++) {
        got[2 * i] = digits[d[i] >> 4];
        got[2 * i + 1] = digits[d[i] & 0xF];
    }
    got[64] = '\0';
    return strcmp(got, hex) == 0;
}

static void test_nist_vectors(void)
{
    unsigned char d[32];

    cu_sha256(cu_str_empty(), d);
    assert(digest_is(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    cu_sha256(CU_LIT("abc"), d);
    assert(digest_is(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    cu_sha256(CU_LIT("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), d);
    assert(digest_is(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    cu_sha256(CU_LIT("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                     "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
              d);
    assert(digest_is(d, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
}

/* Lengths around each padding boundary: 55 and 56 bytes decide whether the
 * length fits in the last block, 63 to 65 cross a block. */
static const struct {
    size_t len;
    const char *hex;
} pattern[] = {
    {55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
    {56, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
    {57, "35df609437dcfea3279283ab79fd554e2bf78f8f7ae2de532d8ee300b09e8f73"},
    {63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
    {64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
    {65, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e"},
    {119, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e"},
    {120, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5"},
    {128, "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6"},
    {1000, "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371"},
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
    unsigned char d[32];
    size_t i;

    for (i = 0; i < sizeof pattern / sizeof pattern[0]; i++) {
        cu_sha256(cu_str_n((const char *)bytes, pattern[i].len), d);
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
    CuSha256 h;
    unsigned char d[32];
    size_t len;
    size_t i;
    size_t at;
    size_t n;

    len = pattern[which].len;
    for (i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        cu_sha256_init(&h);
        for (at = 0; at < len; at += n) {
            n = len - at < steps[i] ? len - at : steps[i];
            cu_sha256_update(&h, cu_str_n((const char *)bytes + at, n));
        }
        cu_sha256_final(&h, d);
        assert(digest_is(d, pattern[which].hex));
    }
}

/* One million 'a's, fed 1000 at a time: the NIST long-message vector.
 * Not run under Cerberus, where 15625 block compressions take too long. */
static void test_million_a(void)
{
    char block[1000];
    CuSha256 h;
    unsigned char d[32];
    size_t i;

    for (i = 0; i < sizeof block; i++)
        block[i] = 'a';
    cu_sha256_init(&h);
    for (i = 0; i < 1000; i++)
        cu_sha256_update(&h, cu_str_n(block, sizeof block));
    cu_sha256_final(&h, d);
    assert(digest_is(d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

/* An empty update changes nothing, and a hasher can be reused after
 * cu_sha256_init. */
static void test_empty_update_and_reuse(void)
{
    CuSha256 h;
    unsigned char d[32];

    cu_sha256_init(&h);
    cu_sha256_update(&h, cu_str_empty());
    cu_sha256_update(&h, CU_LIT("abc"));
    cu_sha256_update(&h, cu_str_empty());
    cu_sha256_final(&h, d);
    assert(digest_is(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    cu_sha256_init(&h);
    cu_sha256_final(&h, d);
    assert(digest_is(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

/* Lengths of 2^29 bytes and more, whose bit counts carry past 32 bits,
 * are too slow to hash in every test configuration. These tests set the
 * hasher's public byte count directly instead, as if that much had been
 * hashed; the digests come from a pure-Python SHA-256, checked against
 * hashlib, given the same claimed length. */
static void test_long_length_field(void)
{
    CuSha256 h;
    unsigned char d[32];

    cu_sha256_init(&h);
    h.len = cu_u64(0UL, 0x20000000UL);
    cu_sha256_final(&h, d);
    assert(digest_is(d, "0631d34041c92e76b584f8922943526982f9053a1b7895aeae06f641df905370"));
    cu_sha256_init(&h);
    h.len = cu_u64(1UL, 0x20000000UL);
    cu_sha256_update(&h, CU_LIT("abc"));
    cu_sha256_final(&h, d);
    assert(digest_is(d, "debdf63cdf5ff2812e357325c7269c7e60651904187460b5d391de2a24865df3"));
}

#ifdef CU_DEATH_TESTS
static void update_after_final(void)
{
    CuSha256 h;
    unsigned char d[32];

    cu_sha256_init(&h);
    cu_sha256_final(&h, d);
    cu_sha256_update(&h, CU_LIT("x"));
}

static void final_twice(void)
{
    CuSha256 h;
    unsigned char d[32];

    cu_sha256_init(&h);
    cu_sha256_final(&h, d);
    cu_sha256_final(&h, d);
}

static void null_hasher(void)
{
    cu_sha256_update(NULL, CU_LIT("x"));
}

static void null_digest(void)
{
    CuSha256 h;

    cu_sha256_init(&h);
    cu_sha256_final(&h, NULL);
}

/* 2^61 bytes is the first length whose bit count needs 65 bits. */
static void message_too_long(void)
{
    CuSha256 h;

    cu_sha256_init(&h);
    h.len = cu_u64(0x1FFFFFFFUL, 0xFFFFFFFFUL);
    cu_sha256_update(&h, CU_LIT("x"));
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
