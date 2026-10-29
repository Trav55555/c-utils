/* The depth, writer, u64 and Base64 cases of test_json.c, in a file of their
 * own: under Cerberus a run slows as it goes, and the whole file took 265 s
 * of dcheck's 300 s per-file limit on a loaded host. The rest of
 * test_json.c, renamed out of the way, supplies the loop sizes and
 * helpers. */
int test_json_main(void);
#define main test_json_main
#include "test_json.c"
#undef main

/* CU_JSON_MAX_DEPTH containers nest; one more is refused. */
static void test_depth(void)
{
    CuArena a;
    CuBuilder b;
    CuStr text;
    CuJson v;
    CuJsonError err;
    size_t i;

    cu_arena_init(&a);
    cu_sb_init(&b, &a);
    for (i = 0; i < CU_JSON_MAX_DEPTH; i++)
        cu_sb_char(&b, '[');
    for (i = 0; i < CU_JSON_MAX_DEPTH; i++)
        cu_sb_char(&b, ']');
    assert(cu_sb_finish(&b, &text));
    assert(cu_json_parse(&a, text, &v, &err));
    cu_sb_init(&b, &a);
    for (i = 0; i <= CU_JSON_MAX_DEPTH; i++)
        cu_sb_char(&b, '[');
    assert(cu_sb_finish(&b, &text));
    assert(!cu_json_parse(&a, text, &v, &err));
    assert(err.offset == CU_JSON_MAX_DEPTH && strcmp(err.reason, "nested too deeply") == 0);
    cu_arena_release(&a);
}

/* ---- Writer ---- */

static void expect_out(CuJsonOut *o, const char *want)
{
    CuStr got;

    assert(cu_json_out_finish(o, &got));
    assert(is(got, want, strlen(want)));
}

/* Every kind of value, commas only between items, escaping as JSON
 * requires. */
static void test_writer(void)
{
    CuArena a;
    CuJsonOut o;
    CuStr got;

    cu_arena_init(&a);
    cu_json_out_init(&o, &a);
    cu_json_begin_object(&o);
    cu_json_key(&o, cu_str_cstr("s"));
    cu_json_string(&o, cu_str_n("q\"b\\n\n\r\t\x01\x1f\x7f\xc3\xa9", 13));
    cu_json_key(&o, cu_str_cstr("n"));
    cu_json_begin_array(&o);
    cu_json_u64(&o, cu_u64(0, 0));
    cu_json_u64(&o, cu_u64(1, 0));
    cu_json_u64(&o, cu_u64(0xFFFFFFFFUL, 0xFFFFFFFFUL));
    cu_json_long(&o, -42);
    cu_json_number(&o, cu_str_cstr("-2.5e+3"));
    cu_json_end_array(&o);
    cu_json_key(&o, cu_str_cstr("e"));
    cu_json_begin_object(&o);
    cu_json_end_object(&o);
    cu_json_key(&o, cu_str_cstr("x"));
    cu_json_begin_array(&o);
    cu_json_bool(&o, 1);
    cu_json_bool(&o, 0);
    cu_json_null(&o);
    cu_json_begin_array(&o);
    cu_json_end_array(&o);
    cu_json_end_array(&o);
    cu_json_end_object(&o);
    expect_out(&o, "{\"s\":\"q\\\"b\\\\n\\n\\r\\t\\u0001\\u001f\x7f\xc3\xa9\","
                   "\"n\":[0,4294967296,18446744073709551615,-42,-2.5e+3],"
                   "\"e\":{},\"x\":[true,false,null,[]]}");

    cu_json_out_init(&o, &a);
    cu_json_string(&o, cu_str_n("\0", 1));
    expect_out(&o, "\"\\u0000\"");

    /* Invalid UTF-8 and a non-number fail the writer, which still closes
     * what it opened. */
    cu_json_out_init(&o, &a);
    cu_json_begin_array(&o);
    cu_json_string(&o, cu_str_n("a\xff", 2));
    cu_json_end_array(&o);
    assert(!cu_json_out_finish(&o, &got) && got.len == 0);
    cu_json_out_init(&o, &a);
    cu_json_number(&o, cu_str_cstr("01"));
    assert(!cu_json_out_finish(&o, &got));
    cu_json_out_init(&o, &a);
    cu_json_begin_object(&o);
    cu_json_key(&o, cu_str_n("\xed\xa0\x80", 3));
    cu_json_null(&o);
    cu_json_end_object(&o);
    assert(!cu_json_out_finish(&o, &got));
    cu_arena_release(&a);
}

/* A key repeated in one object fails the writer, since the parser would
 * refuse what it wrote; the same key in different objects, or one that
 * only shares a prefix or needs escaping, does not. */
