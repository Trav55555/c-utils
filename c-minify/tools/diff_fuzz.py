#!/usr/bin/env python3
"""Differential fuzzing of c-minify against the minify oracle.

Generates random HTML and XML documents from a seed (a token soup weighted
toward what the minifiers look at: block and inline tags, raw-text and
void elements, attributes quoted every way, entities named, decimal and
hex, comments of every kind including conditional ones, doctypes, CDATA,
meta viewport and keywords, data: URLs, nested iframes, XML script types,
whitespace runs, NUL and invalid UTF-8) and minifies each with the C
driver (tools/minify_run.c) and the Go oracle (c-minify/oracle) under
several option sets. Every byte must agree.

The oracle's -cases mode carries the documents in base64, so one process
handles a whole set and invalid UTF-8 travels intact. It also reports
which embedded minifiers a document reached; one that reaches the CSS,
JS, JSON or SVG minifier is skipped, since this slice does not port those,
and counted. Where the oracle returns an error (a NUL where its XML lexer
forbids one) the driver must report CU_MINIFY_INVALID.

A mismatch is shrunk (lines first, then bytes, keeping the mismatch) and
saved under --save as a directory per case: input, expected (the oracle's
bytes), actual and case.json. --replay reruns saved cases through the
driver alone.

    python3 -I tools/diff_fuzz.py --oracle BIN --driver BIN [--seed S]
        [--count N] [--sets a,b] [--save DIR] [--jobs J]
    python3 -I tools/diff_fuzz.py --driver BIN --replay DIR

Standard library only. Exit 0 when every document agrees, 1 otherwise.
"""
import argparse
import base64
import concurrent.futures
import hashlib
import json
import os
import random
import subprocess
import sys
import tempfile

# CU_MINIFY_* flag bits, as include/cu_minify.h defines them.
FLAG_BITS = {
    "keepComments": 0x01, "keepConditionalComments": 0x02, "keepDefaultAttrVals": 0x04,
    "keepDocumentTags": 0x08, "keepEndTags": 0x10, "keepQuotes": 0x20, "keepWhitespace": 0x40,
}
SITE = {"keepComments": True, "keepConditionalComments": True, "keepDefaultAttrVals": True,
        "keepDocumentTags": True, "keepEndTags": True}
UNPORTED = {"css", "js", "json", "svg"}
STATUS_INVALID = 4  # CU_MINIFY_INVALID


class OptionSet:
    def __init__(self, name, typ, options):
        self.name = name
        self.typ = typ
        self.options = options
        self.html = typ == "text/html"

    def driver_args(self):
        flags = 0
        for key, bit in FLAG_BITS.items():
            if self.options.get(key):
                flags |= bit
        return ["-type", self.typ, "-flags", "0x%x" % flags]


def _opts(*names):
    return {n: True for n in names}


SETS = [
    OptionSet("site-html", "text/html", SITE),
    OptionSet("html-none", "text/html", {}),
    OptionSet("html-keepws", "text/html", _opts("keepWhitespace")),
    OptionSet("html-keepcond", "text/html", _opts("keepConditionalComments", "keepEndTags")),
    OptionSet("html-keepquotes", "text/html", _opts("keepQuotes", "keepDocumentTags")),
    OptionSet("site-xml", "application/xml", SITE),
    OptionSet("xml-keepws", "application/xml", _opts("keepWhitespace")),
]
SET_BY_NAME = {s.name: s for s in SETS}

