#include "death.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
#include "cu.h"

/* Builder and parsing together: every formatted number must parse back. */

static CuStr fmt_ulong(CuArena *a, unsigned long v)
{
    CuBuilder b;
    CuStr out;

    cu_sb_init(&b, a);
    cu_sb_ulong(&b, v);
    assert(cu_sb_finish(&b, &out));
    return out;
}

static CuStr fmt_long(CuArena *a, long v)
{
    CuBuilder b;
    CuStr out;

    cu_sb_init(&b, a);
    cu_sb_long(&b, v);
    assert(cu_sb_finish(&b, &out));
    return out;
}

static void test_builder_appends(CuArena *a)
{
    CuBuilder b;
    CuStr out;

    cu_sb_init(&b, a);
    cu_sb_str(&b, CU_LIT("n="));
    cu_sb_long(&b, -42);
    cu_sb_char(&b, ' ');
    cu_sb_hex(&b, 255, 4);
    cu_sb_char(&b, ' ');
    cu_sb_hex(&b, 0, 0);
    cu_sb_char(&b, ' ');
    cu_sb_ulong(&b, 0);
    assert(cu_sb_finish(&b, &out));
    assert(cu_str_eq(out, CU_LIT("n=-42 00ff 0 0")));
}

static void test_builder_empty(CuArena *a)
{
    CuBuilder b;
    CuStr out;

    cu_sb_init(&b, a);
    cu_sb_str(&b, cu_str_empty());
    assert(cu_sb_finish(&b, &out));
    assert(out.len == 0 && out.ptr != NULL);
}

static void test_builder_grows(CuArena *a)
{
    CuBuilder b;
    CuStr out;
    size_t i;

    cu_sb_init(&b, a);
    for (i = 0; i < 1000; i++)
        cu_sb_char(&b, (char)('a' + (char)(i % 26)));
    assert(cu_sb_finish(&b, &out));
    assert(out.len == 1000 && out.ptr[0] == 'a' && out.ptr[999] == 'l');
}

static void test_builder_failure_is_sticky(CuArena *a)
{
    CuBuilder b;
    CuStr out;
    char one;

    one = 'x';
    cu_sb_init(&b, a);
    cu_sb_char(&b, 'a');
    /* A length that cannot fit after one byte: the append fails without reading. */
    cu_sb_str(&b, cu_str_n(&one, (size_t)-1));
    cu_sb_char(&b, 'b');
    assert(cu_sb_finish(&b, &out) == 0);
    assert(out.len == 0);
}

static void test_parse_ulong(CuArena *a)
{
    unsigned long v;
    CuStr max;
    char *bumped;

    assert(cu_parse_ulong(CU_LIT("0"), 10, &v) && v == 0);
    assert(cu_parse_ulong(CU_LIT("12345"), 10, &v) && v == 12345);
    assert(cu_parse_ulong(CU_LIT("ff"), 16, &v) && v == 255);
    assert(cu_parse_ulong(CU_LIT("FF"), 16, &v) && v == 255);
    assert(cu_parse_ulong(CU_LIT("101"), 2, &v) && v == 5);
    assert(cu_parse_ulong(CU_LIT("zz"), 36, &v) && v == 1295);
    v = 9;
    assert(!cu_parse_ulong(CU_LIT(""), 10, &v) && v == 9);
    assert(!cu_parse_ulong(CU_LIT("12a"), 10, &v) && v == 9);
    assert(!cu_parse_ulong(CU_LIT("2"), 2, &v));
    assert(!cu_parse_ulong(CU_LIT(" 1"), 10, &v));
    assert(!cu_parse_ulong(CU_LIT("+1"), 10, &v));
    assert(!cu_parse_ulong(CU_LIT("-1"), 10, &v));

    max = fmt_ulong(a, ULONG_MAX);
    assert(cu_parse_ulong(max, 10, &v) && v == ULONG_MAX);
    /* ULONG_MAX is 2^n - 1, so its last decimal digit is 1, 3, 5 or 7. */
    bumped = (char *)cu_str_cstr_copy(a, max);
    assert(bumped != NULL);
    bumped[max.len - 1] = (char)(bumped[max.len - 1] + 1);
    assert(!cu_parse_ulong(cu_str_cstr(bumped), 10, &v));
}

