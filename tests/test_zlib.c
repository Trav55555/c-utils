#include "death.h"
#include <assert.h>
#include <string.h>
#include "cu.h"
#include "zlib_vectors.h"

/* Inputs, made by the same rules as make_input in tests/zlib_vectors.py. */
enum { ZEROS, TEXT, RANDOM, RUNS, FAR };

static unsigned char input[70000];
static unsigned char mutated[8192];
static unsigned long lcg_state;

/* Masked to 31 bits, so a 32-bit unsigned long wraps to the same values. */
static unsigned long lcg(void)
{
    lcg_state = (lcg_state * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
    return lcg_state >> 16;
}

static CuStr make_input(int index)
{
    static const char *const words[10] = {"the",  "agent", "commit", "tree", "blob",
                                          "merge", "pack", "delta",  "zlib", "git"};
    int kind;
    size_t n;
    size_t at;
    size_t run;
    size_t i;
    const char *w;
    unsigned char c;

    kind = inputs[index].kind;
    n = inputs[index].n;
    assert(n <= sizeof input);
    lcg_state = (unsigned long)n + (unsigned long)kind;
    at = 0;
    if (kind == ZEROS || kind == FAR) {
        for (i = 0; i < n; i++)
            input[i] = 0;
    }
    if (kind == FAR) {
        for (i = 0; i < 300; i++) {
            input[i] = (unsigned char)(lcg() & 0xFF);
            input[n - 300 + i] = input[i];
        }
    }
    while (at < n && kind == TEXT) {
        for (w = words[lcg() % 10]; *w != '\0' && at < n; w++)
            input[at++] = (unsigned char)*w;
        c = (unsigned char)(lcg() % 8 == 0 ? '\n' : ' ');
        if (at < n)
            input[at++] = c;
    }
    while (at < n && kind == RANDOM)
        input[at++] = (unsigned char)(lcg() & 0xFF);
    while (at < n && kind == RUNS) {
        run = 1 + lcg() % 300;
        c = (unsigned char)(lcg() % 4);
        for (; run > 0 && at < n; run--)
            input[at++] = c;
    }
    return cu_str_n((const char *)input, n);
}

static int sha1_is(CuStr data, const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    unsigned char d[20];
    char got[41];
    size_t i;

    cu_sha1(data, d);
    for (i = 0; i < 20; i++) {
        got[2 * i] = digits[d[i] >> 4];
        got[2 * i + 1] = digits[d[i] & 0xF];
    }
    got[40] = '\0';
    return strcmp(got, hex) == 0;
}

/* Cerberus takes minutes over what native builds run in a second, so it
 * runs every STRIDE-th stream and crafted case; every one has run clean
 * under it in full, and native, sanitizer and valgrind builds run them all. */
#ifdef __cerb__
#define STREAM_STRIDE 6
#define CRAFTED_STRIDE 8
#define PREFIX_STREAMS 2
#else
#define STREAM_STRIDE 1
#define CRAFTED_STRIDE 1
#define PREFIX_STREAMS ((size_t)-1)
#endif

static CuStr view(const unsigned char *p, size_t n)
{
    return cu_str_n((const char *)p, n);
}

/* Values from Python's zlib.adler32. Runs of the largest byte need the
 * sums reduced in time: with 32-bit sums, about 5800 of them overflow b.
 * Cerberus runs the shortest run that does. */
#ifdef __cerb__
#define FF_LEN 6000
#define FF_SUM 0xA49759EAUL
#define FF_SUM_HIGH 0x75B659E8UL
#else
#define FF_LEN 100000
#define FF_SUM 0x149A302CUL
#define FF_SUM_HIGH 0x072C302AUL
#endif

static void test_adler32(void)
{
    static unsigned char ff[FF_LEN];
    size_t i;

    for (i = 0; i < sizeof ff; i++)
        ff[i] = 0xFF;
    assert(cu_adler32(1, cu_str_empty()) == 1);
    assert(cu_adler32(1, CU_LIT("a")) == 0x00620062UL);
    assert(cu_adler32(1, CU_LIT("abc")) == 0x024D0127UL);
    assert(cu_adler32(1, CU_LIT("Wikipedia")) == 0x11E60398UL);
    assert(cu_adler32(1, view(ff, sizeof ff)) == FF_SUM);
    assert(cu_adler32(0xFFF0FFF0UL, view(ff, sizeof ff)) == FF_SUM_HIGH);
    /* Continuing a sum is the same as one sum over both parts. */
    for (i = 0; i <= 6000; i += 3000)
        assert(cu_adler32(cu_adler32(1, view(ff, i)), view(ff + i, sizeof ff - i)) == FF_SUM);
}

/* Each stream zlib wrote inflates to its input with exactly its length
 * used, and not into one byte less. At level 0 zlib writes what cu_sb_zlib
 * does, for inputs that fit zlib's first stored block. */
static void check_stream(const struct stream *v, CuArena *a)
{
    CuTemp t;
    CuBuilder b;
    CuStr data;
    CuStr out;
    CuStr written;
    size_t used;

    t = cu_temp_begin(a);
    data = make_input(v->input);
    used = 0;
    assert(cu_zlib_inflate(a, view(v->z, v->len), data.len, &out, &used));
    assert(cu_str_eq(out, data));
    assert(used == v->len);
    if (data.len > 0) {
        used = 7;
        assert(!cu_zlib_inflate(a, view(v->z, v->len), data.len - 1, &out, &used));
        assert(out.len == 0 && used == 7);
    }
    if (v->level == 0 && data.len < 65531) {
        cu_sb_init(&b, a);
        cu_sb_zlib(&b, data);
        assert(cu_sb_finish(&b, &written));
        assert(cu_str_eq(written, view(v->z, v->len)));
    }
    cu_temp_end(t);
}

/* Every proper prefix of a stream is cut short. */
static void check_prefixes(const struct stream *v, CuArena *a)
{
    CuTemp t;
    CuStr out;
    size_t used;
    size_t n;

    for (n = 0; n < v->len; n++) {
        t = cu_temp_begin(a);
        assert(!cu_zlib_inflate(a, view(v->z, n), 1UL << 20, &out, &used));
        cu_temp_end(t);
    }
}

static void test_streams(void)
{
    CuArena a;
    size_t i;

    cu_arena_init(&a);
    for (i = 0; i < sizeof small_streams / sizeof small_streams[0]; i += STREAM_STRIDE) {
        check_stream(&small_streams[i], &a);
        if (i < PREFIX_STREAMS)
            check_prefixes(&small_streams[i], &a);
    }
#ifndef __cerb__
    for (i = 0; i < sizeof large_streams / sizeof large_streams[0]; i++) {
        check_stream(&large_streams[i], &a);
        if (large_streams[i].len <= 600)
            check_prefixes(&large_streams[i], &a);
    }
#endif
    cu_arena_release(&a);
}

#ifndef __cerb__
static const struct stream *stream_at(int index)
{
    size_t small;

    small = sizeof small_streams / sizeof small_streams[0];
    if ((size_t)index < small)
        return &small_streams[index];
    return &large_streams[(size_t)index - small];
}

/* One-byte changes to zlib's streams get zlib's verdict. */
static void test_mutations(void)
{
    const struct mutation *m;
    const struct stream *v;
    CuArena a;
    CuTemp t;
    CuStr out;
    size_t used;
    size_t n;
    size_t i;
    int ok;

    cu_arena_init(&a);
    for (i = 0; i < sizeof mutations / sizeof mutations[0]; i++) {
        m = &mutations[i];
        v = stream_at(m->base);
        assert(v->len <= sizeof mutated && m->at < v->len);
        memcpy(mutated, v->z, v->len);
        n = v->len;
        if (m->how == 0)
            mutated[m->at] = (unsigned char)(mutated[m->at] ^ (1U << m->v));
        else if (m->how == 1)
            mutated[m->at] = (unsigned char)m->v;
        else
            n = m->at;
        t = cu_temp_begin(&a);
        used = 0;
        ok = cu_zlib_inflate(&a, view(mutated, n), 1UL << 20, &out, &used);
        assert(ok == m->ok);
        if (ok)
            assert(used == m->used && out.len == m->out_len && sha1_is(out, m->sha1));
        cu_temp_end(t);
    }
    cu_arena_release(&a);
}
#endif

/* Streams that reach each rule about codes, lengths, distances, blocks,
 * headers and trailers, with zlib's verdict on each. */
static void test_crafted(void)
{
    CuArena a;
    CuStr out;
    size_t used;
    size_t i;
    int ok;

    cu_arena_init(&a);
    for (i = 0; i < sizeof crafted / sizeof crafted[0]; i += CRAFTED_STRIDE) {
        used = 0;
        ok = cu_zlib_inflate(&a, view(crafted[i].z, crafted[i].len), 1UL << 20, &out, &used);
        assert(ok == crafted[i].ok);
        if (ok)
            assert(used == crafted[i].used && out.len == crafted[i].out_len &&
                   sha1_is(out, crafted[i].sha1));
        else
            assert(out.len == 0 && used == 0);
    }
    cu_arena_release(&a);
}

/* A match one byte too far back would copy the byte before the output.
 * Whatever that byte is, the stream is refused, even with a trailer that
 * matches what such a copy would give. Cerberus, which looks for undefined
 * behaviour rather than this off-by-one, tries three of the bytes. */
#ifdef __cerb__
#define TOO_FAR_STEP 127
#else
#define TOO_FAR_STEP 1
#endif

static void check_too_far(const char *name, const char *prefix, size_t length, size_t back)
{
    const struct crafted *c;
    unsigned char buf[64];
    unsigned long sum;
    CuArena a;
    CuStr out;
    size_t used;
    size_t n;
    size_t i;
    int x;

    c = NULL;
    for (i = 0; i < sizeof crafted / sizeof crafted[0]; i++)
        if (strcmp(crafted[i].name, name) == 0)
            c = &crafted[i];
    assert(c != NULL && !c->ok && c->len <= sizeof mutated);
    n = strlen(prefix);
    assert(back == n + 1 && 1 + n + length <= sizeof buf);
    cu_arena_init(&a);
    for (x = 0; x < 256; x += TOO_FAR_STEP) {
        buf[0] = (unsigned char)x;
        memcpy(buf + 1, prefix, n);
        for (i = 1 + n; i < 1 + n + length; i++)
            buf[i] = buf[i - back];
        sum = cu_adler32(1, view(buf + 1, n + length));
        memcpy(mutated, c->z, c->len);
        for (i = 0; i < 4; i++)
            mutated[c->len - 4 + i] = (unsigned char)(sum >> (24 - 8 * i) & 0xFF);
        assert(!cu_zlib_inflate(&a, view(mutated, c->len), 1UL << 20, &out, &used));
    }
    cu_arena_release(&a);
}

static void test_too_far_back(void)
{
    check_too_far("distance too far back", "a", 3, 2);
    check_too_far("extra bits on distance code 4 too far", "abcd", 14, 5);
}

/* The output never grows past max_out: under a cap only a little above
 * it, inflating to exactly max_out bytes still fits, both below the first
 * room the output gets and after it has doubled. */
static void check_growth_capped(size_t n)
{
    CuArena scratch;
    CuArena a;
    CuBuilder b;
    CuStr data;
    CuStr z;
    CuStr out;
    char *p;
    size_t used;
    size_t i;

    cu_arena_init(&scratch);
    p = cu_arena_push_bytes(&scratch, n);
    assert(p != NULL);
    for (i = 0; i < n; i++)
        p[i] = (char)(i % 13);
    data = cu_str_n(p, n);
    cu_sb_init(&b, &scratch);
    cu_sb_zlib(&b, data);
    assert(cu_sb_finish(&b, &z));
    cu_arena_init_with(&a, 1024, 0);
    cu_arena_set_limit(&a, n + 1024);
    assert(cu_zlib_inflate(&a, z, n, &out, &used));
    assert(cu_str_eq(out, data));
    cu_arena_release(&a);
    cu_arena_release(&scratch);
}

static void test_growth_capped(void)
{
    check_growth_capped(100);
#ifndef __cerb__
    check_growth_capped(16385);
#endif
}

/* Stored blocks hold 65535 bytes each: lengths on each side of a block's
 * end take one block more, and the stream inflates back. */
static void test_writer_blocks(void)
{
    static const size_t lengths[] = {0, 1, 65534, 65535, 65536, 131070, 131071, 200000};
    CuArena a;
    CuTemp t;
    CuBuilder b;
    CuStr data;
    CuStr z;
    CuStr out;
    char *p;
    size_t blocks;
    size_t used;
    size_t at;
    size_t i;
    size_t k;

    cu_arena_init(&a);
    for (i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
#ifdef __cerb__
        if (lengths[i] > 1)
            break;
#endif
        t = cu_temp_begin(&a);
        p = cu_arena_push_bytes(&a, lengths[i]);
        assert(p != NULL);
        for (k = 0; k < lengths[i]; k++)
            p[k] = (char)(k * 7 % 251);
        data = cu_str_n(p, lengths[i]);
        cu_sb_init(&b, &a);
        cu_sb_zlib(&b, data);
        assert(cu_sb_finish(&b, &z));
        blocks = lengths[i] == 0 ? 1 : (lengths[i] + 65534) / 65535;
        assert(z.len == lengths[i] + 6 + 5 * blocks);
        /* Each block's first byte says whether it is the last. */
        for (at = 2, k = 0; k < blocks; k++) {
            assert(cu_str_at(z, at) == (k + 1 == blocks ? 1 : 0));
            at += 5 + (k + 1 == blocks ? lengths[i] - 65535 * k : 65535);
        }
        assert(at + 4 == z.len);
        assert(cu_zlib_inflate(&a, z, lengths[i], &out, &used));
        assert(cu_str_eq(out, data) && used == z.len);
        cu_temp_end(t);
    }
    cu_arena_release(&a);
}

/* A stream followed by more bytes, as in a pack, reports where it ends. */
static void test_used(void)
{
    CuArena a;
    CuBuilder b;
    CuStr z;
    CuStr out;
    size_t used;

    cu_arena_init(&a);
    cu_sb_init(&b, &a);
    cu_sb_zlib(&b, CU_LIT("first"));
    cu_sb_zlib(&b, CU_LIT("second"));
    assert(cu_sb_finish(&b, &z));
    assert(cu_zlib_inflate(&a, z, 100, &out, &used));
    assert(cu_str_eq(out, CU_LIT("first")) && used == 16);
    assert(cu_zlib_inflate(&a, cu_str_skip(z, used), 100, &out, &used));
    assert(cu_str_eq(out, CU_LIT("second")) && used == 17);
    /* A cap of 0 still takes an empty stream. */
    cu_sb_init(&b, &a);
    cu_sb_zlib(&b, cu_str_empty());
    assert(cu_sb_finish(&b, &z));
    assert(cu_zlib_inflate(&a, z, 0, &out, &used));
    assert(out.len == 0 && used == 11);
    cu_arena_release(&a);
}

/* Each block allocation failing alone fails the inflate cleanly, until a
 * run needs no more than have failed. */
static void test_out_of_memory(void)
{
    const struct stream *v;
    CuArena a;
    CuStr data;
    CuStr out;
    size_t used;
    size_t n;
    int ok;

    v = &small_streams[sizeof small_streams / sizeof small_streams[0] - 1];
#ifndef __cerb__
    /* The output of 30300 bytes grows through several sizes. */
    for (n = 0; inputs[large_streams[n].input].kind != FAR; n++)
        continue;
    v = &large_streams[n];
#endif
    data = make_input(v->input);
    for (n = 0;; n++) {
        cu_arena_init_with(&a, 1024, 0);
        cu_arena_fail_at(&a, n);
        used = 0;
        ok = cu_zlib_inflate(&a, view(v->z, v->len), data.len, &out, &used);
        if (ok) {
            assert(cu_str_eq(out, data) && a.attempts <= n);
            cu_arena_release(&a);
            break;
        }
        assert(cu_arena_failed(&a) && out.len == 0 && used == 0);
        cu_arena_release(&a);
    }
    assert(n > 0);
}

#ifdef CU_DEATH_TESTS
static void null_arena(void)
{
    CuStr out;
    size_t used;

    assert(!cu_zlib_inflate(NULL, cu_str_empty(), 0, &out, &used));
}

static void null_out(void)
{
    CuArena a;
    size_t used;

    cu_arena_init(&a);
    assert(!cu_zlib_inflate(&a, cu_str_empty(), 0, NULL, &used));
}

static void null_used(void)
{
    CuArena a;
    CuStr out;

    cu_arena_init(&a);
    assert(!cu_zlib_inflate(&a, cu_str_empty(), 0, &out, NULL));
}

static void null_builder(void)
{
    cu_sb_zlib(NULL, cu_str_empty());
}

/* 65521 is the modulus, so no sum has it in either half. */
static void adler_low_too_big(void)
{
    (void)cu_adler32(65521UL, cu_str_empty());
}

static void adler_high_too_big(void)
{
    (void)cu_adler32(65521UL << 16, cu_str_empty());
}

static void death_tests(void)
{
    expect_abort(null_arena);
    expect_abort(null_out);
    expect_abort(null_used);
    expect_abort(null_builder);
    expect_abort(adler_low_too_big);
    expect_abort(adler_high_too_big);
}
#endif

int main(void)
{
    test_adler32();
    test_streams();
#ifndef __cerb__
    test_mutations();
#endif
    test_crafted();
    test_too_far_back();
    test_growth_capped();
    test_writer_blocks();
    test_used();
    test_out_of_memory();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
