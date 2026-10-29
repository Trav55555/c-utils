#include "death.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cu.h"

/* Cerberus interprets every step, so under it the loops are smaller;
 * native runs keep the full workloads. KEYS must be coprime with 37. */
#ifdef __cerb__
#define KEYS 23
#define ROUND_TRIPS 16
#else
#define KEYS 200
#define ROUND_TRIPS 2000
#endif

/* Parses text, which must succeed. */
static CuJson parse(CuArena *a, const char *text)
{
    CuJson v;
    CuJsonError err;

    assert(cu_json_parse(a, cu_str_cstr(text), &v, &err));
    return v;
}

/* Parsing n bytes of text must fail at offset with reason. */
static void rejects_n(const char *text, size_t n, size_t offset, const char *reason)
{
    CuArena a;
    CuJson v;
    CuJsonError err;

    cu_arena_init(&a);
    assert(!cu_json_parse(&a, cu_str_n(text, n), &v, &err));
    assert(err.offset == offset);
    assert(strcmp(err.reason, reason) == 0);
    cu_arena_release(&a);
}

static void rejects(const char *text, size_t offset, const char *reason)
{
    rejects_n(text, strlen(text), offset, reason);
}

static int is(CuStr s, const char *bytes, size_t n)
{
    return s.len == n && (n == 0 || memcmp(s.ptr, bytes, n) == 0);
}

/* Every kind of value, whitespace around and between, and lookup. */
static void test_parse_values(void)
{
    CuArena a;
    CuJson v;
    const CuJson *f;

    cu_arena_init(&a);
    v = parse(&a, " \t\r\n{\"b\": [1, -2.5e+3, true, false, null, \"x\"], \"a\": {}, \"\": []} ");
    assert(v.kind == CU_JSON_OBJECT && v.n == 3);
    assert(is(v.keys[0], "b", 1) && is(v.keys[1], "a", 1) && is(v.keys[2], "", 0));
    f = cu_json_find(&v, cu_str_cstr("b"));
    assert(f != NULL && f->kind == CU_JSON_ARRAY && f->n == 6);
    assert(f->items[0].kind == CU_JSON_NUMBER && is(f->items[0].text, "1", 1));
    assert(f->items[1].kind == CU_JSON_NUMBER && is(f->items[1].text, "-2.5e+3", 7));
    assert(f->items[2].kind == CU_JSON_BOOL && f->items[2].truth == 1);
    assert(f->items[3].kind == CU_JSON_BOOL && f->items[3].truth == 0);
    assert(f->items[4].kind == CU_JSON_NULL);
    assert(f->items[5].kind == CU_JSON_STRING && is(f->items[5].text, "x", 1));
    f = cu_json_find(&v, cu_str_cstr("a"));
    assert(f != NULL && f->kind == CU_JSON_OBJECT && f->n == 0);
    assert(cu_json_find(f, cu_str_cstr("a")) == NULL);
    f = cu_json_find(&v, cu_str_empty());
    assert(f != NULL && f->kind == CU_JSON_ARRAY && f->n == 0);
    assert(cu_json_find(&v, cu_str_cstr("c")) == NULL);
    assert(cu_json_find(&v, cu_str_cstr("bb")) == NULL);

    v = parse(&a, "\"top\"");
    assert(v.kind == CU_JSON_STRING && is(v.text, "top", 3));
    v = parse(&a, "0");
    assert(v.kind == CU_JSON_NUMBER && is(v.text, "0", 1));
    cu_arena_release(&a);
}

/* Lookup finds every key of a larger object, whatever its input order. */
static void test_find_many(void)
{
    CuArena a;
    CuBuilder b;
    CuStr text;
    CuJson v;
    const CuJson *f;
    char key[32];
    long got;
    long i;

    cu_arena_init(&a);
    cu_sb_init(&b, &a);
    cu_sb_char(&b, '{');
    for (i = 0; i < KEYS; i++) {
        if (i > 0)
            cu_sb_char(&b, ',');
        cu_sb_str(&b, cu_str_cstr("\"k"));
        cu_sb_long(&b, i * 37 % KEYS);
        cu_sb_str(&b, cu_str_cstr("\":"));
        cu_sb_long(&b, i * 37 % KEYS);
    }
    cu_sb_char(&b, '}');
    assert(cu_sb_finish(&b, &text));
    v = parse(&a, "{}");
    {
        CuJsonError err;

        assert(cu_json_parse(&a, text, &v, &err));
    }
    assert(v.n == KEYS);
    for (i = 0; i < KEYS; i++) {
        sprintf(key, "k%ld", i);
        f = cu_json_find(&v, cu_str_cstr(key));
        assert(f != NULL && cu_json_to_long(f, &got) && got == i);
    }
    sprintf(key, "k%ld", (long)KEYS);
    assert(cu_json_find(&v, cu_str_cstr(key)) == NULL);
    assert(cu_json_find(&v, cu_str_cstr("k")) == NULL);
    cu_arena_release(&a);
}