/* Writes v in base, either letter case, into the end of buf; returns the
 * first digit. */
static char *ref_digits(unsigned long v, unsigned base, int upper, char *end)
{
    const char *digits;

    digits = upper ? "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ" : "0123456789abcdefghijklmnopqrstuvwxyz";
    do {
        *--end = digits[v % base];
        v /= base;
    } while (v != 0);
    return end;
}

/* In each base tried (both ends, powers of two, and odd ones; every base
 * takes too long under Cerberus), the overflow boundary is exact: ULONG_MAX parses, and so
 * does ULONG_MAX / base followed by any digit up to ULONG_MAX % base; the
 * next digit, or one digit more, overflows. */
static void test_parse_ulong_bounds(void)
{
    static const unsigned bases[] = {2, 3, 7, 8, 10, 16, 35, 36};
    char buf[80];
    size_t b;
    char *end;
    char *first;
    unsigned base;
    unsigned long v;
    unsigned long lim;
    unsigned long rem;
    unsigned long d;
    const char *digits;

    digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    end = buf + sizeof buf - 1;
    for (b = 0; b < sizeof bases / sizeof bases[0]; b++) {
        base = bases[b];
        first = ref_digits(ULONG_MAX, base, (int)(base % 2), end);
        assert(cu_parse_ulong(cu_str_n(first, (size_t)(end - first)), base, &v) && v == ULONG_MAX);
        *end = '0';
        assert(!cu_parse_ulong(cu_str_n(first, (size_t)(end - first) + 1), base, &v));
        lim = ULONG_MAX / base;
        rem = ULONG_MAX % base;
        first = ref_digits(lim, base, 0, end);
        for (d = 0; d < base; d++) {
            *end = digits[d];
            v = 7;
            if (d <= rem)
                assert(cu_parse_ulong(cu_str_n(first, (size_t)(end - first) + 1), base, &v) &&
                       v == lim * base + d);
            else
                assert(!cu_parse_ulong(cu_str_n(first, (size_t)(end - first) + 1), base, &v) && v == 7);
        }
        /* Bytes next to the digit and letter ranges are not digits. */
        assert(!cu_parse_ulong(CU_LIT("/"), base, &v) && !cu_parse_ulong(CU_LIT(":"), base, &v));
        assert(!cu_parse_ulong(CU_LIT("@"), base, &v) && !cu_parse_ulong(CU_LIT("["), base, &v));
        assert(!cu_parse_ulong(CU_LIT("`"), base, &v) && !cu_parse_ulong(CU_LIT("{"), base, &v));
        assert(!cu_parse_ulong(cu_str_n("\xb0", 1), base, &v));
    }
}

static void test_parse_long(CuArena *a)
{
    long v;
    CuStr s;
    char *bumped;

    assert(cu_parse_long(CU_LIT("-42"), 10, &v) && v == -42);
    assert(cu_parse_long(CU_LIT("+42"), 10, &v) && v == 42);
    assert(cu_parse_long(CU_LIT("-0"), 10, &v) && v == 0);
    assert(cu_parse_long(CU_LIT("-ff"), 16, &v) && v == -255);
    v = 9;
    assert(!cu_parse_long(CU_LIT("-"), 10, &v) && v == 9);
    assert(!cu_parse_long(CU_LIT("--1"), 10, &v) && v == 9);

    s = fmt_long(a, LONG_MAX);
    assert(cu_parse_long(s, 10, &v) && v == LONG_MAX);
    bumped = (char *)cu_str_cstr_copy(a, s);
    assert(bumped != NULL);
    bumped[s.len - 1] = (char)(bumped[s.len - 1] + 1);
    assert(!cu_parse_long(cu_str_cstr(bumped), 10, &v));

    s = fmt_long(a, LONG_MIN);
    assert(cu_parse_long(s, 10, &v) && v == LONG_MIN);
    bumped = (char *)cu_str_cstr_copy(a, s);
    assert(bumped != NULL);
    bumped[s.len - 1] = (char)(bumped[s.len - 1] + 1);
    assert(!cu_parse_long(cu_str_cstr(bumped), 10, &v));
}

