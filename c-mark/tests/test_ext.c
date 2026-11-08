/* Targeted tests for c-mark's extensions beyond the corpus: outputs checked
 * against the Go oracle for inputs the corpus lacks (edge cases of each
 * extension, Hugo's heading ids with non-ASCII letters and digits,
 * duplicates and empty headings, the four table alignment methods), the
 * bounds the extensions' containers fall under (CU_MARK_LIMIT), and
 * allocation failure across every extension (every arena push failing in
 * turn must end as CU_MARK_NOMEM). The expected strings are the oracle's
 * bytes, captured by running it (c-mark/oracle, goldmark v1.6.0), not
 * typed. */
#include "cu_mark.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Case {
    unsigned long flags;
    CuMarkTableAlign align;
    const char *in;
    const char *out[3]; /* the oracle's output in pieces under C90's literal limit */
} Case;

/* Rendered by the Go oracle. CU_MARK_HUGO | CU_MARK_UNSAFE is the oracle's
 * -hugo -unsafe; the other option sets went through its -cases mode. */
static const Case oracle_cases[] = {
    /* a body row with too many cells is cut, one with too few is padded */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "| a | b |\n|:-:|--:|\n| 1 | 2 | 3 |\n| 4 |\n",
     {"<table>\n<thead>\n<tr>\n<th style=\"text-align:center\">a</th>\n<th style=\"text-align:right\">b</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td style=\"text-align:center\">1</td>\n<td style=\"text-align:right\">2</td>\n</tr>\n<tr>\n<td style=\"text-align:center\">4</td>\n<td></td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* table.go computes cell positions from the padded line: a tab-indented table in a list item reads shifted bytes */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "- \t| a |\n\t|---|\n\t| `\\|` x |\n",
     {"<ul>\n<li>\n<table>\n<thead>\n<tr>\n<th>a</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td><code>|</code> x</td>\n</tr>\n</tbody>\n</table>\n</li>\n</ul>\n", NULL, NULL}},
    /* a tab-split row at the end of the source: parseRow's positions run
     * past it, and the zero bytes Go's slice capacity yields render as
     * U+FFFD (found by tools/diff_fuzz.py; one byte past, padding 1) */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "1. a\n-|\n\th",
     {"<ol>\n<li>\n<table>\n<thead>\n<tr>\n<th>a</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>\357\277\275</td>\n</tr>\n</tbody>\n</table>\n</li>\n</ol>\n", NULL, NULL}},
    /* the same two bytes past (padding 2), after a row within the source */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "* 0\n|--|\n\\<?x \n\t{",
     {"<ul>\n<li>\n<table>\n<thead>\n<tr>\n<th>0</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>&lt;?x</td>\n</tr>\n<tr>\n<td>\357\277\275</td>\n</tr>\n</tbody>\n</table>\n</li>\n</ul>\n", NULL, NULL}},
    /* the same with cell bytes before the end: the last two are replaced, not the whole cell */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "- a\n-|\n\t{xy",
     {"<ul>\n<li>\n<table>\n<thead>\n<tr>\n<th>a</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>y\357\277\275\357\277\275</td>\n</tr>\n</tbody>\n</table>\n</li>\n</ul>\n", NULL, NULL}},
    /* escaped pipes: split out of code spans, kept as text elsewhere */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "| `a\\|b` | c |\n|---|---|\n| `x\\|y\\|z` | \\| |\n",
     {"<table>\n<thead>\n<tr>\n<th><code>a|b</code></th>\n<th>c</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td><code>x|y|z</code></td>\n<td>|</td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* a blank single column is not a delimiter row */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "a\n|\n",
     {"<p>a\n|</p>\n", NULL, NULL}},
    /* \v is a space to isTableDelim but not to the delimiter expressions' \s */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "a | b\n:-|-:\n\013\n",
     {"<table>\n<thead>\n<tr>\n<th style=\"text-align:left\">a</th>\n<th style=\"text-align:right\">b</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td style=\"text-align:left\">\013</td>\n<td></td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* the first delimiter row whose header fits wins; a misfit aborts */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "x\ny\n| a | b |\n|---|---|\n| 1 | 2 |\n",
     {"<p>x\ny</p>\n<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>1</td>\n<td>2</td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* the table extension's transformer runs when a setext bar interrupts (extra.txt 0 -: -) */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "0\n-:\n-\n",
     {"<table>\n<thead>\n<tr>\n<th style=\"text-align:right\">0</th>\n</tr>\n</thead>\n</table>\n<ul>\n<li></li>\n</ul>\n", NULL, NULL}},
    /* a definition list after a table-paragraph */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "|a|\n|-|\n: d\n",
     {"<table>\n<thead>\n<tr>\n<th>a</th>\n</tr>\n</thead>\n</table>\n<p>: d</p>\n", NULL, NULL}},
    /* delimiter row only */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "| a | b |\n|:-:|--:|\n",
     {"<table>\n<thead>\n<tr>\n<th style=\"text-align:center\">a</th>\n<th style=\"text-align:right\">b</th>\n</tr>\n</thead>\n</table>\n", NULL, NULL}},
    /* left/center/right under the default method without XHTML (style) */
    {CU_MARK_TABLE, CU_MARK_ALIGN_DEFAULT, "| a | b | c |\n|:--|:-:|--:|\n| 1 | 2 | 3 |\n",
     {"<table>\n<thead>\n<tr>\n<th style=\"text-align:left\">a</th>\n<th style=\"text-align:center\">b</th>\n<th style=\"text-align:right\">c</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td style=\"text-align:left\">1</td>\n<td style=\"text-align:center\">2</td>\n<td style=\"text-align:right\">3</td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* the same under XHTML (align) */
    {CU_MARK_TABLE | CU_MARK_XHTML, CU_MARK_ALIGN_DEFAULT, "| a | b | c |\n|:--|:-:|--:|\n| 1 | 2 | 3 |\n",
     {"<table>\n<thead>\n<tr>\n<th align=\"left\">a</th>\n<th align=\"center\">b</th>\n<th align=\"right\">c</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td align=\"left\">1</td>\n<td align=\"center\">2</td>\n<td align=\"right\">3</td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* align method attribute */
    {CU_MARK_TABLE, CU_MARK_ALIGN_ATTRIBUTE, "| a | b | c |\n|:--|:-:|--:|\n| 1 | 2 | 3 |\n",
     {"<table>\n<thead>\n<tr>\n<th align=\"left\">a</th>\n<th align=\"center\">b</th>\n<th align=\"right\">c</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td align=\"left\">1</td>\n<td align=\"center\">2</td>\n<td align=\"right\">3</td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* align method attribute under XHTML */
    {CU_MARK_TABLE | CU_MARK_XHTML, CU_MARK_ALIGN_ATTRIBUTE, "| a | b | c |\n|:--|:-:|--:|\n| 1 | 2 | 3 |\n",
     {"<table>\n<thead>\n<tr>\n<th align=\"left\">a</th>\n<th align=\"center\">b</th>\n<th align=\"right\">c</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td align=\"left\">1</td>\n<td align=\"center\">2</td>\n<td align=\"right\">3</td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* align method style */
    {CU_MARK_TABLE, CU_MARK_ALIGN_STYLE, "| a | b | c |\n|:--|:-:|--:|\n| 1 | 2 | 3 |\n",
     {"<table>\n<thead>\n<tr>\n<th style=\"text-align:left\">a</th>\n<th style=\"text-align:center\">b</th>\n<th style=\"text-align:right\">c</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td style=\"text-align:left\">1</td>\n<td style=\"text-align:center\">2</td>\n<td style=\"text-align:right\">3</td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* align method style under XHTML */
    {CU_MARK_TABLE | CU_MARK_XHTML, CU_MARK_ALIGN_STYLE, "| a | b | c |\n|:--|:-:|--:|\n| 1 | 2 | 3 |\n",
     {"<table>\n<thead>\n<tr>\n<th style=\"text-align:left\">a</th>\n<th style=\"text-align:center\">b</th>\n<th style=\"text-align:right\">c</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td style=\"text-align:left\">1</td>\n<td style=\"text-align:center\">2</td>\n<td style=\"text-align:right\">3</td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* align method none */
    {CU_MARK_TABLE, CU_MARK_ALIGN_NONE, "| a | b | c |\n|:--|:-:|--:|\n| 1 | 2 | 3 |\n",
     {"<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n<th>c</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>1</td>\n<td>2</td>\n<td>3</td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* align method none under XHTML */
    {CU_MARK_TABLE | CU_MARK_XHTML, CU_MARK_ALIGN_NONE, "| a | b | c |\n|:--|:-:|--:|\n| 1 | 2 | 3 |\n",
     {"<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n<th>c</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>1</td>\n<td>2</td>\n<td>3</td>\n</tr>\n</tbody>\n</table>\n", NULL, NULL}},
    /* a single tilde is not a delimiter; three are, consuming two */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "~~a~~ ~b~ ~~~c~~~ ~~d~~~ a~~b~~c ~~ ~~\n",
     {"<p><del>a</del> ~b~ <del><del>c</del></del> <del>d</del>~ a<del>b</del>c ~~ ~~</p>\n", NULL, NULL}},
    /* strikethrough and emphasis nest and cross */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "~~*a*~~ *~~a~~* ~~a *b~~ c*\n",
     {"<p><del><em>a</em></del> <em><del>a</del></em> <del>a *b</del> c*</p>\n", NULL, NULL}},
    /* the checkbox regexp \s: tab yes, two spaces no; trailing spaces after ] are consumed */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "- [ ] a\n- [x] b\n- [X] c\n- [  ] d\n- [\t] e\n-  [ ]\tf\n",
     {"<ul>\n<li><input disabled=\"\" type=\"checkbox\"> a</li>\n<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> b</li>\n<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> c</li>\n<li>[  ] d</li>\n<li><input disabled=\"\" type=\"checkbox\"> e</li>\n<li><input disabled=\"\" type=\"checkbox\"> f</li>\n</ul>\n", NULL, NULL}},
    /* a checkbox needs a first child of a list item with nothing before it */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "- a\n  [ ] b\n- *[ ] c*\n- [ ][ ] d\n\n[ ] e\n",
     {"<ul>\n<li>a\n[ ] b</li>\n<li><em>[ ] c</em></li>\n<li><input disabled=\"\" type=\"checkbox\"> [ ] d</li>\n</ul>\n<p>[ ] e</p>\n", NULL, NULL}},
    /* checkboxes in a loose list and under XHTML */
    {CU_MARK_TASKLIST | CU_MARK_XHTML, CU_MARK_ALIGN_DEFAULT, "- [ ] a\n\n- [x] b\n",
     {"<ul>\n<li>\n<p><input disabled=\"\" type=\"checkbox\" /> a</p>\n</li>\n<li>\n<p><input checked=\"\" disabled=\"\" type=\"checkbox\" /> b</p>\n</li>\n</ul>\n", NULL, NULL}},
    /* www and the three protocols; a port; a trailing dot or paren is cut */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "www.example.com http://x.y https://a.b.c/d?e=f#g ftp://h.i:21/j.\nhttp://a.b/c). http://a.b/(c) http://a.b/((c)\n",
     {"<p><a href=\"http://www.example.com\">www.example.com</a> <a href=\"http://x.y\">http://x.y</a> <a href=\"https://a.b.c/d?e=f#g\">https://a.b.c/d?e=f#g</a> <a href=\"ftp://h.i:21/j\">ftp://h.i:21/j</a>.\n<a href=\"http://a.b/c)\">http://a.b/c)</a>. <a href=\"http://a.b/(c)\">http://a.b/(c)</a> <a href=\"http://a.b/((c)\">http://a.b/((c)</a></p>\n", NULL, NULL}},
    /* a trailing entity is cut; an upper-case label is not a TLD */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "http://a.b/c&amp; http://a.b/c&amp;d; http://a.b/c; http://A.B http://a.B http://a.b.C/x\n",
     {"<p><a href=\"http://a.b/c\">http://a.b/c</a>&amp; <a href=\"http://a.b/c&amp;amp;d;\">http://a.b/c&amp;amp;d;</a> <a href=\"http://a.b/c;\">http://a.b/c;</a> http://A.B http://a.B <a href=\"http://a.b\">http://a.b</a>.C/x</p>\n", NULL, NULL}},
    /* emails: a dot in the domain is required; a following - or _ cancels */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "x@y.com x@y.com. x@y.com- x@y.c0m x@y x@y.com_ a.b@c.d.e\n",
     {"<p><a href=\"mailto:x@y.com\">x@y.com</a> <a href=\"mailto:x@y.com\">x@y.com</a>. x@y.com- <a href=\"mailto:x@y.c0m\">x@y.c0m</a> x@y x@y.com_ <a href=\"mailto:a.b@c.d.e\">a.b@c.d.e</a></p>\n", NULL, NULL}},
    /* the trigger character before a link becomes text */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "*www.a.b* _www.a.b_ ~www.a.b~ (www.a.b) (http://a.b)\n",
     {"<p><em><a href=\"http://www.a.b\">www.a.b</a></em> <em><a href=\"http://www.a.b\">www.a.b</a></em> ~<a href=\"http://www.a.b\">www.a.b</a>~ (<a href=\"http://www.a.b\">www.a.b</a>) (<a href=\"http://a.b\">http://a.b</a>)</p>\n", NULL, NULL}},
    /* no linkify inside a link label */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "[www.a.b](http://c.d) [http://e.f]\n",
     {"<p><a href=\"http://c.d\">www.a.b</a> [http://e.f]</p>\n", NULL, NULL}},
    /* the host run is at most 256 characters */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.b.com\n",
     {"<p><a href=\"http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.b.com\">http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.b.com</a></p>\n", NULL}},
    /* '$' is in the URL path class, not the www one */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "http://a.b/x$y www.a.b/x$y\n",
     {"<p><a href=\"http://a.b/x$y\">http://a.b/x$y</a> <a href=\"http://www.a.b/x\">www.a.b/x</a>$y</p>\n", NULL, NULL}},
    /* trailing punctuation is trimmed back to one character */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "http://a.b/x?!.,:*_~ www.a.b/x?!.,:*_~ x@y.com?!.\n",
     {"<p><a href=\"http://a.b/x\">http://a.b/x</a>?!.,:<em>_~ <a href=\"http://www.a.b/x\">www.a.b/x</a>?!.,:</em>_~ <a href=\"mailto:x@y.com\">x@y.com</a>?!.</p>\n", NULL, NULL}},
    /* non-ASCII ends a match */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "http://a.b\303\251/c www.a.b\303\251 x\303\251@y.com\n",
     {"<p><a href=\"http://a.b\">http://a.b</a>\303\251/c <a href=\"http://www.a.b\">www.a.b</a>\303\251 x\303\251@y.com</p>\n", NULL, NULL}},
    /* a space-led line (padding) is not a line head for linkify */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "x\n www.a.b\n",
     {"<p>x\n<a href=\"http://www.a.b\">www.a.b</a></p>\n", NULL, NULL}},
    /* decades, 'twas, apostrophes between letters */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "'90s '90 '9x '90s. 'twas 'em x'tis a'b 1'b a'1 \303\251'\303\251\n",
     {"<p>&rsquo;90s &lsquo;90 &lsquo;9x &rsquo;90s. &rsquo;twas &rsquo;em x&rsquo;tis a&rsquo;b 1&rsquo;b a'1 \303\251&rsquo;\303\251</p>\n", NULL, NULL}},
    /* Alice's, I've, I'll, You're and a left quote */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "'s 'm 't 'd 'sx 've 'll 're 'vex 'a\n",
     {"<p>&rsquo;s &rsquo;m &rsquo;t &rsquo;d &lsquo;sx &rsquo;ve &rsquo;ll &rsquo;re &lsquo;vex &lsquo;a</p>\n", NULL, NULL}},
    /* quotes close only while one is open; the counter is never reset between blocks */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "\"a\n\nb\" c\" 'd\n\ne' f'\n",
     {"<p>&ldquo;a</p>\n<p>b&rdquo; c&quot; &rsquo;d</p>\n<p>e&rsquo; f'</p>\n", NULL, NULL}},
    /* "Monitor 21"" keeps the inch mark */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "\"5\" \"5\"\" 5\"\" x\"\"\n",
     {"<p>&ldquo;5&rdquo; &ldquo;5&quot;&rdquo; 5&quot;&quot; x&quot;&quot;</p>\n", NULL, NULL}},
    /* dashes, ellipses and angle quotes; two dots are not an ellipsis */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "-- --- ---- ----- -x --x ... .... .. a...b << >> <<< >>>\n",
     {"<p>&ndash; &mdash; &mdash;- &mdash;&ndash; -x &ndash;x &hellip; &hellip;. .. a&hellip;b &laquo; &raquo; &laquo;&lt; &raquo;&gt;</p>\n", NULL, NULL}},
    /* nothing in code spans or URLs */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "`'a'` <'b'> ['c'](d) ![e](f 'g')\n",
     {"<p><code>'a'</code> &lt;&lsquo;b&rsquo;&gt; <a href=\"d\">&lsquo;c&rsquo;</a> <img src=\"f\" alt=\"e\" title=\"g\"></p>\n", NULL, NULL}},
    /* a quote before invalid UTF-8 (RuneError, neither letter nor digit) */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "'\377 'a\377 \"\377\n",
     {"<p>&lsquo;\377 &lsquo;a\377 &ldquo;\377</p>\n", NULL, NULL}},
    /* a second description, and a loose description */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "A\n: a\n: b\n\nB\n\n: c\n",
     {"<dl>\n<dt>A</dt>\n<dd>a</dd>\n<dd>b</dd>\n<dt>B</dt>\n<dd>\n<p>c</p>\n</dd>\n</dl>\n", NULL, NULL}},
    /* only the first paragraph of a tight description becomes a text block (goldmark's loop stops) */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "Term\n: desc 1\n\n  desc 2\n\n  desc 3\n",
     {"<dl>\n<dt>Term</dt>\n<dd>desc 1\n<p>desc 2</p>\n<p>desc 3</p>\n</dd>\n</dl>\n", NULL, NULL}},
    /* eight or more spaces after the colon count as five */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "A\n:        a\n\nB\n:         b\n\nC\n:\ta\n",
     {"<dl>\n<dt>A</dt>\n<dd>a</dd>\n<dt>B</dt>\n<dd><pre><code>b\n</code></pre>\n</dd>\n<dt>C</dt>\n<dd>a</dd>\n</dl>\n", NULL, NULL}},
    /* terms need a paragraph before; a colon alone or indented does not start a list */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, ": a\n\nA\n : a\n\nB\n:a\n",
     {"<p>: a</p>\n<p>A\n: a</p>\n<p>B\n:a</p>\n", NULL, NULL}},
    /* nested blocks in a description, and a nested list */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "A\n: - a\n  - b\n\nB\n: > q\n\nC\n: D\n  : d\n",
     {"<dl>\n<dt>A</dt>\n<dd><ul>\n<li>a</li>\n<li>b</li>\n</ul>\n</dd>\n<dt>B</dt>\n<dd><blockquote>\n<p>q</p>\n</blockquote>\n</dd>\n<dt>C</dt>\n<dd><dl>\n<dt>D</dt>\n<dd>d</dd>\n</dl>\n</dd>\n</dl>\n", NULL, NULL}},
    /* a definition list ends at a less indented line and at a setext bar */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "A\n: a\nB\n: b\n\nC\n: c\n===\n",
     {"<dl>\n<dt>A</dt>\n<dd>a\nB</dd>\n<dd>b</dd>\n<dt>C</dt>\n<dd>c\n===</dd>\n</dl>\n", NULL, NULL}},
    /* references number footnotes in reference order; a duplicate definition is unused */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "[^1]: x\n[^2]: y\n\na[^2] b[^1] c[^1]\n\n[^1]: z\n",
     {"<p>a<sup id=\"fnref:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup> b<sup id=\"fnref:2\"><a href=\"#fn:2\" class=\"footnote-ref\" role=\"doc-noteref\">2</a></sup> c<sup id=\"fnref1:2\"><a href=\"#fn:2\" class=\"footnote-ref\" role=\"doc-noteref\">2</a></sup></p>\n<div class=\"footnotes\" role=\"doc-endnotes\">\n<hr>\n<ol>\n<li id=\"fn:1\">\n<p>y&#160;<a href=\"#fnref:1\" class=\"footnote-backref\" role=\"do", "c-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n<li id=\"fn:2\">\n<p>x&#160;<a href=\"#fnref:2\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a>&#160;<a href=\"#fnref1:2\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n</ol>\n</div>\n", NULL}},
    /* labels are byte-exact, not normalised */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "a[^x] b[^X] c[^ 1 ]\n\n[^x]: x\n[^X]: X\n[^ 1 ]: one\n",
     {"<p>a<sup id=\"fnref:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup> b<sup id=\"fnref:2\"><a href=\"#fn:2\" class=\"footnote-ref\" role=\"doc-noteref\">2</a></sup> c<sup id=\"fnref:3\"><a href=\"#fn:3\" class=\"footnote-ref\" role=\"doc-noteref\">3</a></sup></p>\n<div class=\"footnotes\" role=\"doc-endnotes\">\n<hr>\n<ol>\n<li id=\"fn:1\">\n<p>x&#160;<a href=\"#fnref:1\" class=\"footnote-backref\" role=\"doc", "-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n<li id=\"fn:2\">\n<p>X&#160;<a href=\"#fnref:2\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n<li id=\"fn:3\">\n<p>one&#160;<a href=\"#fnref:3\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n</ol>\n</div>\n", NULL}},
    /* footnotes referenced only from footnotes, and unreferenced ones */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "a[^1]\n\n[^1]: x[^2]\n[^2]: y[^3]\n[^3]: z\n[^4]: w\n",
     {"<p>a<sup id=\"fnref:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup></p>\n<div class=\"footnotes\" role=\"doc-endnotes\">\n<hr>\n<ol>\n<li id=\"fn:1\">\n<p>x<sup id=\"fnref:2\"><a href=\"#fn:2\" class=\"footnote-ref\" role=\"doc-noteref\">2</a></sup>&#160;<a href=\"#fnref:1\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n<li id=\"fn:2\">\n<p>y<sup id=\"fnref:3\"><a href=\"#", "fn:3\" class=\"footnote-ref\" role=\"doc-noteref\">3</a></sup>&#160;<a href=\"#fnref:2\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n<li id=\"fn:3\">\n<p>z&#160;<a href=\"#fnref:3\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n</ol>\n</div>\n", NULL}},
    /* the ! before a footnote reference is text; an image label is not a footnote */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "a![^1] ![^1](u) [a[^1]](u)\n\n[^1]: x\n",
     {"<p>a!<sup id=\"fnref:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup> !<sup id=\"fnref1:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup>(u) <a href=\"u\">a<sup id=\"fnref2:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup></a></p>\n<div class=\"footnotes\" role=\"doc-endnotes\">\n<hr>\n<ol>\n<li id=\"fn:1\">\n<p>x&#160;<a href=\"#fnref:1\" class=\"footn", "ote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a>&#160;<a href=\"#fnref1:1\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a>&#160;<a href=\"#fnref2:1\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n</ol>\n</div>\n", NULL}},
    /* a footnote definition in a blockquote still lists at the end */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "> a[^1]\n>\n> [^1]: x\n\nb\n",
     {"<blockquote>\n<p>a<sup id=\"fnref:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup></p>\n</blockquote>\n<p>b</p>\n<div class=\"footnotes\" role=\"doc-endnotes\">\n<hr>\n<ol>\n<li id=\"fn:1\">\n<p>x&#160;<a href=\"#fnref:1\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n</ol>\n</div>\n", NULL, NULL}},
    /* continuation needs four spaces; a backlink goes after the last paragraph */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "a[^1]\n\n[^1]: x\n   y\n\n[^2]: p\n\n    q\n\n    - r\n\nb[^2]\n",
     {"<p>a<sup id=\"fnref:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup></p>\n<p>b<sup id=\"fnref:2\"><a href=\"#fn:2\" class=\"footnote-ref\" role=\"doc-noteref\">2</a></sup></p>\n<div class=\"footnotes\" role=\"doc-endnotes\">\n<hr>\n<ol>\n<li id=\"fn:1\">\n<p>x\ny&#160;<a href=\"#fnref:1\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n<li id=\"fn:2\">\n<p>p</p>\n<p>q</p>\n<ul", ">\n<li>r</li>\n</ul>\n&#160;<a href=\"#fnref:2\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></li>\n</ol>\n</div>\n", NULL}},
    /* a footnote with an empty label or no content */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "a[^]\n\n[^]: x\n\nb[^1]\n\n[^1]:\n",
     {"<p>a<a href=\"x\">^</a></p>\n<p>b<sup id=\"fnref:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup></p>\n<div class=\"footnotes\" role=\"doc-endnotes\">\n<hr>\n<ol>\n<li id=\"fn:1\">\n&#160;<a href=\"#fnref:1\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></li>\n</ol>\n</div>\n", NULL, NULL}},
    /* footnotes under XHTML */
    {CU_MARK_FOOTNOTE | CU_MARK_XHTML, CU_MARK_ALIGN_DEFAULT, "[^1]: x\n\na[^1] a[^1]\n",
     {"<p>a<sup id=\"fnref:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup> a<sup id=\"fnref1:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup></p>\n<div class=\"footnotes\" role=\"doc-endnotes\">\n<hr />\n<ol>\n<li id=\"fn:1\">\n<p>x&#160;<a href=\"#fnref:1\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a>&#160;<a href=\"#fnref1:1\" class=\"footnote-backref\" ro", "le=\"doc-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n</ol>\n</div>\n", NULL}},
    /* Hugo ids: letters and digits of any script are kept and lower-cased, the rest dropped */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "# \303\211t\303\251 Caf\303\251\n# \303\211t\303\251 Caf\303\251\n# a_b-c d\n# ---\n#\n# 123 \331\241\331\242\331\243\n# \344\270\255\346\226\207 \346\226\207\n# a\377b\n# \302\240x\302\240\n",
     {"<h1 id=\"\303\251t\303\251-caf\303\251\">\303\211t\303\251 Caf\303\251</h1>\n<h1 id=\"\303\251t\303\251-caf\303\251-1\">\303\211t\303\251 Caf\303\251</h1>\n<h1 id=\"a_b-c-d\">a_b-c d</h1>\n<h1 id=\"---\">&mdash;</h1>\n<h1 id=\"heading\"></h1>\n<h1 id=\"123-\331\241\331\242\331\243\">123 \331\241\331\242\331\243</h1>\n<h1 id=\"\344\270\255\346\226\207-\346\226\207\">\344\270\255\346\226\207 \346\226\207</h1>\n<h1 id=\"ab\">a\377b</h1>\n<h1 id=\"x\">\302\240x\302\240</h1>\n", NULL, NULL}},
    /* Hugo ids: duplicates take -1, -2, ... skipping taken ones; explicit ids are registered */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "# heading\n# heading\n# heading-1\n# heading\n# a\n# a\n# a-1\n# a-1\n# x {#heading}\n# {#a}\n# a\n",
     {"<h1 id=\"heading\">heading</h1>\n<h1 id=\"heading-1\">heading</h1>\n<h1 id=\"heading-1-1\">heading-1</h1>\n<h1 id=\"heading-2\">heading</h1>\n<h1 id=\"a\">a</h1>\n<h1 id=\"a-1\">a</h1>\n<h1 id=\"a-1-1\">a-1</h1>\n<h1 id=\"a-1-2\">a-1</h1>\n<h1 id=\"heading\">x</h1>\n<h1 id=\"a\"></h1>\n<h1 id=\"a-2\">a</h1>\n", NULL, NULL}},
    /* Hugo ids: Unicode white space is trimmed, not inner; ToLower on titlecase and final sigma */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "# \342\200\203x\342\200\203\n# a\343\200\200b\n# \307\205\307\210 \316\243\317\202 \304\260\n# \360\237\230\200 x\n# \360\220\220\200\n",
     {"<h1 id=\"x\">\342\200\203x\342\200\203</h1>\n<h1 id=\"ab\">a\343\200\200b</h1>\n<h1 id=\"\307\206\307\211-\317\203\317\202-i\">\307\205\307\210 \316\243\317\202 \304\260</h1>\n<h1 id=\"-x\">\360\237\230\200 x</h1>\n<h1 id=\"\360\220\220\250\">\360\220\220\200</h1>\n", NULL, NULL}},
    /* goldmark's own ids drop non-ASCII (contrast with Hugo's) */
    {CU_MARK_AUTO_HEADING_ID, CU_MARK_ALIGN_DEFAULT, "# \303\211t\303\251 Caf\303\251\n# \303\211\n",
     {"<h1 id=\"t-caf\">\303\211t\303\251 Caf\303\251</h1>\n<h1 id=\"heading\">\303\211</h1>\n", NULL, NULL}},
    /* Hugo ids without the attribute option */
    {CU_MARK_AUTO_HEADING_ID | CU_MARK_HUGO_IDS, CU_MARK_ALIGN_DEFAULT, "# a {#x}\n# a\n",
     {"<h1 id=\"a-x\">a {#x}</h1>\n<h1 id=\"a\">a</h1>\n", NULL, NULL}},
    /* every extension in one table row */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "| a |\n|---|\n| 'x' -- ~~y~~ www.z.w [^1] [ ] |\n\n[^1]: f\n",
     {"<table>\n<thead>\n<tr>\n<th>a</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>&lsquo;x&rsquo; &ndash; <del>y</del> <a href=\"http://www.z.w\">www.z.w</a> <sup id=\"fnref:1\"><a href=\"#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup> [ ]</td>\n</tr>\n</tbody>\n</table>\n<div class=\"footnotes\" role=\"doc-endnotes\">\n<hr>\n<ol>\n<li id=\"fn:1\">\n<p>f&#160;<a href=\"#fnref:1\" class=\"footnote-backref\" role=\"doc-backlink\">", "&#x21a9;&#xfe0e;</a></p>\n</li>\n</ol>\n</div>\n", NULL}},
    /* a task list item with typographer and linkify */
    {CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT, "- [ ] 'x' -- www.a.b\n- [x] \"y\"\n",
     {"<ul>\n<li><input disabled=\"\" type=\"checkbox\"> &lsquo;x&rsquo; &ndash; <a href=\"http://www.a.b\">www.a.b</a></li>\n<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> &ldquo;y&rdquo;</li>\n</ul>\n", NULL, NULL}},
};