/* n copies of item, separated by commas, between open and close. */
static CuStr repeated(CuArena *a, const char *open, const char *item, size_t n,
                      const char *close)
{
    CuBuilder b;
    CuStr text;
    size_t i;

    cu_sb_init(&b, a);
    cu_sb_str(&b, cu_str_cstr(open));
    for (i = 0; i < n; i++) {
        if (i > 0 && close[0] != '"')
            cu_sb_char(&b, ',');
        cu_sb_str(&b, cu_str_cstr(item));
    }
    cu_sb_str(&b, cu_str_cstr(close));
    assert(cu_sb_finish(&b, &text));
    return text;
}

#ifndef __cerb__
/* Bytes held by a fresh arena (not in debug mode, so pushes share
 * blocks) after parsing text. */
static size_t held_after(CuStr text, CuJson *v)
{
    CuArena a;
    CuJsonError err;
    size_t held;

    cu_arena_init_with(&a, 65536, 0);
    assert(cu_json_parse(&a, text, v, &err));
    held = a.held;
    cu_arena_release(&a);
    return held;
}

/* The tree is sized exactly: the bound the header gives holds for the
 * worst input, and a long string takes its decoded bytes and no more. */
static void test_memory(void)
{
    CuArena a;
    CuArena small;
    CuStr text;
    CuJson v;
    CuJsonError err;
    size_t held;

    cu_arena_init(&a);
    text = repeated(&a, "[", "0", 50000, "]");
    held = held_after(text, &v);
    assert(v.n == 50000);
    assert(held <= 30 * text.len);
    assert(held >= 50000 * sizeof(CuJson));

    text = repeated(&a, "{\"k\":[", "\"\"", 50000, "]}");
    held = held_after(text, &v);
    assert(held <= 30 * text.len);
    text = repeated(&a, "[", "[[[[1]]]]", 20000, "]");
    held = held_after(text, &v);
    assert(held <= 32 * text.len);

    /* 200000 escapes of two decoded bytes each, in one string. */
    text = repeated(&a, "\"", "\\u00e9", 200000, "\"");
    held = held_after(text, &v);
    assert(v.kind == CU_JSON_STRING && v.text.len == 400000);
    assert(held <= 400000 + 65536 + 1024);

    /* A cap fails the parse cleanly. */
    text = repeated(&a, "[", "0", 50000, "]");
    cu_arena_init_with(&small, 4096, 0);
    cu_arena_set_limit(&small, 100000);
    assert(!cu_json_parse(&small, text, &v, &err));
    assert(strcmp(err.reason, "out of memory") == 0);
    cu_arena_release(&small);
    cu_arena_release(&a);
}
#endif

/* Each allocation failing in turn fails the parse with "out of memory",
 * until there are none left to fail. */
static void test_out_of_memory(void)
{
    static const char doc[] =
        "{\"a\": [1, \"two\", {\"b\": null, \"c\": [true, \"\\u00e9\"]}], \"d\": \"x\"}";
    CuArena a;
    CuJson v;
    CuJsonError err;
    size_t n;
    int ok;

    for (n = 0;; n++) {
        cu_arena_init_with(&a, 64, 1);
        cu_arena_fail_at(&a, n);
        ok = cu_json_parse(&a, cu_str_cstr(doc), &v, &err);
        if (ok) {
            assert(a.attempts <= n);
            assert(cu_json_find(&v, cu_str_cstr("d")) != NULL);
            cu_arena_release(&a);
            break;
        }
        assert(strcmp(err.reason, "out of memory") == 0);
        cu_arena_release(&a);
    }
    assert(n > 5);
}