/* Padding is reserved in one piece: a width that cannot fit fails at once
 * instead of appending zeros one at a time until memory runs out. */
static void test_hex_padding(int debug)
{
    CuArena a;
    CuBuilder b;
    CuStr out;

    cu_arena_init_with(&a, 64, debug);
    cu_sb_init(&b, &a);
    cu_sb_hex(&b, 0xabcUL, 6);
    cu_sb_hex(&b, 0xabcUL, 2);
    assert(cu_sb_finish(&b, &out) && cu_str_eq(out, CU_LIT("000abcabc")));

    cu_arena_set_limit(&a, a.held + 4096);
    cu_sb_init(&b, &a);
    cu_sb_hex(&b, 0, (size_t)-1);
    assert(!cu_sb_finish(&b, &out));
    cu_sb_init(&b, &a);
    cu_sb_hex(&b, 0, (size_t)1 << 20);
    assert(!cu_sb_finish(&b, &out) && cu_arena_failed(&a));
    cu_arena_release(&a);
}

/* Run n fails allocation n alone, so each allocation fails in turn while
 * the ones after it succeed; the runs end when one makes no more than n
 * allocations, and only that run may succeed. */
static void test_append_fails_at_every_allocation(int debug)
{
    CuArena a;
    CuBuilder b;
    CuStr out;
    size_t at;
    int injected;
    int i;
    int done;

    for (at = 0;; at++) {
        cu_arena_init_with(&a, 64, debug);
        cu_arena_fail_at(&a, at);
        cu_sb_init(&b, &a);
        for (i = 0; i < 40; i++)
            cu_sb_str(&b, CU_LIT("0123456789"));
        done = cu_sb_finish(&b, &out);
        if (done)
            assert(out.len == 400 && cu_str_starts_with(out, CU_LIT("01234567890123")));
        else
            assert(out.len == 0 && cu_arena_failed(&a));
        /* The run succeeds exactly when the failure was never reached. */
        injected = a.attempts > at;
        assert(done == !injected);
        cu_arena_release(&a);
        if (!injected)
            break;
    }
}

/* Appends v to b as cu_sb_ulong and cu_sb_hex, and the same text, built
 * by ref_digits, to want at *w. Padding and signs have their own tests. */
static void add_formats(CuBuilder *b, char *want, size_t *w, unsigned long v)
{
    char buf[80];
    char *end;
    char *first;
    size_t n;

    end = buf + sizeof buf;
    cu_sb_ulong(b, v);
    cu_sb_char(b, ' ');
    cu_sb_hex(b, v, 0);
    cu_sb_char(b, ' ');

    first = ref_digits(v, 10, 0, end);
    n = (size_t)(end - first);
    memcpy(want + *w, first, n);
    want[*w + n] = ' ';
    *w += n + 1;
    first = ref_digits(v, 16, 0, end);
    n = (size_t)(end - first);
    memcpy(want + *w, first, n);
    want[*w + n] = ' ';
    *w += n + 1;
}

/* Every value below 100 and every multiple of 101 below 10000 (each digit
 * pair, in both places the decimal writer uses its table), every power of ten with its neighbours, and
 * every power of two and the value below it, against a plain
 * divide-by-base reference. One builder holds it all: per-call setup is
 * what makes this slow under Cerberus. The loops count rather than shift
 * until 0, which Cerberus (git-b3e11ea) does not wrap. */
