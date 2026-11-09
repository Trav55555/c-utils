/* Targeted tests for c-minify: the status contract (limits, allocation
 * failure, the UNSUPPORTED gate on every path that reaches an unported
 * minifier, INVALID), the media-type matcher, and edge cases whose
 * expected bytes are the oracle's (c-minify/oracle, minify v2.20.5 under
 * the site's configuration unless a flag set says otherwise). The corpus
 * runner covers minify's own test tables and the live pages. */
#include "cu_minify.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static CuMinifyOptions options(unsigned long flags)
{
    CuMinifyOptions o;

    o.flags = flags;
    return o;
}

/* Minifies src into a fresh builder on a; *out holds the bytes on success. */
static CuMinifyStatus minify(CuArena *scratch, CuArena *a, CuMinifyType type, const CuMinifyOptions *opt, CuStr src,
                             CuStr *out)
{
    CuBuilder b;
    CuMinifyStatus st;

    cu_sb_init(&b, a);
    st = cu_minify(scratch, type, opt, src, &b);
    if (!cu_sb_finish(&b, out)) *out = cu_str_empty();
    return st;
}

/* Asserts the site configuration's output for src. */
static void expect(CuArena *scratch, CuArena *a, CuMinifyType type, unsigned long flags, CuStr src, CuStr want)
{
    CuTemp t = cu_temp_begin(a);
    CuMinifyOptions opt = options(flags);
    CuStr got;
    CuMinifyStatus st = minify(scratch, a, type, &opt, src, &got);

    if (st != CU_MINIFY_OK || !cu_str_eq(got, want)) {
        fprintf(stderr, "status %d\ninput: %.*s\nwant:  %.*s\ngot:   %.*s\n", (int)st, (int)src.len, src.ptr,
                (int)want.len, want.ptr, (int)got.len, got.ptr);
        assert(0);
    }
    cu_temp_end(t);
}

static void expect_status(CuArena *scratch, CuArena *a, CuMinifyType type, unsigned long flags, CuStr src,
                          CuMinifyStatus want)
{
    CuTemp t = cu_temp_begin(a);
    CuMinifyOptions opt = options(flags);
    CuStr got;
    CuMinifyStatus st = minify(scratch, a, type, &opt, src, &got);

    if (st != want) {
        fprintf(stderr, "status %d, want %d\ninput: %.*s\n", (int)st, (int)want, (int)src.len, src.ptr);
        assert(0);
    }
    cu_temp_end(t);
}

#define HTML(flags, in, out) expect(scratch, a, CU_MINIFY_HTML, flags, CU_LIT(in), CU_LIT(out))
#define XML(flags, in, out) expect(scratch, a, CU_MINIFY_XML, flags, CU_LIT(in), CU_LIT(out))
#define HTML_STATUS(in, st) expect_status(scratch, a, CU_MINIFY_HTML, CU_MINIFY_SITE, CU_LIT(in), st)
#define XML_STATUS(in, st) expect_status(scratch, a, CU_MINIFY_XML, CU_MINIFY_SITE, CU_LIT(in), st)

/* NULL options are the site's configuration. */
static void test_default_options(CuArena *scratch, CuArena *a)
{
    CuTemp t = cu_temp_begin(a);
    CuStr got;

    assert(minify(scratch, a, CU_MINIFY_HTML, NULL, CU_LIT("<!-- c --><p>x</p>"), &got) == CU_MINIFY_OK);
    assert(cu_str_eq(got, CU_LIT("<!-- c --><p>x</p>")));
    HTML(0, "<!-- c --><p>x</p>", "<p>x");
    cu_temp_end(t);
}

/* Each html.go path that calls into another minifier, and what the
 * oracle does there. */
