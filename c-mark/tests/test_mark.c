/* Targeted tests for c-mark beyond the corpus: outputs checked against the
 * Go oracle for inputs the corpus lacks (CRLF, NUL, invalid UTF-8, option
 * combinations, goldmark quirks), the bounds (CU_MARK_LIMIT), allocation
 * failure (every arena push failing in turn must end as CU_MARK_NOMEM),
 * the code hook contract, and behaviour this port defines where goldmark
 * crashes. */
#include "cu_mark.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Case {
    unsigned long flags;
    const char *in;
    size_t in_len; /* 0: strlen; inputs with NUL give it */
    const char *out;
} Case;

/* Rendered by the Go oracle (c-mark/oracle, goldmark v1.6.0). */
static const Case oracle_cases[] = {
    /* CRLF and NUL: lines end at \n, \r is a space, NUL renders U+FFFD */
    {0, "a\r\nb\0c\r\n", 8, "<p>a\nb\357\277\275c</p>\n"},
    /* invalid UTF-8 passes through untouched (checked with the oracle's raw
     * stdin mode: its -cases mode goes through JSON, which rewrites it) */
    {CU_MARK_UNSAFE, "\377\376*a*\377 \342\202 <\303\251>\n", 0, "<p>\377\376<em>a</em>\377 \342\202 &lt;\303\251&gt;</p>\n"},
    {0, "# T\303 x\n", 0, "<h1>T\303 x</h1>\n"},
    /* a truncated sequence in a URL: its lead byte is escaped, the rest is raw */
    {0, "[a](\342\202) ![\377](x) <\342\202@b.c> `\377`\n", 0,
     "<p><a href=\"%E2\202\">a</a> <img src=\"x\" alt=\"\377\"> &lt;\342\202@b.c&gt; <code>\377</code></p>\n"},
    /* auto ids: non-ASCII contributes nothing, so the id falls back to "heading" */
    {CU_MARK_AUTO_HEADING_ID, "# \303\251\n\n# \303\251\n", 0,
     "<h1 id=\"heading\">\303\251</h1>\n<h1 id=\"heading-1\">\303\251</h1>\n"},
    /* numeric references in URLs go through ParseUint base 0 (octal), in text through base 10 */
    {0, "[a](<&#010;>) [b](&#x110000;) &amp; &#65; &#x26; &#0; \\&\n", 0,
     "<p><a href=\"%08\">a</a> <a href=\"%EF%BF%BD\">b</a> &amp; A &amp; \357\277\275 &amp;</p>\n"},
    {CU_MARK_HARD_WRAPS, "a\nb  \nc\\\nd\n", 0, "<p>a<br>\nb<br>\nc<br>\nd</p>\n"},
    {CU_MARK_HARD_WRAPS | CU_MARK_XHTML, "a\nb\n\n***\n\n![i](u)\n", 0,
     "<p>a<br />\nb</p>\n<hr />\n<p><img src=\"u\" alt=\"i\" /></p>\n"},
    /* link_ref.go keeps a title that is followed by text on its line */
    {0, "[foo]: /url\n\"title\" ok\n\n[foo]\n", 0,
     "<p>&quot;title&quot; ok</p>\n<p><a href=\"/url\" title=\"title\">foo</a></p>\n"},
    /* a backslash ending a line carries its escape state into the next line */
    {0, "a\\\\\\\n*b*\n", 0, "<p>a\\\\\n<em>b</em></p>\n"},
    {0, "<a.b@c-d.e> <a@b-> <x@y.z-> <http://a b> <a:b>\n", 0,
     "<p><a href=\"mailto:a.b@c-d.e\">a.b@c-d.e</a> &lt;a@b-&gt; &lt;x@y.z-&gt; &lt;http://a b&gt; &lt;a:b&gt;</p>\n"},
    /* raw HTML across lines; a comment with -- inside is not a comment */
    {CU_MARK_UNSAFE, "x <a\nhref=\"y\">z</a> <!-- c\n-- > --> <? p\n?>\n", 0,
     "<p>x <a\nhref=\"y\">z</a> &lt;!-- c\n-- &gt; --&gt; <? p\n?></p>\n"},
    {CU_MARK_ATTRIBUTE | CU_MARK_AUTO_HEADING_ID, "Title {#id .c}\n===\n\nT2 {.d}\n---\n\nT2\n---\n", 0,
     "<h1 id=\"id\" class=\"c\">Title</h1>\n<h2 class=\"d\" id=\"t2\">T2</h2>\n<h2 id=\"t2-1\">T2</h2>\n"},
    /* a tab split by a list indent becomes padding spaces */
    {0, "- ```\n\tfoo\n  ```\n", 0, "<ul>\n<li>\n<pre><code>  foo\n</code></pre>\n</li>\n</ul>\n"},
    {0, "```go info here\nline1\n  line2\n```\n", 0, "<pre><code class=\"language-go\">line1\n  line2\n</code></pre>\n"},
    {0, "1. a\n\n   b\n2. c\n   - d\n\n     e\n", 0,
     "<ol>\n<li>\n<p>a</p>\n<p>b</p>\n</li>\n<li>\n<p>c</p>\n<ul>\n<li>\n<p>d</p>\n<p>e</p>\n</li>\n</ul>\n</li>\n</ol>\n"},
    {0, "> \tcode\n", 0, "<blockquote>\n<p>code</p>\n</blockquote>\n"},
    {0, "  foo\tbar  \n", 0, "<p>foo\tbar</p>\n"},
    {0, "*foo\314\201*  _a_b _c_ **d**e\n", 0, "<p><em>foo\314\201</em>  _a_b <em>c</em> <strong>d</strong>e</p>\n"},
    {0, "[x]: /u \"t\n\n[x] [X] [ x ]\n", 0, "<p>[x]: /u &quot;t</p>\n<p>[x] [X] [ x ]</p>\n"},
    /* a real U+FFFD in the source reads as utf8.RuneError and ends a regexp's input */
    {0, "\357\277\275 <a\357\277\275>\n", 0, "<p>\357\277\275 &lt;a\357\277\275&gt;</p>\n"},
};