static void test_formats_match_reference(void)
{
    static char want[20000];
    CuArena a;
    CuBuilder b;
    CuStr out;
    unsigned long v;
    unsigned long p;
    size_t k;
    size_t w;

    cu_arena_init(&a);
    cu_sb_init(&b, &a);
    w = 0;
    for (v = 0; v < 100; v++) {
        add_formats(&b, want, &w, v);
        add_formats(&b, want, &w, v * 101);
    }
    p = 1;
    while (p <= ULONG_MAX / 10) {
        p *= 10;
        add_formats(&b, want, &w, p - 1);
        add_formats(&b, want, &w, p);
        add_formats(&b, want, &w, p + 1);
    }
    for (k = 1; k < sizeof(unsigned long) * CHAR_BIT; k++) {
        add_formats(&b, want, &w, (1UL << k) - 1);
        add_formats(&b, want, &w, 1UL << k);
    }
    add_formats(&b, want, &w, ULONG_MAX);
    assert(w <= sizeof want);
    assert(cu_sb_finish(&b, &out) && cu_str_eq(out, cu_str_n(want, w)));
    cu_arena_release(&a);
}

/* A view from cu_sb_finish stays valid while the builder grows, and a
 * builder can append its own contents: growth never frees the old buffer.
 * Past its block, the builder takes a new block rather than reallocating. */
static void test_finished_views_survive_growth(int debug)
{
    CuArena a;
    CuBuilder b;
    CuStr first;
    CuStr s;
    size_t blocks;
    size_t i;

    cu_arena_init_with(&a, 64, debug);
    cu_sb_init(&b, &a);
    cu_sb_str(&b, CU_LIT("hello"));
    assert(cu_sb_finish(&b, &first));
    blocks = a.blocks;
    for (i = 0; i < 200; i++)
        cu_sb_char(&b, 'x');
    assert(a.blocks > blocks);
    assert(cu_str_eq(first, CU_LIT("hello")));

    /* Self-append: the source is the builder's own buffer. */
    cu_sb_init(&b, &a);
    for (i = 0; i < 40; i++)
        cu_sb_char(&b, 'y');
    assert(cu_sb_finish(&b, &s));
    cu_sb_str(&b, s);
    cu_sb_str(&b, s);
    assert(cu_sb_finish(&b, &s) && s.len == 120);
    for (i = 0; i < 120; i++)
        assert(s.ptr[i] == 'y');
    cu_arena_release(&a);
}

/* A builder that nothing else pushes after grows in place while its
 * block has room: the same buffer, no new block, nothing left behind. */
