#include "death.h"
#include <assert.h>
#include <string.h>
#include "cu.h"

static void test_construct(void)
{
    CuStr s;

    s = CU_LIT("hello");
    assert(s.len == 5 && memcmp(s.ptr, "hello", 5) == 0);
    s = cu_str_cstr("abc");
    assert(s.len == 3);
    s = cu_str_cstr(NULL);
    assert(s.len == 0 && s.ptr != NULL);
    s = cu_str_n(NULL, 0);
    assert(s.len == 0 && s.ptr != NULL);
    s = cu_str_empty();
    assert(s.len == 0 && s.ptr != NULL);
}

static void test_eq_and_at(void)
{
    assert(cu_str_eq(CU_LIT("abc"), cu_str_cstr("abc")));
    assert(!cu_str_eq(CU_LIT("abc"), CU_LIT("abd")));
    assert(!cu_str_eq(CU_LIT("abc"), CU_LIT("ab")));
    assert(cu_str_eq(cu_str_empty(), CU_LIT("")));
    assert(cu_str_at(CU_LIT("xyz"), 2) == 'z');
}

/* The header fast path reads what cu_str_at reads. */
static void test_at_fast(void)
{
    CuStr s;
    size_t i;

    s = CU_LIT("hello");
    for (i = 0; i < s.len; i++)
        assert(CU_STR_AT_FAST(s, i) == cu_str_at(s, i));
}

static void test_slicing_clamps(void)
{
    CuStr s;
    CuStr end;

    s = CU_LIT("abcdef");
    assert(cu_str_eq(cu_str_prefix(s, 2), CU_LIT("ab")));
    assert(cu_str_eq(cu_str_prefix(s, 99), s));
    assert(cu_str_eq(cu_str_prefix(s, 7), s));
    assert(cu_str_eq(cu_str_prefix(s, 6), s));
    assert(cu_str_eq(cu_str_skip(s, 4), CU_LIT("ef")));
    assert(cu_str_eq(cu_str_skip(s, 99), CU_LIT("")));
    assert(cu_str_eq(cu_str_substr(s, 1, 3), CU_LIT("bc")));
    assert(cu_str_eq(cu_str_substr(s, 4, 99), CU_LIT("ef")));
    assert(cu_str_eq(cu_str_substr(s, 5, 2), CU_LIT("")));
    end = cu_str_skip(s, 99);
    assert(end.ptr == s.ptr + 6);
}

static void test_prefix_suffix_find(void)
{
    CuStr s;
    size_t at;

    s = CU_LIT("key=value=x");
    assert(cu_str_starts_with(s, CU_LIT("key")));
    assert(!cu_str_starts_with(CU_LIT("k"), CU_LIT("key")));
    assert(cu_str_ends_with(s, CU_LIT("=x")));
    assert(!cu_str_ends_with(s, CU_LIT("y")));
    assert(!cu_str_ends_with(CU_LIT("x"), CU_LIT("=x")));
    assert(cu_str_find(s, CU_LIT("="), &at) && at == 3);
    assert(cu_str_find(s, CU_LIT("value"), &at) && at == 4);
    assert(cu_str_find(s, CU_LIT(""), &at) && at == 0);
    at = 77;
    assert(!cu_str_find(s, CU_LIT("vx"), &at) && at == 77);
    assert(!cu_str_find(CU_LIT("ab"), CU_LIT("abc"), &at));
    assert(cu_str_find(CU_LIT("aab"), CU_LIT("ab"), &at) && at == 1);
}

/* The first match by brute force, or (size_t)-1. */
static size_t naive_find(const char *s, size_t n, const char *needle, size_t m)
{
    size_t i;

    for (i = 0; m <= n && i <= n - m; i++) {
        if (memcmp(s + i, needle, m) == 0)
            return i;
    }
    return (size_t)-1;
}