static void test_unsupported_gate(CuArena *scratch, CuArena *a)
{
    /* <style> text, style attributes: the CSS minifier */
    HTML_STATUS("<style>a{}</style>", CU_MINIFY_UNSUPPORTED);
    HTML_STATUS("<p style=\"color:red\">x", CU_MINIFY_UNSUPPORTED);
    HTML_STATUS("<a href=\"data:text/css,a{}\">", CU_MINIFY_UNSUPPORTED);
    /* <script> text, on* attributes: the JS minifier */
    HTML_STATUS("<script>x</script>", CU_MINIFY_UNSUPPORTED);
    HTML_STATUS("<p onclick=\"f()\">x", CU_MINIFY_UNSUPPORTED);
    HTML_STATUS("<a href=\"data:text/javascript,x\">", CU_MINIFY_UNSUPPORTED);
    /* JSON script types and data URLs */
    HTML_STATUS("<script type=application/ld+json>{}</script>", CU_MINIFY_UNSUPPORTED);
    HTML_STATUS("<a href=\"data:application/json,{}\">", CU_MINIFY_UNSUPPORTED);
    /* inline svg and svg data URLs */
    HTML_STATUS("<svg><g/></svg>", CU_MINIFY_UNSUPPORTED);
    HTML_STATUS("<a href=\"data:image/svg+xml,<svg/>\">", CU_MINIFY_UNSUPPORTED);
    /* unsupported content inside nested HTML */
    HTML_STATUS("<iframe><style>a{}</style></iframe>", CU_MINIFY_UNSUPPORTED);
    /* nothing is relied on in out, but the gate is reached even deep in a page */
    HTML_STATUS("<p>a</p><div><span onmouseover=\" x \">b</span></div>", CU_MINIFY_UNSUPPORTED);

    /* Empty and all-whitespace CSS and JS write nothing, so these ports are exact. */
    HTML(CU_MINIFY_SITE, "<p style=\"\">x", "<p>x");
    HTML(CU_MINIFY_SITE, "<p style=\"  \">x", "<p>x");
    HTML(CU_MINIFY_SITE, "<p onclick=\"\">x", "<p>x");
    HTML(CU_MINIFY_SITE, "<p onclick=\" javascript: \">x", "<p>x");
    HTML(CU_MINIFY_SITE, "<script> </script>x", "<script></script>x");
    HTML(CU_MINIFY_SITE, "<script>\n\n</script>x", "<script></script>x");
    HTML(CU_MINIFY_SITE, "<style> </style>x", "<style></style>x");
    /* math has no minifier in Hugo's set: passed through */
    HTML(CU_MINIFY_SITE, "<math><mi>x</mi></math>", "<math><mi>x</mi></math>");
    /* XML and HTML script types and data URLs are minified by the port */
    HTML(CU_MINIFY_SITE, "<script type=\"text/xml\"> <a> </a> </script>", "<script type=text/xml><a/></script>");
    HTML(CU_MINIFY_SITE, "<a href=\"data:text/xml, <a> </a> \">", "<a href=data:text/xml,%3Ca/%3E>");
    HTML(CU_MINIFY_SITE, "<a href=\"data:text/html, <b> x </b>\">", "<a href=data:text/html,%3Cb%3Ex%3C/b%3E>");
    HTML(CU_MINIFY_SITE, "<a href=\"data:text/html,<iframe>%3Cb%3E x %3C/b%3E\">",
         "<a href=data:text/html,%3Ciframe%3E%3Cb%3Ex%3C/b%3E>");
    /* a type the set does not know keeps its text */
    HTML(CU_MINIFY_SITE, "<script type=\"text/template\"> <a> </a> </script>",
         "<script type=text/template> <a> </a> </script>");
    /* the type is matched before minify.Mediatype lowercases it */
    HTML(CU_MINIFY_SITE, "<script type=\"TEXT/CSS\">a {  }</script>", "<script type=text/css>a {  }</script>");
}

/* A NUL byte where minify's XML lexer fails: the XML minifier, inline svg
 * and math in HTML, and XML reached through a script type or an iframe.
 * Inside a data URL the error is swallowed and the data kept, as
 * minify.DataURI ignores m.Bytes' error. */