/* Integers that fit, and every way a number can fail to. */
static void test_numbers(void)
{
    CuArena a;
    CuJson v;
    CuU64 u;
    long l;

    cu_arena_init(&a);
    v = parse(&a, "18446744073709551615");
    assert(cu_json_to_u64(&v, &u) && u.hi == 0xFFFFFFFFUL && u.lo == 0xFFFFFFFFUL);
    v = parse(&a, "4294967296");
    assert(cu_json_to_u64(&v, &u) && u.hi == 1 && u.lo == 0);
    v = parse(&a, "18446744073709551616");
    u = cu_u64(0, 7);
    assert(!cu_json_to_u64(&v, &u) && u.hi == 0 && u.lo == 7);
    v = parse(&a, "-1");
    assert(!cu_json_to_u64(&v, &u));
    assert(cu_json_to_long(&v, &l) && l == -1);
    v = parse(&a, "1.0");
    assert(!cu_json_to_u64(&v, &u) && !cu_json_to_long(&v, &l));
    v = parse(&a, "1e3");
    assert(!cu_json_to_u64(&v, &u) && !cu_json_to_long(&v, &l));
    v = parse(&a, "-2147483648");
    assert(cu_json_to_long(&v, &l) && l == -2147483647L - 1);
    v = parse(&a, "99999999999999999999");
    assert(!cu_json_to_long(&v, &l));
    v = parse(&a, "\"1\"");
    assert(!cu_json_to_u64(&v, &u) && !cu_json_to_long(&v, &l));

    rejects("01", 1, "bytes after the value");
    rejects("-", 0, "unexpected character");
    rejects("1.", 0, "unexpected character");
    rejects(".5", 0, "unexpected character");
    rejects("1e", 0, "unexpected character");
    rejects("+1", 0, "unexpected character");
    cu_arena_release(&a);
}