static CuStr case_input(const Case *c)
{
    return cu_str_n(c->in, c->in_len != 0 ? c->in_len : strlen(c->in));
}

static CuMarkOptions options(unsigned long flags)
{
    CuMarkOptions o;

    o.flags = flags;
    o.table_align = CU_MARK_ALIGN_DEFAULT;
    o.code_hook = NULL;
    o.code_ctx = NULL;
    return o;
}

/* Renders src into a fresh builder on out_arena and returns the status;
 * *got holds the output on success. */
static CuMarkStatus render(CuArena *scratch, CuArena *out_arena, CuStr src, const CuMarkOptions *opt, CuStr *got)
{
    CuBuilder b;
    CuMarkStatus st;

    cu_sb_init(&b, out_arena);
    st = cu_mark_render(scratch, src, opt, &b);
    if (!cu_sb_finish(&b, got)) *got = cu_str_empty();
    return st;
}

static void expect(CuArena *scratch, CuArena *out_arena, unsigned long flags, CuStr src, const char *want)
{
    CuMarkOptions opt = options(flags);
    CuTemp t = cu_temp_begin(out_arena);
    CuStr got;
    CuMarkStatus st = render(scratch, out_arena, src, &opt, &got);

    if (st != CU_MARK_OK || !cu_str_eq(got, cu_str_cstr(want))) {
        fprintf(stderr, "status %d\ninput:\n%.*s\nwant:\n%s\ngot:\n%.*s\n", (int)st, (int)src.len, src.ptr, want,
                (int)got.len, got.ptr);
        abort();
    }
    cu_temp_end(t);
}

static void test_oracle_cases(CuArena *scratch, CuArena *out_arena)
{
    size_t i;

    for (i = 0; i < sizeof oracle_cases / sizeof oracle_cases[0]; i++) {
        expect(scratch, out_arena, oracle_cases[i].flags, case_input(&oracle_cases[i]), oracle_cases[i].out);
    }
}

/* NULL options render as goldmark.New() with no options. */
static void test_null_options(CuArena *scratch, CuArena *out_arena)
{
    CuTemp t = cu_temp_begin(out_arena);
    CuBuilder b;
    CuStr got;

    cu_sb_init(&b, out_arena);
    assert(cu_mark_render(scratch, CU_LIT("<b>x</b> ---\n"), NULL, &b) == CU_MARK_OK);
    assert(cu_sb_finish(&b, &got));
    assert(cu_str_eq(got, CU_LIT("<p><!-- raw HTML omitted -->x<!-- raw HTML omitted --> ---</p>\n")));
    cu_temp_end(t);
}

/* goldmark panics rendering an attribute whose value is not a string; this
 * port leaves the attribute out. */