static void test_writer_keys(void)
{
    CuArena a;
    CuJsonOut o;
    CuStr got;

    cu_arena_init(&a);
    cu_json_out_init(&o, &a);
    cu_json_begin_object(&o);
    cu_json_key(&o, cu_str_cstr("a"));
    cu_json_begin_object(&o);
    cu_json_key(&o, cu_str_cstr("a"));
    cu_json_null(&o);
    cu_json_key(&o, cu_str_cstr("a\""));
    cu_json_null(&o);
    cu_json_end_object(&o);
    cu_json_key(&o, cu_str_cstr("ab"));
    cu_json_begin_array(&o);
    cu_json_begin_object(&o);
    cu_json_key(&o, cu_str_cstr("a"));
    cu_json_null(&o);
    cu_json_end_object(&o);
    cu_json_end_array(&o);
    cu_json_key(&o, cu_str_cstr("\""));
    cu_json_null(&o);
    cu_json_end_object(&o);
    expect_out(&o, "{\"a\":{\"a\":null,\"a\\\"\":null},\"ab\":[{\"a\":null}],\"\\\"\":null}");

    cu_json_out_init(&o, &a);
    cu_json_begin_object(&o);
    cu_json_key(&o, cu_str_cstr("x"));
    cu_json_null(&o);
    cu_json_key(&o, cu_str_cstr("a\"b"));
    cu_json_null(&o);
    cu_json_key(&o, cu_str_cstr("y"));
    cu_json_null(&o);
    cu_json_key(&o, cu_str_cstr("a\"b"));
    cu_json_null(&o);
    cu_json_end_object(&o);
    assert(!cu_json_out_finish(&o, &got) && got.len == 0);

    /* A repeat in an inner object fails the whole document. */
    cu_json_out_init(&o, &a);
    cu_json_begin_array(&o);
    cu_json_begin_object(&o);
    cu_json_key(&o, cu_str_cstr(""));
    cu_json_null(&o);
    cu_json_key(&o, cu_str_cstr(""));
    cu_json_null(&o);
    cu_json_end_object(&o);
    cu_json_end_array(&o);
    assert(!cu_json_out_finish(&o, &got));
    cu_arena_release(&a);
}

/* Every u64 written parses back to itself, across digit-count edges. */
static void test_u64_round_trip(void)
{
    CuArena a;
    CuJsonOut o;
    CuStr text;
    CuJson v;
    CuJsonError err;
    CuU64 x;
    CuU64 back;
    unsigned long seed;
    int i;

    cu_arena_init(&a);
    seed = 12345;
    for (i = 0; i < ROUND_TRIPS; i++) {
        seed = (seed * 1103515245UL + 12345UL) & 0xFFFFFFFFUL;
        x = cu_u64(i % 3 == 0 ? 0 : seed, (seed * 2654435761UL) & 0xFFFFFFFFUL);
        if (i % 7 == 0)
            x = cu_u64(0, (unsigned long)i);
        cu_json_out_init(&o, &a);
        cu_json_u64(&o, x);
        assert(cu_json_out_finish(&o, &text));
        assert(cu_json_parse(&a, text, &v, &err));
        assert(cu_json_to_u64(&v, &back) && cu_u64_cmp(back, x) == 0);
    }
    cu_arena_release(&a);
}

/* ---- Base64 ---- */

static void base64_is(const char *bytes, const char *text)
{
    CuArena a;
    CuBuilder b;
    CuStr got;

    cu_arena_init(&a);
    cu_sb_init(&b, &a);
    cu_sb_base64(&b, cu_str_cstr(bytes));
    assert(cu_sb_finish(&b, &got) && is(got, text, strlen(text)));
    assert(cu_base64_decode(&a, cu_str_cstr(text), &got));
    assert(is(got, bytes, strlen(bytes)));
    cu_arena_release(&a);
}

static void base64_rejects(const char *text)
{
    CuArena a;
    CuStr got;

    cu_arena_init(&a);
    assert(!cu_base64_decode(&a, cu_str_cstr(text), &got) && got.len == 0);
    cu_arena_release(&a);
}

/* RFC 4648's vectors, every byte value, and every non-canonical text. */
static void test_base64(void)
{
    CuArena a;
    CuBuilder b;
    CuStr text;
    CuStr back;
    char all[256];
    int i;

    base64_is("", "");
    base64_is("f", "Zg==");
    base64_is("fo", "Zm8=");
    base64_is("foo", "Zm9v");
    base64_is("foob", "Zm9vYg==");
    base64_is("fooba", "Zm9vYmE=");
    base64_is("foobar", "Zm9vYmFy");

    cu_arena_init(&a);
    for (i = 0; i < 256; i++)
        all[i] = (char)i;
    cu_sb_init(&b, &a);
    cu_sb_base64(&b, cu_str_n(all, 256));
    assert(cu_sb_finish(&b, &text));
    assert(cu_base64_decode(&a, text, &back) && is(back, all, 256));
    /* 256 bytes need both letters past the digits: '+' and '/'. */
    assert(memchr(text.ptr, '+', text.len) != NULL && memchr(text.ptr, '/', text.len) != NULL);
    cu_arena_release(&a);

    base64_rejects("Zg=");
    base64_rejects("Zh==");
    base64_rejects("Zm9=");
    base64_rejects("Zg==Zg==");
    base64_rejects("Z===");
    base64_rejects("====");
    base64_rejects("Zm-v");
    base64_rejects("Zm_v");
    base64_rejects("Zm9v\n");
    base64_rejects("Z g=");
}

int main(void)
{
    test_depth();
    test_writer();
    test_writer_keys();
    test_u64_round_trip();
    test_base64();
    cu_scratch_release();
    return 0;
}