static void test_invalid(CuArena *scratch, CuArena *a)
{
    XML_STATUS("a\0b", CU_MINIFY_INVALID);
    XML_STATUS("<a \0b='5'>", CU_MINIFY_INVALID);
    XML_STATUS("<![CDATA[\0", CU_MINIFY_INVALID);
    HTML_STATUS("<svg>\0</svg>", CU_MINIFY_INVALID);
    HTML_STATUS("<math>\0</math>", CU_MINIFY_INVALID);
    HTML_STATUS("<svg></svg\0>", CU_MINIFY_INVALID);
    HTML_STATUS("<script type=\"text/xml\"><a>\0</a></script>", CU_MINIFY_INVALID);
    HTML_STATUS("<iframe>a<svg>\0</svg></iframe>", CU_MINIFY_INVALID);
    HTML(CU_MINIFY_SITE, "<a href=\"data:text/xml,<a>%00</a>\">", "<a href=data:text/xml,%3Ca%3E%00%3C/a%3E>");
    /* elsewhere in HTML a NUL is an ordinary byte */
    HTML(CU_MINIFY_SITE, "a\0b<p \0x=\"\0\">\0</p>", "a\0b<p \0x=\0>\0</p>");
}

static void test_type_for(void)
{
    CuMinifyType t = (CuMinifyType)0;

    assert(cu_minify_type_for(CU_LIT("text/html"), &t) && t == CU_MINIFY_HTML);
    assert(cu_minify_type_for(CU_LIT("text/html; charset=utf-8"), &t) && t == CU_MINIFY_HTML);
    assert(cu_minify_type_for(CU_LIT("  application/xml"), &t) && t == CU_MINIFY_XML);
    assert(cu_minify_type_for(CU_LIT("application/rss+xml"), &t) && t == CU_MINIFY_XML);
    assert(cu_minify_type_for(CU_LIT("text/xml;x=y"), &t) && t == CU_MINIFY_XML);
    assert(!cu_minify_type_for(CU_LIT("Text/HTML"), &t));
    assert(!cu_minify_type_for(CU_LIT("text/css"), &t));
    assert(!cu_minify_type_for(CU_LIT("application/ld+json"), &t));
    assert(!cu_minify_type_for(CU_LIT("text/javascript"), &t));
    assert(!cu_minify_type_for(CU_LIT("image/svg+xml"), &t));
    assert(!cu_minify_type_for(CU_LIT("text/plain"), &t));
    assert(!cu_minify_type_for(CU_LIT(""), &t));
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

static void test_limit_input_size(CuArena *scratch, CuArena *a)
{
    CuTemp t = cu_temp_begin(a);
    char *big = cu_arena_push_bytes(a, CU_MINIFY_MAX_INPUT + 1);
    CuStr got;

    assert(big != NULL);
    memset(big, 'a', CU_MINIFY_MAX_INPUT + 1);
    assert(minify(scratch, a, CU_MINIFY_HTML, NULL, cu_str_n(big, CU_MINIFY_MAX_INPUT + 1), &got) == CU_MINIFY_LIMIT);
    assert(got.len == 0);
    assert(minify(scratch, a, CU_MINIFY_XML, NULL, cu_str_n(big, CU_MINIFY_MAX_INPUT + 1), &got) == CU_MINIFY_LIMIT);
    /* exactly the bound minifies */
    assert(minify(scratch, a, CU_MINIFY_HTML, NULL, cu_str_n(big, CU_MINIFY_MAX_INPUT), &got) == CU_MINIFY_OK);
    assert(got.len == CU_MINIFY_MAX_INPUT && memcmp(got.ptr, big, got.len) == 0);
    cu_temp_end(t);
}

/* Each <iframe> re-enters the HTML minifier on the rest of the input: the
 * bound counts those calls. The oracle returns such a document unchanged. */
static void test_limit_depth(CuArena *scratch, CuArena *a)
{
    CuTemp t = cu_temp_begin(a);
    CuStr ok = repeat_then(a, "<iframe>", CU_MINIFY_MAX_DEPTH, "x");
    CuStr over = repeat_then(a, "<iframe>", CU_MINIFY_MAX_DEPTH + 1, "x");
    CuStr got;

    assert(minify(scratch, a, CU_MINIFY_HTML, NULL, ok, &got) == CU_MINIFY_OK);
    assert(cu_str_eq(got, ok));
    assert(minify(scratch, a, CU_MINIFY_HTML, NULL, over, &got) == CU_MINIFY_LIMIT);
    /* the same through data URLs that hold HTML */
    HTML(CU_MINIFY_SITE, "<a href=\"data:text/html,<a href=%22data:text/html,%3Cb%3E x %3C/b%3E%22>\">",
         "<a href=\"data:text/html,%3Ca%20href=data:text/html,%253Cb%253Ex%253C/b%253E%3E\">");
    cu_temp_end(t);
}

/* A document that walks every allocating path: attributes with entities
 * and quotes, a data URL in both encodings, a viewport, a keywords meta,
 * nested HTML through an iframe, XML through a script type, text with
 * entities and whitespace. */
static const char *const nomem_parts[] = {
    "<!DOCTYPE html><html lang=en><head><meta name=viewport content=\"width=0.50, initial-scale=1.0e0\">"
    "<meta name=keywords content=\"a, b\"><meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\">"
    "<title> t  t </title></head><body><p class=\" a  b \" title='x\"y' data-x=\"a&quot;b&#39;c&lt;d\">",
    " x &amp; y &nbsp; z <a href=\"data:text/plain,hello world\">d</a><a href=\"data:text/plain;base64,aGVsbG8=\">e</a>"
    "<a href=\"data:,aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C\">f</a>",
    "</p><iframe><p> q </p><iframe> r </iframe></iframe><script type=\"text/xml\"> <a x=\"&quot;\"> </a> </script>"
    "<ul> <li> a </li> <li> b </li> </ul><pre> p </pre><textarea> t </textarea></body></html>",
};

/* The document above as one string in a (C89 literals stop at 509 bytes). */
static CuStr nomem_doc(CuArena *a)
{
    CuBuilder b;
    CuStr s;
    size_t i;

    cu_sb_init(&b, a);
    for (i = 0; i < sizeof nomem_parts / sizeof nomem_parts[0]; i++) cu_sb_str(&b, cu_str_cstr(nomem_parts[i]));
    assert(cu_sb_finish(&b, &s));
    return s;
}

/* With the scratch arena failing at each allocation in turn, the status
 * is NOMEM or the output is right; nothing else. */
static void test_nomem_scratch(CuArena *a)
{
    CuArena scratch;
    CuTemp outer = cu_temp_begin(a);
    CuStr doc = nomem_doc(a);
    CuStr want;
    size_t n;
    size_t failures = 0;
    size_t successes = 0;

    cu_arena_init_with(&scratch, 4096, 1);
    assert(minify(&scratch, a, CU_MINIFY_HTML, NULL, doc, &want) == CU_MINIFY_OK);
    assert(want.len > 0 && want.len < doc.len);
    for (n = 0;; n++) {
        CuTemp t = cu_temp_begin(a);
        CuStr got;
        CuMinifyStatus st;
        size_t attempts;

        cu_arena_release(&scratch);
        cu_arena_init_with(&scratch, 4096, 1);
        cu_arena_fail_at(&scratch, n);
        st = minify(&scratch, a, CU_MINIFY_HTML, NULL, doc, &got);
        attempts = scratch.attempts;
        if (st == CU_MINIFY_OK) {
            assert(cu_str_eq(got, want));
            successes++;
        } else {
            assert(st == CU_MINIFY_NOMEM);
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
 * failure must become NOMEM. */
static void test_nomem_output(CuArena *scratch, CuArena *a)
{
    CuArena out_arena;
    CuTemp outer = cu_temp_begin(a);
    CuStr doc = nomem_doc(a);
    size_t n;
    size_t failures = 0;

    cu_arena_init_with(&out_arena, 4096, 1);
    for (n = 0;; n++) {
        CuStr got;
        CuMinifyStatus st;
        size_t attempts;

        cu_arena_release(&out_arena);
        cu_arena_init_with(&out_arena, 4096, 1);
        cu_arena_fail_at(&out_arena, n);
        st = minify(scratch, &out_arena, CU_MINIFY_HTML, NULL, doc, &got);
        attempts = out_arena.attempts;
        if (st == CU_MINIFY_NOMEM) failures++;
        else assert(st == CU_MINIFY_OK && got.len != 0);
        if (attempts <= n) break;
    }
    cu_arena_release(&out_arena);
    cu_temp_end(outer);
    assert(failures >= 1);
}

/* A capped scratch arena ends as NOMEM, not as anything else. */
static void test_scratch_limit(CuArena *a)
{
    CuArena scratch;
    CuTemp outer = cu_temp_begin(a);
    CuStr doc = nomem_doc(a);
    CuStr got;

    cu_arena_init_with(&scratch, 4096, 0);
    cu_arena_set_limit(&scratch, 4096);
    assert(minify(&scratch, a, CU_MINIFY_HTML, NULL, doc, &got) == CU_MINIFY_NOMEM);
    cu_arena_release(&scratch);
    cu_temp_end(outer);
}

/* Edge cases, expected bytes from the oracle. */
static void test_oracle_cases(CuArena *scratch, CuArena *a)
{
    /* data URLs: parse.DataURI and minify.DataURI */
    HTML(CU_MINIFY_SITE, "<a href=\"data:text/plain;charset=us-ascii,hello\">", "<a href=data:,hello>");
    HTML(CU_MINIFY_SITE, "<a href=\"data:image/png;base64,iVBORw0KGgo=\">", "<a href=data:image/png,%89PNG%0D%0A%1A%0A>");
    HTML(CU_MINIFY_SITE, "<a href=\"data:text/plain;base64,aGVs\nbG8=\">", "<a href=data:,hello>");
    HTML(CU_MINIFY_SITE, "<a href=\"data:text/plain;base64,aGVsbG8\">", "<a href=data:text/plain;base64,aGVsbG8>");
    HTML(CU_MINIFY_SITE, "<a href=\"data:;base64,aGVsbG8=\">", "<a href=data:,hello>");
    HTML(CU_MINIFY_SITE, "<a href=\"data:text/plain,%ff%41+b\">", "<a href=data:,%25ffA%20b>");
    HTML(CU_MINIFY_SITE, "<a href=\"DATA:,hello\">", "<a href=DATA:,hello>");
    HTML(CU_MINIFY_SITE, "<a href=\"data:text/plain;charset=US-ASCII;foo=bar,x%20y\">", "<a href=\"data:;foo=bar,x%20y\">");
    HTML(CU_MINIFY_SITE,
         "<a href=data:,aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
         "%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C>",
         "<a href=\"data:;base64,YWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFh"
         "YWFhYWFhYWFhYWFhYWFhYWFhPDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw=\">");
    /* viewport numbers: minify.Number without precision */
    HTML(CU_MINIFY_SITE,
         "<meta name=viewport content=\"width=0.50, initial-scale=1.0e0, x=.5e-10, y=12345678901234567890\">",
         "<meta name=viewport content=\"width=.5,initial-scale=1,x=5e-11,y=12345678901234567890\">");
    HTML(CU_MINIFY_SITE, "<meta name=viewport content=\"a=1e400, b=-0.0, c=+01, d=100000, e=0.0001, f=123.456e2, g=1e-5\">",
         "<meta name=viewport content=\"a=1e400,b=0,c=1,d=1e5,e=1e-4,f=12345.6,g=1e-5\">");
    HTML(CU_MINIFY_SITE, "<meta name=viewport content=\"a=1234.5678e-9, b=.000001, c=9e1, d=5., e=1e+2\">",
         "<meta name=viewport content=\"a=.12345678e-5,b=1e-6,c=90,d=5.,e=100\">");
    HTML(CU_MINIFY_SITE, "<meta name=viewport content=\"width=device-width, initial-scale=1.0\">",
         "<meta name=viewport content=\"width=device-width,initial-scale=1\">");
    /* entities, including Go's 64-bit wrap of long hex references */
    HTML(CU_MINIFY_SITE,
         "<p>&#x10000000000000000;&#xFFFFFFFFFFFFFFFF;&#x10000000000000080;&#x270F;&#x2710;&#99999;&#65;&bogus;"
         "&amp;x&amp;;&quot;&lt;&LT;",
         "<p>\0\xff&#128;&#9999;&#x2710;&#99999;A&bogus;&amp;x&;\"&lt;&lt;");
    HTML(CU_MINIFY_SITE,
         "<p a=\"&#x10000000000000000;&#xFFFFFFFFFFFFFFFF;&#x10000000000000080;&#x270F;&#x2710;&#99999;&#65;&bogus;"
         "&amp;x&amp;;&quot;&lt;&LT;\">",
         "<p a='\0\xff&#128;&#9999;&#x2710;&#99999;A&bogus;&amp;x&;\"<<'>");
    /* meta, anchors, inputs, scripts with attributes */
    HTML(CU_MINIFY_SITE,
         "<Meta HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\"><META name=Keywords content=\"a, b,  c\">"
         "<a ID=x NAME=x HREF=\"HTTP://X.y\"><input TYPE=radio value=ON><script SRC=x CHARSET=u></script>",
         "<meta charset=utf-8><meta name=Keywords content=\"a,b, c\"><a id=x href=http://X.y><input type=radio>"
         "<script src=x></script>");
    HTML(CU_MINIFY_SITE,
         "<p class=\"\" id=\"\" dir=\"\" name=\"\" lang=\"\"><form action=\"\" method=GET><td colspan=1 rowspan=1>"
         "<a type=\"TEXT/HTML; charset=\xe2\x80\x9cx\xe2\x80\x9d\">",
         "<p lang><form method=GET><td colspan=1 rowspan=1><a type=\"text/html;charset=\xe2\x80\x9cx\xe2\x80\x9d\">");
    /* the default-value removals the site configuration keeps */
    HTML(0, "<p class=\"\" id=\"\" dir=\"\" name=\"\" lang=\"\"><form action=\"\" method=GET><td colspan=1 rowspan=1>"
            "<input type=text><button type=submit><style media=all type=text/css></style>",
         "<p lang><form><td colspan=1 rowspan=1><input><button><style></style>");
    /* quoting */
    HTML(CU_MINIFY_SITE,
         "<div title='a\"b' alt=\"a'b\" x=\"a'b&quot;c\" y='a\"b\"c\\'d' vocab='x' content='y\"z'>",
         "<div title='a\"b' alt=\"a'b\" x=\"a'b&#34;c\" y='a\"b\"c\\' d' vocab=\"x\" content=\"y&#34;z\">");
    HTML(CU_MINIFY_KEEP_QUOTES | CU_MINIFY_SITE, "<p a=\"x\" b='y' c=z d=\"'\" e='\"'>",
         "<p a=\"x\" b='y' c=z d=\"'\" e='\"'>");
    /* raw text elements, whitespace around phrasing and block tags */
    HTML(CU_MINIFY_SITE, "<pre>  a  \n b </pre> <textarea> x  y </textarea><title> t  t </title><plaintext> a  b",
         "<pre>  a  \n b </pre><textarea> x  y </textarea><title> t t </title><plaintext>a b");
    HTML(CU_MINIFY_SITE,
         "<ul> <li> a </li> <li> b </li> </ul><select> <option>a</option> x <optgroup> <option>b</option> </optgroup></select>",
         "<ul><li>a</li><li>b</li></ul><select><option>a</option><optgroup><option>b</option></optgroup></select>");
    HTML(0, "<ul> <li> a </li> <li> b </li> </ul><select> <option>a</option> x <optgroup> <option>b</option> </optgroup>"
            "<optgroup><option>c</option></optgroup></select><p>x</p><p>y</p><a>z</a>",
         "<ul><li>a<li>b</ul><select><option>a<optgroup><option>b<optgroup><option>c</select><p>x<p>y</p><a>z</a>");
    HTML(CU_MINIFY_SITE, "<p>a <span> b </span> c <br> d <img> e <template> f </template> g</p>",
         "<p>a <span>b </span>c<br>d <img> e <template>f </template>g</p>");
    HTML(CU_MINIFY_KEEP_WHITESPACE, "<p>a <span> b </span> c <br> d <img> e </p>  <div> f </div>",
         "<p>a <span> b </span> c <br> d <img> e <div> f </div>");
    HTML(CU_MINIFY_SITE, "<table><colgroup><col></colgroup><tr><td>a</td></tr></table><p>x<p>y</p><div>z</div>",
         "<table><col><tr><td>a</td></tr></table><p>x<p>y</p><div>z</div>");
    HTML(CU_MINIFY_SITE, "<!DOCTYPE html><HTML><HEAD><TITLE>x</TITLE></HEAD><BODY CLASS=a>y</BODY></HTML>",
         "<!doctype html><html><head><title>x</title></head><body class=a>y</body></html>");
    HTML(0, "<!DOCTYPE html><HTML><HEAD><TITLE>x</TITLE></HEAD><BODY CLASS=a>y</BODY></HTML>",
         "<!doctype html><title>x</title><body class=a>y");
    HTML(CU_MINIFY_SITE,
         "<style amp-boilerplate>a {  }</style><style media=all type=text/css> </style>"
         "<script type=\"TEXT/JAVASCRIPT\"></script><script type=module src=/a.js></script>",
         "<style amp-boilerplate>a { }</style><style media=all type=text/css></style>"
         "<script type=text/javascript></script><script type=module src=/a.js></script>");
    /* comments of every kind, bogus markup, CDATA in HTML */
    HTML(CU_MINIFY_SITE, "x<!--#include file=\"a\" --><?php x ?><!bogus><![CDATA[ a ]]></ >",
         "x<!--#include file=\"a\" --><?php x ?><!bogus><![CDATA[ a ]]></ >");
    HTML(0, "x<!--#include file=\"a\" --><?php x ?><!bogus><![CDATA[ a ]]></ ><!-- c --!><!-- d",
         "x<!--#include file=\"a\" --><![CDATA[ a ]]>");
    HTML(0, "<!--[if IE]><p> a </p><![endif]--><!--[if IE]>x<![endif]-->", "");
    HTML(CU_MINIFY_KEEP_CONDITIONAL_COMMENTS, "<!--[if IE]><p> a </p><![endif]--><!--[if IE]>x<![endif]-->",
         "<!--[if IE]><p>a<![endif]--><!--[if IE]>x<![endif]-->");
    HTML(CU_MINIFY_KEEP_CONDITIONAL_COMMENTS, "<!--[if IE]><style> </style><![endif]-->",
         "<!--[if IE]><style></style><![endif]-->");
    expect_status(scratch, a, CU_MINIFY_HTML, CU_MINIFY_KEEP_CONDITIONAL_COMMENTS,
                  CU_LIT("<!--[if IE]><style>a{}</style><![endif]-->"), CU_MINIFY_UNSUPPORTED);
    HTML(CU_MINIFY_KEEP_CONDITIONAL_COMMENTS, "<!--[if IE 6]> <p> a </p> <![endif]-->", "<!--[if IE 6]><p>a<![endif]-->");
    HTML(CU_MINIFY_KEEP_CONDITIONAL_COMMENTS, "<!--[if IE]><![endif]-->", "<!--[if IE]><![endif]-->");
    HTML(CU_MINIFY_KEEP_CONDITIONAL_COMMENTS, "<!--[if IE]><iframe> <b> </iframe><![endif]-->",
         "<!--[if IE]><iframe><b></iframe><![endif]-->");
    /* invalid UTF-8 is bytes like any other */
    HTML(CU_MINIFY_SITE, "<a href=\"  https://E.com/ \">\xff\xfe <b>\xc3</b>", "<a href=https://E.com/>\xff\xfe <b>\xc3</b>");
    /* the lexer's early endings */
    HTML(CU_MINIFY_SITE, "<a b", "<a b>");
    HTML(CU_MINIFY_SITE, "<a b=", "<a b>");
    HTML(CU_MINIFY_SITE, "<a b='x", "<a b>"); /* the unterminated quote is stripped with its mate */
    HTML(CU_MINIFY_SITE, "</a", "</a");
    HTML(CU_MINIFY_SITE, "<!doctype", "<!doctype html>");
    HTML(CU_MINIFY_SITE, "<![CDATA[ x", "<![CDATA[ x");
    HTML_STATUS("<script><!--<script>a</script>", CU_MINIFY_UNSUPPORTED); /* the text reaches JS */

    /* XML */
    XML(0, "<a> <b/> <!-- c --> <c>x</c>  </a>", "<a><b/><c>x</c></a>");
    XML(CU_MINIFY_KEEP_WHITESPACE, "<a> <b/> <!-- c --> <c>x</c>  </a>", "<a> <b/> <c>x</c> </a>");
    XML(0,
        "<?xml version=\"1.0\"?><!DOCTYPE a [ <!ELEMENT a (b)> ]><a x='1' y=\"2\" z=\"a'b\" w=\"a&quot;b\" v='a\"b' "
        "u=\"&amp;&lt;&gt;&apos;\"><![CDATA[ a < b & c ]]><![CDATA[<<<<<<]]><![CDATA[]]><b>  </b><c/><d > x </d ></a>",
        "<?xml version=\"1.0\"?><!DOCTYPE a [ <!ELEMENT a (b)> ]><a x='1' y=\"2\" z=\"a'b\" w='a\"b' v='a\"b' "
        "u=\"&amp;&lt;>'\">a &lt; b &amp; c <![CDATA[<<<<<<]]><b/><c/><d>x</d></a>");
    XML(0, "  text  <!-- c -->  text  <a>  <![CDATA[ x ]]>  </a>  ", "text text<a> x</a>");
    XML(0, "<a b=\"x\ny\tz\r\">", "<a b=\"x y z \">");
    XML(0, "<!DOCTYPE", "<!DOCTYPE");
    XML(0, "</0", "</0");
    XML(0, "<![CDATA[", "");
    XML(0, "<a>&#38;&#x26;&#60;&#x3c;&#x3C;&#X3C;&#x;&#;</a>", "<a>&amp;&amp;&lt;&lt;&lt;&#X3C;&#x;&#;</a>");
}

int main(void)
{
    CuArena scratch;
    CuArena a;

    cu_arena_init(&scratch);
    cu_arena_init(&a);
    test_default_options(&scratch, &a);
    test_unsupported_gate(&scratch, &a);
    test_invalid(&scratch, &a);
    test_type_for();
    test_limit_input_size(&scratch, &a);
    test_limit_depth(&scratch, &a);
    test_nomem_scratch(&a);
    test_nomem_output(&scratch, &a);
    test_scratch_limit(&a);
    test_oracle_cases(&scratch, &a);
    cu_arena_release(&a);
    cu_arena_release(&scratch);
    puts("PASS c-minify targeted tests");
    return 0;
}
