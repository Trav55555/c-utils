#include <assert.h>
#include <limits.h>
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

/* Each run raises the arena's limit to held + refused, just enough for the
 * allocation that failed last time, so every allocation fails in turn. */
static void test_append_fails_at_every_allocation(int debug)
{
    CuArena a;
    CuBuilder b;
    CuStr out;
    size_t limit;
    size_t next;
    int i;
    int done;

    done = 0;
    for (limit = 0; !done; limit = next) {
        cu_arena_init_with(&a, 64, debug);
        cu_arena_set_limit(&a, limit);
        cu_sb_init(&b, &a);
        for (i = 0; i < 40; i++)
            cu_sb_str(&b, CU_LIT("0123456789"));
        done = cu_sb_finish(&b, &out);
        if (done)
            assert(out.len == 400 && cu_str_starts_with(out, CU_LIT("01234567890123")));
        else
            assert(out.len == 0 && cu_arena_failed(&a));
        /* Every run gets one allocation further. */
        next = a.held + a.refused;
        assert(done || next > limit);
        cu_arena_release(&a);
    }
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
        test_parse_ulong(&a);
        test_parse_long(&a);
        assert(cu_arena_failed(&a) == 0);
        cu_arena_release(&a);
    }
    return 0;
}