static void test_non_string_attribute_omitted(CuArena *scratch, CuArena *out_arena)
{
    expect(scratch, out_arena, CU_MARK_ATTRIBUTE, CU_LIT("# a {data-x=1 .c data-y=true data-z=[1,\"s\"] data-n=null}\n"),
           "<h1 class=\"c\">a</h1>\n");
    /* a nested map only parses after closing hashes (parseLastLineAttributes
     * takes the last '{' of the line, oracle-checked below) */
    expect(scratch, out_arena, CU_MARK_ATTRIBUTE, CU_LIT("# a # {data-w={k=v} .c}\n"), "<h1 class=\"c\">a</h1>\n");
    expect(scratch, out_arena, CU_MARK_ATTRIBUTE, CU_LIT("# a {.c data-w={k=v}}\n"),
           "<h1>a {.c data-w={k=v}}</h1>\n");
}

static CuStr repeat_then(CuArena *a, const char *unit, size_t n, const char *tail)
{
    CuBuilder b;
    CuStr s;
    size_t i;

    cu_sb_init(&b, a);
    for (i = 0; i < n; i++) cu_sb_str(&b, cu_str_cstr(unit));
    cu_sb_str(&b, cu_str_cstr(tail));
    assert(cu_sb_finish(&b, &s));
    return s;
}

static void test_limit_input_size(CuArena *scratch, CuArena *out_arena)
{
    CuTemp t = cu_temp_begin(out_arena);
    CuMarkOptions opt = options(0);
    char *big = cu_arena_push_bytes(out_arena, CU_MARK_MAX_INPUT + 1);
    CuStr got;
    CuStr want;

    assert(big != NULL);
    memset(big, 'a', CU_MARK_MAX_INPUT + 1);
    assert(render(scratch, out_arena, cu_str_n(big, CU_MARK_MAX_INPUT + 1), &opt, &got) == CU_MARK_LIMIT);
    assert(got.len == 0);
    /* exactly the bound renders */
    assert(render(scratch, out_arena, cu_str_n(big, CU_MARK_MAX_INPUT), &opt, &got) == CU_MARK_OK);
    want = repeat_then(out_arena, "a", CU_MARK_MAX_INPUT, "</p>\n");
    assert(got.len == want.len + 3);
    assert(memcmp(got.ptr, "<p>", 3) == 0);
    assert(memcmp(got.ptr + 3, want.ptr, want.len) == 0);
    cu_temp_end(t);
}

static void test_limit_depth(CuArena *scratch, CuArena *out_arena)
{
    CuTemp t = cu_temp_begin(out_arena);
    CuMarkOptions opt = options(CU_MARK_ATTRIBUTE);
    CuStr src;
    CuStr got;
    CuStr want;
    CuBuilder b;

    /* CU_MARK_MAX_DEPTH nested blockquotes render, one more is refused */
    src = repeat_then(out_arena, "> ", CU_MARK_MAX_DEPTH, "a\n");
    assert(render(scratch, out_arena, src, &opt, &got) == CU_MARK_OK);
    cu_sb_init(&b, out_arena);
    cu_sb_str(&b, repeat_then(out_arena, "<blockquote>\n", CU_MARK_MAX_DEPTH, "<p>a</p>\n"));
    cu_sb_str(&b, repeat_then(out_arena, "</blockquote>\n", CU_MARK_MAX_DEPTH, ""));
    assert(cu_sb_finish(&b, &want));
    assert(cu_str_eq(got, want));
    src = repeat_then(out_arena, "> ", CU_MARK_MAX_DEPTH + 1, "a\n");
    assert(render(scratch, out_arena, src, &opt, &got) == CU_MARK_LIMIT);
    assert(got.len == 0);

    /* lists count a list and an item per level */
    src = repeat_then(out_arena, "- ", CU_MARK_MAX_DEPTH / 2, "a\n");
    assert(render(scratch, out_arena, src, &opt, &got) == CU_MARK_OK);
    src = repeat_then(out_arena, "- ", CU_MARK_MAX_DEPTH / 2 + 1, "a\n");
    assert(render(scratch, out_arena, src, &opt, &got) == CU_MARK_LIMIT);

    /* attribute values nest with the same bound */
    cu_sb_init(&b, out_arena);
    cu_sb_str(&b, CU_LIT("# h {.c a="));
    cu_sb_str(&b, repeat_then(out_arena, "[", CU_MARK_MAX_DEPTH, "1"));
    cu_sb_str(&b, repeat_then(out_arena, "]", CU_MARK_MAX_DEPTH, "}\n"));
    assert(cu_sb_finish(&b, &src));
    assert(render(scratch, out_arena, src, &opt, &got) == CU_MARK_OK);
    assert(cu_str_eq(got, CU_LIT("<h1 class=\"c\">h</h1>\n")));
    cu_sb_init(&b, out_arena);
    cu_sb_str(&b, CU_LIT("# h {.c a="));
    cu_sb_str(&b, repeat_then(out_arena, "[", CU_MARK_MAX_DEPTH + 1, "1"));
    cu_sb_str(&b, repeat_then(out_arena, "]", CU_MARK_MAX_DEPTH + 1, "}\n"));
    assert(cu_sb_finish(&b, &src));
    assert(render(scratch, out_arena, src, &opt, &got) == CU_MARK_LIMIT);
    cu_temp_end(t);
}