# (weight, token)
COMMON_TOKENS = [
    (12, b"a"), (8, b"b"), (6, b"foo"), (4, b"x"), (3, b"0"), (3, b"1"),
    (14, b" "), (6, b"  "), (10, b"\n"), (5, b"\n\n"), (3, b"\r\n"), (2, b"\t"), (1, b"\f"), (1, b"\r"),
    (2, b"<"), (2, b">"), (2, b"</"), (2, b"<!"), (2, b"<?"), (2, b"/>"), (2, b"="), (3, b'"'), (3, b"'"),
    (4, b"&amp;"), (3, b"&lt;"), (3, b"&gt;"), (2, b"&quot;"), (2, b"&apos;"), (2, b"&nbsp;"), (2, b"&#160;"),
    (2, b"&#x3C;"), (2, b"&#60;"), (2, b"&#65;"), (2, b"&bogus;"), (3, b"&"), (1, b"&#"), (1, b"&#x"),
    (1, b"&#x10000000000000000;"), (1, b"&#xFFFFFFFFFFFFFFFF;"), (1, b"&#99999;"), (1, b"&AMP;"), (1, b"&LT;"),
    (1, b"&rsquor;"), (1, b"&CounterClockwiseContourIntegral;"), (1, b"&#x270F;"), (1, b"&amp;x"), (1, b"&amp;;"),
    (3, "é".encode()), (2, "中文".encode()), (1, "\U0001F600".encode()),
    (2, b"\x00"), (2, b"\xff"), (1, b"\xc3"), (1, b"\xe2\x82"), (1, b"\x80"),
    (1, b"%20"), (1, b"%3C"), (1, b"%"), (1, b"+"), (1, b","), (1, b";"), (1, b":"),
]
HTML_TOKENS = [
    (5, b"<p>"), (5, b"</p>"), (4, b"<div>"), (4, b"</div>"), (4, b"<span>"), (4, b"</span>"), (3, b"<b>"), (3, b"</b>"),
    (2, b"<i class=fas>"), (2, b"</i>"), (2, b"<code>"), (2, b"</code>"), (2, b"<br>"), (2, b"<br/>"), (2, b"<img>"),
    (2, b"<img src=a alt=''>"), (2, b"<hr>"), (2, b"<ul>"), (2, b"</ul>"), (3, b"<li>"), (3, b"</li>"), (2, b"<ol>"),
    (2, b"<table>"), (2, b"</table>"), (2, b"<tr>"), (2, b"</tr>"), (2, b"<td>"), (2, b"</td>"), (1, b"<th>"), (1, b"</th>"),
    (1, b"<thead>"), (1, b"</thead>"), (1, b"<tbody>"), (1, b"</tbody>"), (1, b"<colgroup>"), (1, b"</colgroup>"), (1, b"<col>"),
    (2, b"<select>"), (2, b"</select>"), (2, b"<option>"), (2, b"</option>"), (1, b"<optgroup>"), (1, b"</optgroup>"),
    (1, b"<dl>"), (1, b"<dt>"), (1, b"</dt>"), (1, b"<dd>"), (1, b"</dd>"), (1, b"<ruby>"), (1, b"<rt>"), (1, b"</rt>"),
    (1, b"<rp>"), (1, b"</rp>"), (1, b"<rb>"), (1, b"<rtc>"),
    (2, b"<a href=x>"), (2, b"</a>"), (2, b"<a id=x name=x>"), (1, b"<a id=x name=y>"), (1, b"<a name=''>"),
    (1, b"<canvas>"), (1, b"</canvas>"), (1, b"<video>"), (1, b"</video>"), (1, b"<button>"), (1, b"</button>"),
    (1, b"<input>"), (1, b"<progress></progress>"), (1, b"<picture>"), (1, b"</picture>"), (1, b"<noscript>"),
    (1, b"</noscript>"), (1, b"<template>"), (1, b"</template>"), (1, b"<h1>"), (1, b"</h1>"), (1, b"<form>"),
    (1, b"</form>"), (1, b"<nav>"), (1, b"</nav>"), (1, b"<main>"), (1, b"</main>"), (1, b"<strike>"), (1, b"</strike>"),
    (2, b"<html>"), (2, b"</html>"), (2, b"<head>"), (2, b"</head>"), (2, b"<body>"), (2, b"</body>"), (1, b"<body id=x>"),
    (1, b"<html lang=en>"), (2, b"<!doctype html>"), (1, b"<!DOCTYPE html PUBLIC 'x'>"), (1, b"<!doctype"),
    (2, b"<DIV TITLE='x'>"), (1, b"</DIV >"), (1, b"</p >"), (1, b"</ p>"), (1, b"</0>"), (1, b"</>"), (1, b"<p/>"),
    (2, b"<pre>"), (2, b"</pre>"), (2, b"<textarea>"), (2, b"</textarea>"), (2, b"<title>"), (2, b"</title>"),
    (1, b"<xmp>"), (1, b"</xmp>"), (1, b"<plaintext>"), (1, b"<TEXTAREA>"), (1, b"</TEXTAREA>"),
    (2, b"<iframe>"), (2, b"</iframe>"), (1, b"<iframe src=x>"), (1, b"<iframe><p> a </p></iframe>"),
    (1, b"<math>"), (1, b"</math>"), (1, b"<math> &int; </math>"),
    (2, b"<script type=text/xml>"), (2, b"</script>"), (1, b"<script type='text/xml'> <a> </a> </script>"),
    (1, b"<script type=text/template>"), (1, b"<script type=text/html> <p> x </script>"), (1, b"<script src=x></script>"),
    (1, b"<script src=x charset=y></script>"), (1, b"<script></script>"), (1, b"<script> </script>"), (1, b"<script"),
    (1, b"<style></style>"), (1, b"<style> </style>"), (1, b"<style amp-boilerplate>"), (1, b"<style media=all>"),
    (1, b" style=''"), (1, b" style='  '"), (1, b" onclick=''"), (1, b" onclick=javascript:"),
    (1, b"<style>"), (1, b"<script>"), (1, b"<svg>"), (1, b"</svg>"), (1, b"<svg><g/></svg>"), (1, b" style=x"), (1, b" onclick=x"),
    (3, b"<!-- c -->"), (2, b"<!---->"), (1, b"<!-- c --!>"), (1, b"<!--"), (1, b"-->"), (2, b"<!--# ssi -->"),
    (2, b"<!--[if IE 6]>"), (2, b"<![endif]-->"), (1, b"<!--[if IE]><![endif]-->"), (1, b"<!--[if !mso]><!-->"),
    (1, b"<!--<![endif]-->"), (1, b"<![if IE]>"), (1, b"<![endif]>"), (1, b"<!--[if IE]><p> a </p><![endif]-->"),
    (1, b"<!--[if IE]><iframe> <b> </iframe><![endif]-->"), (1, b"<!--[endif]-->"), (1, b"<!--[if IE]-->"),
    (2, b"<?php x ?>"), (1, b"<!bogus>"), (2, b"<![CDATA[ x ]]>"), (1, b"<![CDATA["), (1, b"]]>"),
    (3, b"<p class=' a  b '>"), (2, b"<p class=''>"), (2, b"<p id=''>"), (1, b"<p dir=''>"), (1, b"<p name=''>"),
    (1, b"<p lang=''>"), (1, b"<form action=''>"), (1, b"<form method=GET>"), (1, b"<form enctype='application/x-www-form-urlencoded'>"),
    (1, b"<td colspan=1 rowspan=one>"), (1, b"<col span=1>"), (1, b"<area shape=RECT>"), (1, b"<input type=text>"),
    (1, b"<input type=radio value=on>"), (1, b"<input type=radio value=''>"), (1, b"<input type=text value=''>"),
    (1, b"<input value='' type=x>"), (1, b"<button type=submit>"), (1, b"<link type=text/css rel=stylesheet>"),
    (1, b"<style type=text/css>"), (1, b"<script type=text/javascript></script>"), (1, b"<script type='TEXT/JavaScript'></script>"),
    (1, b"<span selected=selected>"), (1, b"<span hidden>"), (1, b"<input disabled=''>"), (1, b"<input checked=checked>"),
    (1, b"<a href='  http://X.y/  '>"), (1, b"<a href=HTTPS://x.y>"), (1, b"<a href=HTTP:x>"), (1, b"<a href=http>"),
    (2, b"<a href='data:text/plain,hello world'>"), (1, b"<a href='data:,hello'>"), (1, b"<a href=data:text/plain;base64,aGVsbG8=>"),
    (1, b"<a href='data:;base64,aGVsbG8='>"), (1, b"<a href='data:text/plain;base64,aGVs\nbG8='>"),
    (1, b"<a href='data:text/plain;base64,aGVsbG8'>"), (1, b"<a href='data:text/plain;charset=us-ascii,x'>"),
    (1, b"<a href='data:text/plain;charset=US-ASCII;foo=bar,x%20y'>"), (1, b"<a href='data:image/png;base64,iVBORw0KGgo='>"),
    (1, b"<a href='data:text/html,<p> x </p>'>"), (1, b"<a href='data:text/xml, <a> </a> '>"),
    (1, b"<a href='data:text/html,<a href=%22data:text/html,%3Cb%3E x %3C/b%3E%22>'>"),
    (1, b"<a href='data:text/plain,%ff%41+b%3'>"), (1, b"<a href='DATA:,x'>"), (1, b"<a href='data:'>"), (1, b"<a href='data:,'>"),
    (1, b"<a href='data:,a%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C%3C'>"),
    (1, b"<a href='data:text/xml,<a>%00</a>'>"), (1, b"<a href='data:text/css,a{}'>"),
    (2, b"<meta name=viewport content='width=device-width, initial-scale=1.0'>"),
    (1, b"<meta name=viewport content='width=0.50, x=.5e-10, y=1e400, z=+01, w=-0.0, v=1234.5678e-9, u=5., t=100000'>"),
    (1, b"<meta name=viewport content='a=1e+2, b=.000001, c=9e1, d=123.456e2, e=0.0001, f=12345678901234567890'>"),
    (1, b"<meta name=viewport content='a=1e9999999999999999999, b=00.00, c=.0, d=0e0, e=-.5e-5, f=1.e5, g=1e'>"),
    (1, b"<meta name=keywords content='a, b,  c'>"), (1, b"<meta http-equiv='Content-Type' content='text/html; charset=UTF-8'>"),
    (1, b"<meta http-equiv=content-type content='text/plain, text/html'>"), (1, b"<meta charset=utf-8 http-equiv=content-type content=x>"),
    (1, b"<meta property=og:x content='a b'>"), (1, b"<meta name=x content=\"a'b\">"), (1, b"<meta e t n content=ful>"),
    (2, b"<span attr=test>"), (2, b"<span attr='test&apos;test'>"), (2, b"<span attr=\"test&quot;test\">"),
    (2, b"<span attr='test\"\"&apos;&amp;test'>"), (1, b"<span attr=test/>"), (1, b"<span attr=''>"), (1, b"<span attr=\"\">"),
    (1, b"<span attr='a=b'>"), (1, b"<span attr='<>'>"), (1, b"<span attr='`'>"), (1, b"<span a=b c>"), (1, b"<span a = b >"),
    (1, b"<span a\n=\n'b'>"), (1, b"<span a='b"), (1, b"<span a="), (1, b"<span a"), (1, b"<span attr=\"a'b&quot;c\">"),
    (1, b"<img alt=a'b=\"\">"), (1, b"<span vocab='x' property='y' content='z\"w'>"), (1, b"<p accept='image/png, image/jpeg'>"),
    (1, b"<p title=' a '>"), (1, b"<p accesskey=' a  b '>"), (1, b"<p translate=no>"), (1, b"<amp-analytics type=x>"),
    (1, b"<source type='video/mp4; codecs=\"av01\"'>"), (1, b"<a type='TEXT/HTML; charset=\"X\"'>"), (1, b"<embed type=' x/Y '>"),
    (1, b"<input maxlength='  5 '>"), (1, b"<html xmlns='http://www.w3.org/1999/xhtml'>"), (1, b"<p a='&#39;' b='&#34;'>"),
]
XML_TOKENS = [
    (6, b"<a>"), (6, b"</a>"), (4, b"<b>"), (4, b"</b>"), (3, b"<c/>"), (2, b"<x y='1'>"), (2, b'<x y="2">'), (2, b"<x y=3>"),
    (2, b"<x y=''>"), (2, b'<x y="">'), (2, b"<x y=\"a'b\">"), (2, b"<x y='a\"b'>"), (2, b"<x y='&quot;&apos;&amp;&lt;&gt;'>"),
    (2, b"<x y=\"&quot;&quot;'\">"), (1, b"<x y=\" a \n\r\t b \">"), (1, b"<x y>"), (1, b"<x y="), (1, b"<x y='"), (1, b"<x y z>"),
    (1, b"<x\ny\t=\r'1'>"), (1, b"<A>"), (1, b"</A>"), (1, b"<a:b>"), (1, b"</a:b>"), (1, b"<a >"), (1, b"</a >"), (1, b"</a\n>"),
    (2, b"<?xml version='1.0'?>"), (1, b"<?xml  version=\"1.0\" ?>"), (1, b"<?pi x?>"), (1, b"<?pi?>"), (1, b"<?"),
    (2, b"<!DOCTYPE foo SYSTEM 'Foo.dtd'>"), (1, b"<!DOCTYPE a [ <!ELEMENT a (b)> ]>"), (1, b"<!DOCTYPE a [ \">\" ]>"),
    (1, b"<!DOCTYPE"), (1, b"<!doctype html>"), (1, b"<!x>"),
    (3, b"<![CDATA[ a ]]>"), (2, b"<![CDATA[<<<<<<]]>"), (2, b"<![CDATA[]]>"), (1, b"<![CDATA[ a < b & c ]]>"), (1, b"<![CDATA["),
    (1, b"]]>"), (1, b"<![CDATA[&&&&]]>"), (1, b"<![CDATA[ ]]>"),
    (3, b"<!-- c -->"), (1, b"<!---->"), (1, b"<!--"), (1, b"-->"), (1, b"<!-- > -->"),
    (2, b"<x></x>"), (2, b"<x> </x>"), (1, b"<x>\n</x>"), (1, b"<x a='b'></x>"), (1, b"<x/></x>"),
    (1, b"</0"), (1, b"</>"), (1, b"< a>"), (1, b"<a/ >"),
    (2, b"&#38;"), (1, b"&#038;"), (1, b"&#x26;"), (1, b"&#X26;"), (1, b"&#60;"), (1, b"&#x3c;"), (1, b"&#x3C;"), (1, b"&#X3C;"),
    (1, b"&#x;"), (1, b"&#;"), (1, b"&#39;"), (1, b"&#34;"),
    (2, b"text"), (1, b"<rss><channel><item><description>&lt;p>x&lt;/p>\n\n&lt;pre>y&lt;/pre></description></item></channel></rss>"),
]