static CuMarkOptions options(unsigned long flags, CuMarkTableAlign align)
{
    CuMarkOptions o;

    o.flags = flags;
    o.table_align = align;
    o.code_hook = NULL;
    o.code_ctx = NULL;
    return o;
}

static CuStr join(CuArena *a, const char *const *parts, size_t n)
{
    CuBuilder b;
    CuStr s;
    size_t i;

    cu_sb_init(&b, a);
    for (i = 0; i < n && parts[i] != NULL; i++) cu_sb_str(&b, cu_str_cstr(parts[i]));
    assert(cu_sb_finish(&b, &s));
    return s;
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

static void test_oracle_cases(CuArena *scratch, CuArena *out_arena)
{
    size_t i;

    for (i = 0; i < sizeof oracle_cases / sizeof oracle_cases[0]; i++) {
        const Case *c = &oracle_cases[i];
        CuMarkOptions opt = options(c->flags, c->align);
        CuTemp t = cu_temp_begin(out_arena);
        CuStr want = join(out_arena, c->out, 3);
        CuStr got;
        CuMarkStatus st = render(scratch, out_arena, cu_str_cstr(c->in), &opt, &got);

        if (st != CU_MARK_OK || !cu_str_eq(got, want)) {
            fprintf(stderr, "case %lu: status %d\ninput:\n%s\nwant:\n%.*s\ngot:\n%.*s\n", (unsigned long)i, (int)st,
                    c->in, (int)want.len, want.ptr, (int)got.len, got.ptr);
            abort();
        }
        cu_temp_end(t);
    }
}

/* Appends unit n times, then tail. */
static void repeat_into(CuBuilder *b, const char *unit, size_t n, const char *tail)
{
    size_t i;

    for (i = 0; i < n; i++) cu_sb_str(b, cu_str_cstr(unit));
    cu_sb_str(b, cu_str_cstr(tail));
}

/* n footnote definitions each nested in the previous one (four more
 * spaces per level), then a paragraph referencing them all. */
static CuStr nested_footnotes(CuArena *a, size_t n)
{
    CuBuilder b;
    CuStr s;
    size_t i;

    cu_sb_init(&b, a);
    for (i = 0; i < n; i++) {
        repeat_into(&b, "    ", i, "[^");
        cu_sb_ulong(&b, (unsigned long)i);
        cu_sb_str(&b, CU_LIT("]: x\n"));
    }
    for (i = 0; i < n; i++) {
        cu_sb_str(&b, CU_LIT("[^"));
        cu_sb_ulong(&b, (unsigned long)i);
        cu_sb_str(&b, CU_LIT("]"));
    }
    cu_sb_str(&b, CU_LIT("\n"));
    assert(cu_sb_finish(&b, &s));
    return s;
}

/* n definition lists each nested in the previous description (two more
 * spaces per level). */
static CuStr nested_definition_lists(CuArena *a, size_t n)
{
    CuBuilder b;
    CuStr s;
    size_t i;

    cu_sb_init(&b, a);
    for (i = 0; i < n; i++) {
        repeat_into(&b, "  ", i, "T\n");
        repeat_into(&b, "  ", i, i + 1 == n ? ": d\n" : ": \n");
    }
    assert(cu_sb_finish(&b, &s));
    return s;
}

/* Footnote definitions and definition lists are block containers, so the
 * depth bound of cu_mark.h counts them: a footnote per level, a list and
 * a description per level. */
static void test_limit_depth(CuArena *scratch, CuArena *out_arena)
{
    CuTemp t = cu_temp_begin(out_arena);
    CuMarkOptions opt = options(CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT);
    CuStr got;

    /* goldmark renders nested footnote definitions as nothing at all (the
     * oracle agrees), so only the status is checked here */
    assert(render(scratch, out_arena, nested_footnotes(out_arena, CU_MARK_MAX_DEPTH), &opt, &got) == CU_MARK_OK);
    assert(render(scratch, out_arena, nested_footnotes(out_arena, CU_MARK_MAX_DEPTH + 1), &opt, &got) ==
           CU_MARK_LIMIT);
    assert(got.len == 0);
    assert(render(scratch, out_arena, nested_definition_lists(out_arena, CU_MARK_MAX_DEPTH / 2), &opt, &got) ==
           CU_MARK_OK);
    assert(cu_str_starts_with(got, CU_LIT("<dl>\n<dt>T</dt>\n<dd><dl>\n")));
    assert(render(scratch, out_arena, nested_definition_lists(out_arena, CU_MARK_MAX_DEPTH / 2 + 1), &opt, &got) ==
           CU_MARK_LIMIT);
    assert(got.len == 0);
    cu_temp_end(t);
}

/* A document that exercises every extension, for the allocation tests. */
static const char nomem_doc[] =
    "# Caf\303\251 {.c}\n# Caf\303\251\n\n| a | b |\n|:-:|--:|\n| `x\\|y` | ~~s~~ |\n\n- [ ] 'twas -- www.a.b x@y.co\n- [x] \"q\" ... <<\n\nTerm\n: desc[^1] [^2]\n\n[^1]: one[^2]\n[^2]: two\n";

/* The oracle's output for nomem_doc (-hugo -unsafe), in pieces. */
static const char *const nomem_want_parts[] = {
    "<h1 class=\"c\" id=\"caf\303\251\">Caf\303\251</h1>\n<h1 id=\"caf\303\251-1\">Caf\303\251</h1>\n<table>\n<thead>\n<tr>\n<th style=\"text-align:center\">a</th>\n<th style=\"text-align:right\">b</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td style=\"text-align:center\"><code>x|y</code></td>\n<td style=\"text-align:right\"><del>s</del></td>\n</tr>\n</tbody>\n",
    "</table>\n<ul>\n<li><input disabled=\"\" type=\"checkbox\"> &rsquo;twas &ndash; <a href=\"http://www.a.b\">www.a.b</a> <a href=\"mailto:x@y.co\">x@y.co</a></li>\n<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> &ldquo;q&rdquo; &hellip; &laquo;</li>\n</ul>\n<dl>\n<dt>Term</dt>\n<dd>desc<sup id=\"fnref:1\"><a href=\"",
    "#fn:1\" class=\"footnote-ref\" role=\"doc-noteref\">1</a></sup> <sup id=\"fnref:2\"><a href=\"#fn:2\" class=\"footnote-ref\" role=\"doc-noteref\">2</a></sup></dd>\n</dl>\n<div class=\"footnotes\" role=\"doc-endnotes\">\n<hr>\n<ol>\n<li id=\"fn:1\">\n<p>one<sup id=\"fnref1:2\"><a href=\"#fn:2\" class=\"footnote-ref\" role=\"doc-not",
    "eref\">2</a></sup>&#160;<a href=\"#fnref:1\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a></p>\n</li>\n<li id=\"fn:2\">\n<p>two&#160;<a href=\"#fnref:2\" class=\"footnote-backref\" role=\"doc-backlink\">&#x21a9;&#xfe0e;</a>&#160;<a href=\"#fnref1:2\" class=\"footnote-backref\" role=\"doc-backlink\">",
    "&#x21a9;&#xfe0e;</a></p>\n</li>\n</ol>\n</div>\n",
};

/* Each scratch allocation fails in turn (debug arenas make every push an
 * attempt): the render must report CU_MARK_NOMEM or, once the failing
 * attempt lies past the run, render exactly. */
static void test_nomem_scratch(CuArena *out_arena)
{
    CuMarkOptions opt = options(CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT);
    CuArena scratch;
    size_t n;
    size_t failures = 0;
    size_t successes = 0;
    CuTemp outer = cu_temp_begin(out_arena);
    CuStr want = join(out_arena, nomem_want_parts, sizeof nomem_want_parts / sizeof nomem_want_parts[0]);

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

/* The same with the output builder's arena failing. */
static void test_nomem_output(CuArena *scratch)
{
    CuMarkOptions opt = options(CU_MARK_HUGO | CU_MARK_UNSAFE, CU_MARK_ALIGN_DEFAULT);
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

int main(void)
{
    CuArena scratch;
    CuArena out_arena;

    cu_arena_init(&scratch);
    cu_arena_init(&out_arena);
    test_oracle_cases(&scratch, &out_arena);
    test_limit_depth(&scratch, &out_arena);
    test_nomem_scratch(&out_arena);
    test_nomem_output(&scratch);
    cu_arena_release(&out_arena);
    cu_arena_release(&scratch);
    printf("PASS test_ext\n");
    return 0;
}