/* A document that exercises every parser, for the allocation tests. */
static const char nomem_doc[] =
    "# Heading {#id .c}\n\nSetext *em* **strong** `code` [l](/u \"t\") ![i](/p) <a@b.c> <http://x>\n"
    "===\n\n> quote\n> - item\n>   continued\n> 1. one\n\n    indented\n\n```go\nfenced\n```\n\n"
    "<div>\nhtml\n</div>\n\n[ref]: /url 'title'\n\n[ref] [ref][] [text][ref] ***\n";

/* The oracle's output for nomem_doc, in pieces under C90's literal limit. */
static const char *const nomem_want_parts[] = {
    "<h1 id=\"id\" class=\"c\">Heading</h1>\n",
    "<h1 id=\"setext-em-strong-code-lu-t-ip-abc-httpx\">Setext <em>em</em> <strong>strong</strong> ",
    "<code>code</code> <a href=\"/u\" title=\"t\">l</a> <img src=\"/p\" alt=\"i\"> ",
    "<a href=\"mailto:a@b.c\">a@b.c</a> <a href=\"http://x\">http://x</a></h1>\n",
    "<blockquote>\n<p>quote</p>\n<ul>\n<li>item\ncontinued</li>\n</ul>\n<ol>\n<li>one</li>\n</ol>\n</blockquote>\n",
    "<pre><code>indented\n</code></pre>\n",
    "<pre><code class=\"language-go\">fenced\n</code></pre>\n",
    "<div>\nhtml\n</div>\n",
    "<p><a href=\"/url\" title=\"title\">ref</a> <a href=\"/url\" title=\"title\">ref</a> ",
    "<a href=\"/url\" title=\"title\">text</a> ***</p>\n",
};

static CuStr nomem_want(CuArena *a)
{
    CuBuilder b;
    CuStr s;
    size_t i;

    cu_sb_init(&b, a);
    for (i = 0; i < sizeof nomem_want_parts / sizeof nomem_want_parts[0]; i++) {
        cu_sb_str(&b, cu_str_cstr(nomem_want_parts[i]));
    }
    assert(cu_sb_finish(&b, &s));
    return s;
}

/* Each scratch allocation fails in turn (debug arenas make every push an
 * attempt): the render must report CU_MARK_NOMEM or, once the failing
 * attempt lies past the run, render exactly. */
static void test_nomem_scratch(CuArena *out_arena)
{
    CuMarkOptions opt = options(CU_MARK_ATTRIBUTE | CU_MARK_AUTO_HEADING_ID | CU_MARK_UNSAFE);
    CuArena scratch;
    size_t n;
    size_t failures = 0;
    size_t successes = 0;

    CuTemp outer = cu_temp_begin(out_arena);
    CuStr want = nomem_want(out_arena);

    cu_arena_init_with(&scratch, 4096, 1);
    for (n = 0;; n++) {
        CuTemp t = cu_temp_begin(out_arena);
        CuStr got;
        CuMarkStatus st;
        size_t attempts;

        cu_arena_release(&scratch);
        cu_arena_init_with(&scratch, 4096, 1);
        cu_arena_fail_at(&scratch, n);
        st = render(&scratch, out_arena, cu_str_cstr(nomem_doc), &opt, &got);
        attempts = scratch.attempts;
        if (st == CU_MARK_OK) {
            assert(cu_str_eq(got, want));
            successes++;
        } else {
            assert(st == CU_MARK_NOMEM);
            failures++;
        }
        cu_temp_end(t);
        if (attempts <= n) break;
    }
    cu_arena_release(&scratch);
    cu_temp_end(outer);
    assert(failures > 10 && successes >= 1);
}

/* The same with the output builder's arena failing: the builder's sticky
 * failure must become CU_MARK_NOMEM. */