def generate(rng, html):
    tokens = COMMON_TOKENS + (HTML_TOKENS if html else XML_TOKENS)
    weights = [w for w, _ in tokens]
    values = [t for _, t in tokens]
    n = rng.randint(1, 40) if rng.random() < 0.9 else rng.randint(40, 200)
    return b"".join(rng.choices(values, weights, k=n))


class Want:
    """The oracle's verdict on one document: the bytes, an error, or skip."""

    def __init__(self, out=None, error=None, skip=False):
        self.out = out
        self.error = error
        self.skip = skip


class Mismatch:
    def __init__(self, opts, doc, want, got, note):
        self.opts = opts
        self.doc = doc
        self.want = want
        self.got = got
        self.note = note


def run(cmd, data, timeout):
    try:
        p = subprocess.run(cmd, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, b"", b"timeout"
    return p.returncode, p.stdout, p.stderr


class Runner:
    def __init__(self, oracle, driver, jobs):
        self.oracle = oracle
        self.driver = driver
        self.pool = concurrent.futures.ThreadPoolExecutor(max_workers=jobs)
        self.skipped = 0

    def minify_c(self, opts, doc):
        """(status, bytes, note): status 0 with the bytes, or the driver's
        CU_MINIFY_* status from its "status N" line."""
        rc, out, err = run([self.driver] + opts.driver_args(), doc, 30)
        if rc == 0:
            return 0, out, ""
        text = err.decode("utf-8", "replace").strip()
        if rc == 3 and text.startswith("status "):
            return int(text.split()[1]), b"", text
        return -1, b"", "driver exit %s: %s" % (rc, text)

    def oracle_many(self, opts, docs):
        cases = [{"type": opts.typ, "options": opts.options, "input": base64.b64encode(d).decode("ascii")}
                 for d in docs]
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "cases.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump(cases, f)
            rc, out, err = run([self.oracle, "-cases", path], b"", 600)
        if rc != 0:
            raise SystemExit("oracle failed: %s" % err.decode("utf-8", "replace"))
        wants = []
        for r in json.loads(out.decode("utf-8")):
            if UNPORTED & set(r["reaches"]):
                wants.append(Want(skip=True))
            elif r.get("error"):
                wants.append(Want(error=r["error"]))
            else:
                wants.append(Want(out=base64.b64decode(r["output"])))
        return wants

    def oracle_one(self, opts, doc):
        return self.oracle_many(opts, [doc])[0]

    def compare(self, opts, doc, want):
        """A Mismatch for doc, or None when the driver agrees with want."""
        if want.skip:
            return None
        status, got, note = self.minify_c(opts, doc)
        if want.error is not None:
            if status == STATUS_INVALID:
                return None
            return Mismatch(opts, doc, b"", got, "oracle error %r, driver %s" % (want.error, note or "ok"))
        if status == 0 and got == want.out:
            return None
        return Mismatch(opts, doc, want.out, got, note)

    def mismatches(self, opts, docs):
        wants = self.oracle_many(opts, docs)
        self.skipped += sum(1 for w in wants if w.skip)
        results = self.pool.map(lambda dw: self.compare(opts, dw[0], dw[1]), zip(docs, wants))
        return [m for m in results if m is not None]

    def still_fails(self, opts, doc):
        return self.compare(opts, doc, self.oracle_one(opts, doc)) is not None

    def shrink(self, opts, doc):
        doc = self._ddmin(opts, doc, doc.splitlines(True))
        return self._ddmin(opts, doc, [doc[i:i + 1] for i in range(len(doc))])

    def _ddmin(self, opts, doc, units):
        n = 2
        while len(units) >= 2:
            size = max(1, len(units) // n)
            chunks = [units[i:i + size] for i in range(0, len(units), size)]
            reduced = False
            for i in range(len(chunks)):
                rest = [u for j, c in enumerate(chunks) if j != i for u in c]
                cand = b"".join(rest)
                if cand != doc and self.still_fails(opts, cand):
                    units, doc, reduced = rest, cand, True
                    n = max(2, n - 1)
                    break
            if not reduced:
                if n >= len(units):
                    break
                n = min(len(units), n * 2)
        return doc


def show(b, limit=200):
    r = repr(b)
    return r if len(r) <= limit else r[:limit] + "..."


def case_dir(save, m):
    digest = hashlib.sha1(m.opts.name.encode() + b"\0" + m.doc).hexdigest()[:12]
    return os.path.join(save, "%s-%s" % (m.opts.name, digest))


def save_case(save, m, seed):
    d = case_dir(save, m)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "input"), "wb") as f:
        f.write(m.doc)
    with open(os.path.join(d, "expected"), "wb") as f:
        f.write(m.want)
    with open(os.path.join(d, "actual"), "wb") as f:
        f.write(m.got if m.got is not None else m.note.encode())
    meta = {"set": m.opts.name, "driver_args": m.opts.driver_args(), "seed": seed, "note": m.note,
            "oracle_type": m.opts.typ, "oracle_options": m.opts.options}
    with open(os.path.join(d, "case.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
        f.write("\n")
    return d


def replay(driver, save):
    failures = 0
    dirs = sorted(d for d in os.listdir(save) if os.path.isdir(os.path.join(save, d)))
    for name in dirs:
        d = os.path.join(save, name)
        with open(os.path.join(d, "case.json"), encoding="utf-8") as f:
            meta = json.load(f)
        with open(os.path.join(d, "input"), "rb") as f:
            doc = f.read()
        with open(os.path.join(d, "expected"), "rb") as f:
            want = f.read()
        rc, got, err = run([driver] + meta["driver_args"], doc, 30)
        if rc == 0 and got == want:
            print("ok   %s" % name)
        else:
            failures += 1
            print("FAIL %s (exit %s)\n  want: %s\n  got:  %s %s" % (name, rc, show(want), show(got), show(err, 80)))
    print("%d cases, %d failures" % (len(dirs), failures))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description="differential fuzzing of c-minify against the minify oracle")
    ap.add_argument("--oracle", help="the oracle binary (needs Go to build; see the Makefile)")
    ap.add_argument("--driver", required=True, help="tools/minify_run built by the Makefile")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=2000, help="documents per option set")
    ap.add_argument("--sets", default=",".join(s.name for s in SETS), help="comma-separated option set names")
    ap.add_argument("--save", help="directory for minimised mismatches")
    ap.add_argument("--replay", help="rerun the cases saved under this directory and exit")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--shrink-max", type=int, default=20, help="mismatches to minimise per option set")
    args = ap.parse_args()

    if args.replay:
        return replay(args.driver, args.replay)
    if not args.oracle:
        ap.error("--oracle is required unless --replay is given")
    runner = Runner(args.oracle, args.driver, args.jobs)
    total_docs = total_bad = 0
    saved = []
    for name in args.sets.split(","):
        opts = SET_BY_NAME[name]
        rng = random.Random("%d:%s" % (args.seed, name))
        docs = [generate(rng, opts.html) for _ in range(args.count)]
        before = runner.skipped
        bad = runner.mismatches(opts, docs)
        total_docs += len(docs)
        total_bad += len(bad)
        print("%-16s seed %d: %d documents, %d skipped (unported minifier), %d mismatches" %
              (name, args.seed, len(docs), runner.skipped - before, len(bad)), flush=True)
        seen = set()
        for m in bad[:args.shrink_max]:
            small = runner.shrink(opts, m.doc)
            if small in seen:
                continue
            seen.add(small)
            want = runner.oracle_one(opts, small)
            status, got, note = runner.minify_c(opts, small)
            mm = Mismatch(opts, small, want.out if want.out is not None else b"", got, note or m.note)
            print("  mismatch %s\n    input: %s\n    want:  %s\n    got:   %s %s" %
                  (name, show(small), show(mm.want), show(got), mm.note), flush=True)
            if args.save:
                saved.append(save_case(args.save, mm, args.seed))
    if saved:
        print("saved %d cases under %s" % (len(saved), args.save))
    print("total: %d documents, %d skipped, %d mismatches" % (total_docs, runner.skipped, total_bad))
    return 1 if total_bad else 0


if __name__ == "__main__":
    sys.exit(main())
