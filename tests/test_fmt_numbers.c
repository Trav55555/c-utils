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

#ifdef CU_DEATH_TESTS
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

#endif

int main(void)
{
    CuArena a;
    int debug;

    for (debug = 0; debug <= 1; debug++) {
        cu_arena_init_with(&a, 64, debug);
        test_parse_ulong(&a);
        test_parse_long(&a);
        assert(cu_arena_failed(&a) == 0);
        cu_arena_release(&a);
    }
    test_formats_match_reference();
    test_parse_ulong_bounds();
#ifdef CU_DEATH_TESTS
    expect_abort(parse_ulong_null_out);
    expect_abort(parse_long_null_out);
#endif
    return 0;
}