static void test_nomem_output(CuArena *scratch)
{
    CuMarkOptions opt = options(0);
    CuArena out_arena;
    size_t n;
    size_t failures = 0;

    cu_arena_init_with(&out_arena, 4096, 1);
    for (n = 0;; n++) {
        CuStr got;
        CuMarkStatus st;
        size_t attempts;

        cu_arena_release(&out_arena);
        cu_arena_init_with(&out_arena, 4096, 1);
        cu_arena_fail_at(&out_arena, n);
        st = render(scratch, &out_arena, cu_str_cstr(nomem_doc), &opt, &got);
        attempts = out_arena.attempts;
        if (st == CU_MARK_NOMEM) failures++;
        else assert(st == CU_MARK_OK && got.len != 0);
        if (attempts <= n) break;
    }
    cu_arena_release(&out_arena);
    assert(failures >= 1);
}

typedef struct HookLog {
    int calls;
    int handle;
    CuStr info;
    CuStr lang;
    CuStr text;
    size_t attr_count;
    CuArena *arena;
} HookLog;

static int hook(void *ctx, const CuMarkCode *code, CuBuilder *out)
{
    HookLog *log = ctx;

    log->calls++;
    log->info = cu_str_copy(log->arena, code->info);
    log->lang = cu_str_copy(log->arena, code->lang);
    log->text = cu_str_copy(log->arena, code->text);
    log->attr_count = code->attr_count;
    if (!log->handle) return 0;
    cu_sb_str(out, CU_LIT("<hooked>"));
    return 1;
}

static void test_code_hook(CuArena *scratch, CuArena *out_arena)
{
    CuTemp t = cu_temp_begin(out_arena);
    CuMarkOptions opt = options(0);
    HookLog log;
    CuStr got;

    log.calls = 0;
    log.handle = 1;
    log.attr_count = 99;
    log.arena = out_arena;
    opt.code_hook = hook;
    opt.code_ctx = &log;

    /* info, language, the lines as stored (a split tab is padding spaces) */
    assert(render(scratch, out_arena, CU_LIT("- ```go info {x}\n\tfoo\n  bar\n  ```\n\n    indented\n"), &opt, &got) ==
           CU_MARK_OK);
    assert(log.calls == 1); /* the indented text is a paragraph of the item, not code */
    assert(cu_str_eq(log.info, CU_LIT("go info {x}")));
    assert(cu_str_eq(log.lang, CU_LIT("go")));
    assert(cu_str_eq(log.text, CU_LIT("  foo\nbar\n")));
    assert(log.attr_count == 0);
    assert(cu_str_eq(got, CU_LIT("<ul>\n<li>\n<hooked><p>indented</p>\n</li>\n</ul>\n")));

    /* no info string: empty info and language */
    assert(render(scratch, out_arena, CU_LIT("~~~\n\n~~~\n"), &opt, &got) == CU_MARK_OK);
    assert(log.calls == 2);
    assert(log.info.len == 0 && log.lang.len == 0);
    assert(cu_str_eq(log.text, CU_LIT("\n")));
    assert(cu_str_eq(got, CU_LIT("<hooked>")));

    /* returning 0 falls back to goldmark's rendering */
    log.handle = 0;
    assert(render(scratch, out_arena, CU_LIT("```c\nx\n```\n"), &opt, &got) == CU_MARK_OK);
    assert(log.calls == 3);
    assert(cu_str_eq(got, CU_LIT("<pre><code class=\"language-c\">x\n</code></pre>\n")));

    /* a failed builder inside the hook fails the render */
    {
        CuArena tiny;
        CuBuilder b;

        cu_arena_init_with(&tiny, 4096, 1);
        cu_arena_set_limit(&tiny, 0);
        log.handle = 1;
        cu_sb_init(&b, &tiny);
        assert(cu_mark_render(scratch, CU_LIT("```\nx\n```\n"), &opt, &b) == CU_MARK_NOMEM);
        cu_arena_release(&tiny);
    }
    cu_temp_end(t);
}

int main(void)
{
    CuArena scratch;
    CuArena out_arena;

    cu_arena_init(&scratch);
    cu_arena_init(&out_arena);
    test_oracle_cases(&scratch, &out_arena);
    test_null_options(&scratch, &out_arena);
    test_non_string_attribute_omitted(&scratch, &out_arena);
    test_limit_input_size(&scratch, &out_arena);
    test_limit_depth(&scratch, &out_arena);
    test_nomem_scratch(&out_arena);
    test_nomem_output(&scratch);
    test_code_hook(&scratch, &out_arena);
    cu_arena_release(&out_arena);
    cu_arena_release(&scratch);
    printf("PASS test_mark\n");
    return 0;
}