static void test_growth_in_place(void)
{
    CuArena a;
    CuBuilder b;
    CuStr out;
    char *first;
    size_t blocks;
    size_t held;
    size_t i;
    size_t n;

    n = 1000;
    cu_arena_init_with(&a, 4096, 0);
    cu_sb_init(&b, &a);
    cu_sb_char(&b, 'a');
    first = b.data;
    blocks = a.blocks;
    held = a.held;
    for (i = 1; i < n; i++)
        cu_sb_char(&b, (char)('a' + (char)(i % 26)));
    assert(cu_sb_finish(&b, &out) && out.len == n && out.ptr == first);
    assert(a.blocks == blocks && a.held == held);
    for (i = 0; i < n; i++)
        assert(out.ptr[i] == (char)('a' + (char)(i % 26)));
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
static void char_null_builder(void)
{
    cu_sb_char(NULL, 'x');
}

static void empty_str_null_builder(void)
{
    cu_sb_str(NULL, cu_str_empty());
}

static void hex_null_builder(void)
{
    cu_sb_hex(NULL, 1, 4);
}

static void finish_null_builder(void)
{
    CuStr out;

    assert(cu_sb_finish(NULL, &out));
}

static void finish_null_out(void)
{
    CuArena a;
    CuBuilder b;

    cu_arena_init(&a);
    cu_sb_init(&b, &a);
    assert(cu_sb_finish(&b, NULL));
}

/* Valid input, so without the check the parser would write through NULL
 * and die by SIGSEGV, not SIGABRT. */
static void parse_ulong_null_out(void)
{
    int ok;

    ok = cu_parse_ulong(CU_LIT("1"), 10, NULL);
    (void)ok;
}

static void parse_long_null_out(void)
{
    int ok;

    ok = cu_parse_long(CU_LIT("-1"), 10, NULL);
    (void)ok;
}

static void death_tests(void)
{
    expect_abort(char_null_builder);
    expect_abort(empty_str_null_builder);
    expect_abort(hex_null_builder);
    expect_abort(finish_null_builder);
    expect_abort(finish_null_out);
    expect_abort(parse_ulong_null_out);
    expect_abort(parse_long_null_out);
}
#endif

/* Until cu_sb_finish hands out a view, nothing can point into the
 * builder's buffer, so growth past its block reallocates instead of
 * leaving every outgrown copy behind: the arena holds less than twice the
 * contents (doubling) plus a block, not about 3.4 times. */
static void test_unshared_growth_leaves_nothing_behind(void)
{
    CuArena a;
    CuBuilder b;
    CuStr out;
    size_t n;
    size_t i;

    /* Just past a power of two: the worst case for doubling. */
    n = (1u << 20) + 1;
    cu_arena_init_with(&a, 65536, 0);
    cu_sb_init(&b, &a);
    for (i = 0; i < n; i++)
        cu_sb_char(&b, (char)('a' + i % 26));
    assert(cu_sb_finish(&b, &out) && out.len == n);
    for (i = 0; i < n; i += 4099)
        assert(out.ptr[i] == (char)('a' + i % 26));
    assert(a.held < 2 * n + 65536);
    cu_arena_release(&a);
}

/* After a finish, the next growth copies, so the view survives; the copy
 * has no views yet, so later growth reallocates again. */
static void test_growth_after_finish(int debug)
{
    CuArena a;
    CuBuilder b;
    CuStr first;
    CuStr out;
    size_t i;

    cu_arena_init_with(&a, 64, debug);
    cu_sb_init(&b, &a);
    for (i = 0; i < 100; i++)
        cu_sb_char(&b, 'p');
    assert(cu_sb_finish(&b, &first) && first.len == 100);
    for (i = 0; i < 5000; i++)
        cu_sb_char(&b, (char)('0' + i % 10));
    assert(cu_sb_finish(&b, &out) && out.len == 5100);
    for (i = 0; i < 100; i++)
        assert(first.ptr[i] == 'p' && out.ptr[i] == 'p');
    for (i = 0; i < 5000; i++)
        assert(out.ptr[100 + i] == (char)('0' + i % 10));
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);
}

/* Appending the builder's own bytes, read straight from its data field
 * with no finish, survives a growth that reallocates the buffer. */
static void test_unshared_self_append(int debug)
{
    CuArena a;
    CuBuilder b;
    CuStr out;
    size_t i;
    int round;

    cu_arena_init_with(&a, 64, debug);
    cu_sb_init(&b, &a);
    cu_sb_str(&b, CU_LIT("ab"));
    for (round = 0; round < 12; round++)
        cu_sb_str(&b, cu_str_n(b.data, b.len));
    assert(cu_sb_finish(&b, &out) && out.len == (size_t)2 << 12);
    for (i = 0; i < out.len; i++)
        assert(out.ptr[i] == (i % 2 ? 'b' : 'a'));
    cu_arena_release(&a);
}

int main(void)
{
    CuArena a;
    int debug;

    for (debug = 0; debug <= 1; debug++) {
        cu_arena_init_with(&a, 64, debug);
        test_builder_appends(&a);
        test_builder_empty(&a);
        test_builder_grows(&a);
        test_builder_failure_is_sticky(&a);
        test_append_fails_at_every_allocation(debug);
        test_hex_padding(debug);
        test_finished_views_survive_growth(debug);
        test_growth_after_finish(debug);
        test_unshared_self_append(debug);
        test_parse_ulong(&a);
        test_parse_long(&a);
        assert(cu_arena_failed(&a) == 0);
        cu_arena_release(&a);
    }
    test_formats_match_reference();
    test_growth_in_place();
    test_unshared_growth_leaves_nothing_behind();
    test_parse_ulong_bounds();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
