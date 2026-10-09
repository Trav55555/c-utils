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

int main(void)
{
    test_construct();
    test_eq_and_at();
    test_slicing_clamps();
    test_prefix_suffix_find();
    test_cut();
    test_trim_and_ascii();
    test_copy(0);
    test_copy(1);
    test_copy_failure(0);
    test_copy_failure(1);
    return 0;
}