/* Escapes decode, including surrogate pairs and NUL; UTF-8 must be valid. */
static void test_strings(void)
{
    CuArena a;
    CuJson v;

    cu_arena_init(&a);
    v = parse(&a, "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"");
    assert(is(v.text, "\"\\/\b\f\n\r\t", 8));
    v = parse(&a, "\"\\u00e9\\u20AC\\ud83d\\ude00\\u0000\"");
    assert(is(v.text, "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\0", 10));
    v = parse(&a, "\"\xc3\xa9\xf4\x8f\xbf\xbf\"");
    assert(is(v.text, "\xc3\xa9\xf4\x8f\xbf\xbf", 6));
    v = parse(&a, "\"\"");
    assert(v.kind == CU_JSON_STRING && v.text.len == 0);

    rejects("\"abc", 0, "unterminated string");
    rejects("\"\\", 1, "unterminated string");
    rejects("\"\\x\"", 1, "bad escape");
    rejects("\"\\u12\"", 1, "bad \\u escape");
    rejects("\"\\udc00\"", 1, "lone low surrogate");
    rejects("\"\\ud800\"", 1, "lone high surrogate");
    rejects("\"\\ud800\\u0041\"", 1, "lone high surrogate");
    rejects("\"a\tb\"", 2, "raw control character in string");
    rejects("\"\xc0\x80\"", 1, "invalid UTF-8");
    rejects("\"\xe0\x80\xaf\"", 1, "invalid UTF-8");
    rejects("\"\xf0\x80\x80\xaf\"", 1, "invalid UTF-8");
    rejects("\"\xed\xa0\x80\"", 1, "invalid UTF-8");
    rejects("\"\xf4\x90\x80\x80\"", 1, "invalid UTF-8");
    rejects("\"\xe2\x82\"", 1, "invalid UTF-8");
    rejects("\"\x80\"", 1, "invalid UTF-8");
    rejects_n("\"a\0\"", 4, 2, "raw control character in string");
    cu_arena_release(&a);
}

/* Structure errors say where. */
static void test_structure_errors(void)
{
    rejects("", 0, "expected a value");
    rejects("   ", 3, "expected a value");
    rejects("1 2", 2, "bytes after the value");
    rejects("[1,]", 3, "unexpected character");
    rejects("[1 2]", 3, "expected ',' or ']'");
    rejects("{\"a\" 1}", 5, "expected ':'");
    rejects("{\"a\":1,}", 7, "expected a key");
    rejects("{1:2}", 1, "expected a key");
    rejects("{\"a\":1 \"b\":2}", 7, "expected ',' or '}'");
    rejects("{\"a\":1,\"b\":2,\"a\":3}", 0, "duplicate key");
    rejects("[{\"\":1,\"\":1}]", 1, "duplicate key");
    rejects("nul", 0, "unexpected character");
    rejects("truex", 4, "bytes after the value");
    rejects("[", 1, "expected a value");
    rejects("// x\n1", 0, "unexpected character");
}

/* ---- The JSONTestSuite corpus ---- */

#if !defined(__wasi__) && !defined(__cerb__)
#define CORPUS "tests/data/jsontestsuite"

/* RFC 8259 only says an object's names SHOULD differ, so the corpus counts
 * repeats as valid. They are refused here on purpose: parsers that keep
 * different copies of a repeated key read one input two ways. */
static int duplicate_keys(CuStr name)
{
    return cu_str_eq(name, cu_str_cstr("y_object_duplicated_key.json")) ||
           cu_str_eq(name, cu_str_cstr("y_object_duplicated_key_and_value.json"));
}

/* y_ files parse, n_ files fail, i_ files do either. */
static void test_corpus(void)
{
    CuArena a;
    CuArena each;
    CuDir d;
    CuDirEntry e;
    CuStr text;
    CuJson v;
    CuJsonError err;
    char path[512];
    int got;
    int ok;
    size_t files;

    cu_arena_init(&a);
    assert(cu_dir_open(&d, &a, CORPUS));
    files = 0;
    while ((got = cu_dir_next(&d, &e)) == 1) {
        if (e.name.len < 7 || !cu_str_ends_with(e.name, cu_str_cstr(".json")))
            continue;
        assert(strlen(CORPUS) + 1 + e.name.len < sizeof path);
        strcpy(path, CORPUS "/");
        memcpy(path + strlen(CORPUS) + 1, e.name.ptr, e.name.len);
        path[strlen(CORPUS) + 1 + e.name.len] = '\0';
        cu_arena_init(&each);
        assert(cu_file_read(&each, path, &text));
        ok = cu_json_parse(&each, text, &v, &err);
        if (duplicate_keys(e.name))
            assert(!ok && strcmp(err.reason, "duplicate key") == 0);
        else if (e.name.ptr[0] == 'y')
            assert(ok);
        else if (e.name.ptr[0] == 'n')
            assert(!ok);
        cu_arena_release(&each);
        files++;
    }
    assert(got == 0);
    assert(files == 318);
    cu_dir_close(&d);
    cu_arena_release(&a);
}
#endif

#ifdef CU_DEATH_TESTS
static void value_without_key(void)
{
    CuArena a;
    CuJsonOut o;

    cu_arena_init(&a);
    cu_json_out_init(&o, &a);
    cu_json_begin_object(&o);
    cu_json_null(&o);
}

static void key_in_array(void)
{
    CuArena a;
    CuJsonOut o;

    cu_arena_init(&a);
    cu_json_out_init(&o, &a);
    cu_json_begin_array(&o);
    cu_json_key(&o, cu_str_cstr("k"));
}

static void end_mismatched(void)
{
    CuArena a;
    CuJsonOut o;

    cu_arena_init(&a);
    cu_json_out_init(&o, &a);
    cu_json_begin_array(&o);
    cu_json_end_object(&o);
}

static void end_after_key(void)
{
    CuArena a;
    CuJsonOut o;

    cu_arena_init(&a);
    cu_json_out_init(&o, &a);
    cu_json_begin_object(&o);
    cu_json_key(&o, cu_str_cstr("k"));
    cu_json_end_object(&o);
}

static void two_top_values(void)
{
    CuArena a;
    CuJsonOut o;

    cu_arena_init(&a);
    cu_json_out_init(&o, &a);
    cu_json_null(&o);
    cu_json_null(&o);
}

static void finish_open(void)
{
    CuArena a;
    CuJsonOut o;
    CuStr s;
    int ok;

    cu_arena_init(&a);
    cu_json_out_init(&o, &a);
    cu_json_begin_array(&o);
    ok = cu_json_out_finish(&o, &s);
    (void)ok;
}

static void too_deep(void)
{
    CuArena a;
    CuJsonOut o;
    size_t i;

    cu_arena_init(&a);
    cu_json_out_init(&o, &a);
    for (i = 0; i <= CU_JSON_MAX_DEPTH; i++)
        cu_json_begin_array(&o);
}

static void find_in_array(void)
{
    CuArena a;
    CuJson v;

    cu_arena_init(&a);
    v = parse(&a, "[]");
    (void)cu_json_find(&v, cu_str_cstr("k"));
}

static void number_null(void)
{
    CuArena a;
    CuJsonOut o;
    CuStr text;

    cu_arena_init(&a);
    cu_json_out_init(&o, &a);
    text.ptr = NULL;
    text.len = 1;
    cu_json_number(&o, text);
}

static void death_tests(void)
{
    expect_abort(value_without_key);
    expect_abort(key_in_array);
    expect_abort(end_mismatched);
    expect_abort(end_after_key);
    expect_abort(two_top_values);
    expect_abort(finish_open);
    expect_abort(too_deep);
    expect_abort(find_in_array);
    expect_abort(number_null);
}
#endif

int main(void)
{
    test_parse_values();
    test_find_many();
    /* Native sizes and alignment are what it measures, and its inputs are
     * far too big to interpret in reasonable time. */
#ifndef __cerb__
    test_memory();
#endif
    test_out_of_memory();
    test_numbers();
    test_strings();
    test_structure_errors();
#if !defined(__wasi__) && !defined(__cerb__)
    test_corpus();
#endif
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    cu_scratch_release();
    return 0;
}