/* cu_str_find agrees with brute force on one case. */
static void hay_case(const char *hay, const char *needle)
{
    size_t want;
    size_t at;
    int found;

    want = naive_find(hay, strlen(hay), needle, strlen(needle));
    at = 7;
    found = cu_str_find(cu_str_cstr(hay), cu_str_cstr(needle), &at);
    assert(found == (want != (size_t)-1));
    assert(found ? at == want : at == 7);
}

static unsigned long rng_state = 12345;

static unsigned rng(unsigned n)
{
    rng_state = (rng_state * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
    return (unsigned)((rng_state >> 8) % n);
}

/* Haystacks that start with a long run of the needle's first byte, or of
 * its first few bytes, make nearly every position a candidate: enough to
 * switch to the linear-time search, which then must agree with brute
 * force over the rest, for needles periodic or not, short ones included. */
static void test_find_on_adversarial_input(void)
{
    static const char alphabet[] = "aab";
    char hay[256];
    char needle[24];
    size_t n;
    size_t m;
    size_t k;
    size_t run;
    size_t want;
    size_t at;
    int found;
    int round;

    for (round = 0; round < 80; round++) {
        m = 2 + rng(21);
        needle[0] = 'a';
        for (k = 1; k < m; k++)
            needle[k] = alphabet[rng(round % 3 == 0 ? 3 : 2)];
        if (round % 5 == 0) {
            run = 1 + rng(3);
            for (k = run; k < m; k++)
                needle[k] = needle[k % run];
            if (m > 2 && rng(2))
                needle[m - 1 - rng(2)] = 'c';
        }
        run = 40 + rng(120);
        n = run + rng(60);
        for (k = 0; k < run; k++)
            hay[k] = round % 2 ? 'a' : needle[k % (1 + rng(2))];
        for (k = run; k < n; k++)
            hay[k] = alphabet[rng(3)];
        if (rng(2) && n + m <= sizeof hay) {
            k = run - rng(20);
            memcpy(hay + k, needle, m);
            if (k + m > n)
                n = k + m;
        }
        want = naive_find(hay, n, needle, m);
        at = 7;
        found = cu_str_find(cu_str_n(hay, n), cu_str_n(needle, m), &at);
        assert(found == (want != (size_t)-1));
        assert(found ? at == want : at == 7);
    }
    /* A periodic needle whose right part matches where its left part does
     * not, so the search shifts by the period. */
    hay_case("aaaaaaaaaaaaaaaaaaaaaaabbabaaaaabaaaaabaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
             "aaaaabaaaaa");
    /* The last candidate fails: the loop ends without memchr failing. */
    hay_case("ab", "ac");
    /* Dense candidates send two-byte needles to Two-Way too. */
    hay_case("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab", "ab");
    hay_case("abababababababababababababababababababababac", "ac");
    hay_case("abababababababababababababababababababababab", "ac");
}

static void test_cut(void)
{
    CuStr before;
    CuStr after;

    assert(cu_str_cut(CU_LIT("key=value=x"), CU_LIT("="), &before, &after));
    assert(cu_str_eq(before, CU_LIT("key")) && cu_str_eq(after, CU_LIT("value=x")));
    assert(!cu_str_cut(CU_LIT("novalue"), CU_LIT("="), &before, &after));
    assert(cu_str_eq(before, CU_LIT("novalue")) && after.len == 0);
}

static void test_trim_and_ascii(void)
{
    assert(cu_str_eq(cu_str_trim(CU_LIT(" \t x y \r\n")), CU_LIT("x y")));
    assert(cu_str_eq(cu_str_trim(CU_LIT("   ")), CU_LIT("")));
    assert(cu_ascii_is_digit('7') && !cu_ascii_is_digit('a'));
    assert(cu_ascii_is_alpha('Q') && cu_ascii_is_alpha('q') && !cu_ascii_is_alpha('1'));
    assert(cu_ascii_is_space('\v') && !cu_ascii_is_space('x'));
    assert(!cu_ascii_is_alpha((char)0xE9) && !cu_ascii_is_space((char)0xA0));
    assert(cu_ascii_to_lower('A') == 'a' && cu_ascii_to_lower('z') == 'z');
    assert(cu_ascii_to_lower((char)0xC9) == (char)0xC9);
}

static void test_copy(int debug)
{
    CuArena a;
    char buf[4];
    CuStr view;
    CuStr copy;
    CuStr none;
    const char *c;

    cu_arena_init_with(&a, 64, debug);
    memcpy(buf, "abcd", 4);
    view = cu_str_n(buf, 4);
    copy = cu_str_copy(&a, view);
    buf[0] = 'z';
    assert(cu_str_eq(copy, CU_LIT("abcd")));
    none = cu_str_copy(&a, cu_str_empty());
    assert(none.len == 0);
    c = cu_str_cstr_copy(&a, cu_str_prefix(copy, 2));
    assert(c != NULL && strcmp(c, "ab") == 0);
    assert(cu_arena_failed(&a) == 0);
    cu_arena_release(&a);
}

/* With no room at all, copies report failure instead of returning bad
 * views or pointers. */
static void test_copy_failure(int debug)
{
    CuArena a;
    CuStr copy;

    cu_arena_init_with(&a, 64, debug);
    cu_arena_set_limit(&a, 0);
    copy = cu_str_copy(&a, CU_LIT("abcd"));
    assert(copy.len == 0 && copy.ptr != NULL);
    assert(cu_str_cstr_copy(&a, CU_LIT("ab")) == NULL);
    assert(cu_arena_failed(&a));
    /* An empty copy needs no memory, so it still succeeds. */
    copy = cu_str_copy(&a, cu_str_empty());
    assert(copy.len == 0);
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
static void at_past_end(void)
{
    assert(cu_str_at(CU_LIT("ab"), 2) == 0);
}

/* Out of bounds, the fast path falls back to cu_str_at, which aborts. */
static void at_fast_past_end(void)
{
    CuStr s;

    s = CU_LIT("ab");
    assert(CU_STR_AT_FAST(s, 2) == 0);
}

static void find_null_at(void)
{
    assert(cu_str_find(CU_LIT("ab"), CU_LIT("b"), NULL));
}

static void find_empty_null_at(void)
{
    assert(cu_str_find(CU_LIT("ab"), cu_str_empty(), NULL));
}

static void cut_null_before(void)
{
    CuStr after;

    assert(cu_str_cut(CU_LIT("a=b"), CU_LIT("="), NULL, &after));
}

static void cut_null_after(void)
{
    CuStr before;

    assert(cu_str_cut(CU_LIT("a=b"), CU_LIT("="), &before, NULL));
}

/* An empty copy needs no memory, but the arena is still required. */
static void copy_null_arena(void)
{
    CuStr copy;

    copy = cu_str_copy(NULL, cu_str_empty());
    assert(copy.len == 0);
}

static void cstr_copy_null_arena(void)
{
    assert(cu_str_cstr_copy(NULL, cu_str_empty()) != NULL);
}

/* The overflow path, which used to mark the arena before checking it. */
static void cstr_copy_null_arena_overflow(void)
{
    char one;

    one = 'x';
    assert(cu_str_cstr_copy(NULL, cu_str_n(&one, (size_t)-1)) == NULL);
}

static void death_tests(void)
{
    expect_abort(at_past_end);
    expect_abort(at_fast_past_end);
    expect_abort(find_null_at);
    expect_abort(find_empty_null_at);
    expect_abort(cut_null_before);
    expect_abort(cut_null_after);
    expect_abort(copy_null_arena);
    expect_abort(cstr_copy_null_arena);
    expect_abort(cstr_copy_null_arena_overflow);
}
#endif

int main(void)
{
    test_construct();
    test_eq_and_at();
    test_at_fast();
    test_slicing_clamps();
    test_prefix_suffix_find();
    test_find_on_adversarial_input();
    test_cut();
    test_trim_and_ascii();
    test_copy(0);
    test_copy(1);
    test_copy_failure(0);
    test_copy_failure(1);
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
