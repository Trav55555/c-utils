#!/usr/bin/env python3
"""Differential fuzzing of c-mark against the goldmark oracle.

Generates random Markdown documents from a seed (a token soup weighted
toward Markdown syntax: emphasis runs, brackets and links, backticks,
tables, list markers, indentation and tabs, footnotes, definition lists,
autolink candidates, typographer punctuation, raw HTML, entities, non-ASCII,
invalid UTF-8, NUL and CRLF) and renders each with the C driver
(tools/mark_render.c) and the Go oracle (c-mark/oracle) under several option
sets. Every byte must agree.

The oracle's stdin mode takes only its -hugo/-unsafe flags, so the Hugo
sets run one oracle process per document on raw bytes, including invalid
UTF-8. The other sets go through the oracle's -cases JSON mode in one
process; JSON cannot carry invalid UTF-8 (Go rewrites it to U+FFFD), so
those sets draw only from the valid-UTF-8 tokens, and the driver's bytes
are compared after the same rewriting when goldmark's output itself held
invalid UTF-8 (which the Hugo sets then check byte for byte).

goldmark panics on a few documents (table.go reading past the source's
capacity, linkParser.Parse peeking past a block); those count as oracle
errors, not mismatches, and are listed.

A mismatch is shrunk (lines first, then bytes, keeping the mismatch) and
saved under --save as a directory per case: input.md, expected.html
(the oracle's bytes), actual.html and case.json. --replay reruns saved
cases through the driver alone.

    python3 -I tools/diff_fuzz.py --oracle BIN --driver BIN [--seed S]
        [--count N] [--sets a,b] [--save DIR] [--jobs J]
    python3 -I tools/diff_fuzz.py --driver BIN --replay DIR

Standard library only. Exit 0 when every document agrees, 1 otherwise.
"""
import argparse
import codecs
import concurrent.futures
import hashlib
import json
import os
import random
import subprocess
import sys
import tempfile

# CU_MARK_* flag bits, as include/cu_mark.h defines them.
FLAG_BITS = {
    "table": 0x0001, "strikethrough": 0x0002, "linkify": 0x0004, "taskList": 0x0008,
    "typographer": 0x0010, "definitionList": 0x0020, "footnote": 0x0040,
    "autoHeadingID": 0x0080, "attribute": 0x0100, "hugoIDs": 0x0200,
    "unsafe": 0x1000, "xhtml": 0x2000, "hardWraps": 0x4000,
}
ALL_EXTENSIONS = ["table", "strikethrough", "linkify", "taskList", "typographer",
                  "definitionList", "footnote", "autoHeadingID", "attribute"]


class OptionSet:
    """One goldmark configuration and how each side is told about it.

    oracle_args: the oracle's stdin-mode flags (raw bytes allowed), or None
    when the set goes through -cases with `options`."""

    def __init__(self, name, oracle_args=None, options=None):
        self.name = name
        self.oracle_args = oracle_args
        self.options = options
        self.raw = oracle_args is not None

    def driver_args(self):
        if self.oracle_args is not None:
            return list(self.oracle_args)
        flags = 0
        for key, bit in FLAG_BITS.items():
            if self.options.get(key):
                flags |= bit
        args = ["-flags", "0x%x" % flags]
        if self.options.get("tableAlign"):
            args += ["-align", self.options["tableAlign"]]
        return args


def _opts(*names, **extra):
    o = {n: True for n in names}
    o.update(extra)
    return o


SETS = [
    OptionSet("hugo-unsafe", oracle_args=["-hugo", "-unsafe"]),
    OptionSet("hugo", oracle_args=["-hugo"]),
    OptionSet("commonmark", options={}),
    OptionSet("xhtml", options=_opts("xhtml", "unsafe")),
    OptionSet("hardwraps", options=_opts("hardWraps", "unsafe")),
    OptionSet("table-default", options=_opts("table", tableAlign="default")),
    OptionSet("table-attribute", options=_opts("table", tableAlign="attribute")),
    OptionSet("table-style", options=_opts("table", tableAlign="style")),
    OptionSet("table-none", options=_opts("table", tableAlign="none")),
    OptionSet("table-xhtml", options=_opts("table", "xhtml", tableAlign="attribute")),
    OptionSet("goldmark-ids", options=_opts(*ALL_EXTENSIONS, "unsafe", "xhtml", "hardWraps")),
]
SET_BY_NAME = {s.name: s for s in SETS}

# (weight, token). Tokens that start with a newline begin a line with a
# block construct, so documents get several lines of structure.
TOKENS = [
    # words and space
    (12, b"a"), (8, b"b"), (6, b"foo"), (4, b"x"), (4, b"0"), (3, b"9"), (3, b"1"),
    (14, b" "), (5, b"  "), (12, b"\n"), (8, b"\n\n"), (3, b"\r\n"), (1, b"\r"),
    # emphasis and strikethrough runs
    (6, b"*"), (5, b"**"), (3, b"***"), (5, b"_"), (3, b"__"), (4, b"~~"), (3, b"~"), (1, b"*" * 8),
    # brackets, links, images, references
    (6, b"["), (6, b"]"), (4, b"("), (4, b")"), (2, b"[a](b)"), (2, b"[a](<b c> 'd')"), (2, b"!["),
    (2, b"[x]: /u 't'"), (2, b"[x]"), (2, b"[x][]"), (2, b"[[["), (2, b"]]]"), (2, b"<"), (2, b">"),
    (2, b"<http://a.b>"), (1, b"<x@y.z>"),
    # code
    (5, b"`"), (3, b"``"), (3, b"```"), (2, b"\n```\n"), (2, b"\n~~~\n"), (1, b"\n```c {lang=x}\n"),
    # tables
    (6, b"|"), (3, b"\n|---|---|\n"), (2, b":-:"), (2, b"--:"), (2, b":--"), (2, b"-|-"), (2, b"\\|"),
    (2, b"\n| a | b |\n"),
    # headings and setext, thematic breaks
    (3, b"\n# "), (2, b"\n## "), (2, b"\n==="), (3, b"\n---"), (2, b"\n***\n"), (1, b"########"),
    # lists, quotes, indentation
    (5, b"\n- "), (3, b"\n* "), (3, b"\n+ "), (4, b"\n1. "), (2, b"\n2) "), (2, b"\n- - - "), (5, b"\n> "),
    (2, b">>>>>"), (4, b"\n    "), (4, b"\t"), (3, b"\n  "), (2, b"\n   "), (2, b"\n\t"),
    # task lists, footnotes, definition lists
    (3, b"[ ]"), (3, b"[x]"), (2, b"\n- [ ] "), (3, b"[^1]"), (2, b"[^a b]"), (3, b"\n[^1]: "),
    (2, b"\n    "), (4, b"\n: "), (2, b":"),
    # autolink candidates
    (3, b"www.a.b"), (3, b"http://x.y/z"), (2, b"https://e.f?g=h#i"), (2, b"ftp://a.b:21/c"),
    (2, b"a@b.cd"), (1, b"www."), (1, b"http://"), (1, b"x@y"),
    # typographer punctuation
    (5, b"'"), (5, b"\""), (4, b"--"), (3, b"---"), (3, b"..."), (2, b".."), (2, b"<<"), (2, b">>"),
    (2, b"'90s"), (2, b"I'm"), (2, b"'twas"), (3, b"."), (3, b","), (2, b"!"), (2, b"?"), (2, b";"),
    # raw HTML
    (3, b"<b>"), (3, b"</b>"), (2, b"\n<div>\n"), (2, b"\n</div>\n"), (2, b"<!-- c -->"), (1, b"<?x ?>"),
    (1, b"<![CDATA[x]]>"), (2, b"<a href='x'>"), (1, b"<br/>"), (1, b"<!A>"), (1, b"\n<pre>\n"),
    # entities and escapes
    (3, b"&amp;"), (2, b"&nbsp;"), (2, b"&#35;"), (2, b"&#x41;"), (2, b"&copy;"), (2, b"&bogus;"),
    (3, b"&"), (3, b"\\"), (2, b"\\*"), (2, b"\\\\"), (1, b"&#0;"), (1, b"&#1234567;"),
    # attributes
    (3, b"{#id}"), (3, b"{.c}"), (2, b"{a=b}"), (2, b"{"), (2, b"}"), (1, b'{a="b c"}'),
    # non-ASCII
    (3, "é".encode()), (2, "中文".encode()), (2, " ".encode()), (1, " ".encode()),
    (2, "\U0001F600".encode()), (1, "ǅ".encode()), (1, "Σς".encode()), (1, "　".encode()),
    # control characters
    (2, b"\x00"), (1, b"\x0b"), (1, b"\x0c"), (1, b"\x1f"),
    # misc
    (2, b"$"), (2, b"/"), (2, b"#"), (2, b"="), (2, b"+"), (1, b"%20"), (1, b"%"),
]
# Only for the raw (stdin) sets: invalid UTF-8.
RAW_TOKENS = [
    (3, b"\xff"), (2, b"\xc3"), (2, b"\xe2\x82"), (1, b"\xed\xa0\x80"), (1, b"\xf4\x90\x80\x80"), (1, b"\x80"),
]


def generate(rng, raw):
    tokens = TOKENS + (RAW_TOKENS if raw else [])
    weights = [w for w, _ in tokens]
    values = [t for _, t in tokens]
    n = rng.randint(1, 60) if rng.random() < 0.9 else rng.randint(60, 240)
    return b"".join(rng.choices(values, weights, k=n))


class Mismatch:
    def __init__(self, opts, doc, want, got, note):
        self.opts = opts
        self.doc = doc
        self.want = want
        self.got = got
        self.note = note


def _one_byte_fffd(err):
    """Error handler that replaces exactly one byte, as Go's encoding/json
    does with invalid UTF-8 (Python's own handler takes the maximal invalid
    subpart)."""
    return "�", err.start + 1


codecs.register_error("go-json", _one_byte_fffd)


def go_json_text(b):
    """b as Go's encoding/json would have written it: UTF-8 with every
    invalid byte replaced by U+FFFD."""
    return b.decode("utf-8", "go-json").encode("utf-8")


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
        self.oracle_errors = []

    def render_c(self, opts, doc):
        """The driver's bytes, or None with a note when it did not render."""
        rc, out, err = run([self.driver] + opts.driver_args(), doc, 30)
        if rc != 0:
            return None, "driver exit %s: %s" % (rc, err.decode("utf-8", "replace").strip())
        return out, ""

    def oracle_one(self, opts, doc):
        """The oracle's bytes for one document, or None on an oracle error."""
        if opts.raw:
            rc, out, err = run([self.oracle] + opts.oracle_args, doc, 60)
            if rc != 0:
                self.oracle_errors.append((opts, doc, err))
                return None
            return out
        return self.oracle_many(opts, [doc])[0]

    def oracle_many(self, opts, docs):
        """The oracle's bytes for valid-UTF-8 documents through -cases. A
        document goldmark panics on takes the whole batch down, so a failed
        batch is bisected until the panicking documents stand alone."""
        cases = [{"options": opts.options, "markdown": d.decode("utf-8")} for d in docs]
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "cases.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump(cases, f, ensure_ascii=True)
            rc, out, err = run([self.oracle, "-cases", path], b"", 600)
        if rc == 0:
            return [s.encode("utf-8") for s in json.loads(out.decode("utf-8"))]
        if len(docs) == 1:
            self.oracle_errors.append((opts, docs[0], err))
            return [None]
        half = len(docs) // 2
        return self.oracle_many(opts, docs[:half]) + self.oracle_many(opts, docs[half:])

    def compare(self, opts, doc, want):
        """A Mismatch for doc, or None when the driver agrees with want. For
        a set that went through JSON, output goldmark wrote with invalid
        UTF-8 (a tab-split table row that starts inside a multibyte
        character) reached us with each bad byte rewritten as U+FFFD, so
        the driver's bytes are compared after the same rewriting."""
        if want is None:
            return None
        got, note = self.render_c(opts, doc)
        if got == want:
            return None
        if not note and not opts.raw and go_json_text(got) == want:
            return None
        return Mismatch(opts, doc, want, got, note)

    def mismatches(self, opts, docs):
        if opts.raw:
            wants = list(self.pool.map(lambda d: self.oracle_one(opts, d), docs))
        else:
            wants = self.oracle_many(opts, docs)
        results = self.pool.map(lambda dw: self.compare(opts, dw[0], dw[1]), zip(docs, wants))
        return [m for m in results if m is not None]

    def still_fails(self, opts, doc):
        return self.compare(opts, doc, self.oracle_one(opts, doc)) is not None

    def shrink(self, opts, doc):
        """Removes lines, then bytes (characters for a set that goes through
        JSON, which must stay valid UTF-8), while the mismatch persists."""
        doc = self._ddmin(opts, doc, doc.splitlines(True))
        if opts.raw:
            units = [doc[i:i + 1] for i in range(len(doc))]
        else:
            units = [c.encode("utf-8") for c in doc.decode("utf-8")]
        return self._ddmin(opts, doc, units)

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
    with open(os.path.join(d, "input.md"), "wb") as f:
        f.write(m.doc)
    with open(os.path.join(d, "expected.html"), "wb") as f:
        f.write(m.want)
    with open(os.path.join(d, "actual.html"), "wb") as f:
        f.write(m.got if m.got is not None else m.note.encode())
    meta = {"set": m.opts.name, "driver_args": m.opts.driver_args(), "seed": seed, "note": m.note}
    if m.opts.raw:
        meta["oracle_args"] = m.opts.oracle_args
    else:
        meta["oracle_options"] = m.opts.options
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
        with open(os.path.join(d, "input.md"), "rb") as f:
            doc = f.read()
        with open(os.path.join(d, "expected.html"), "rb") as f:
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
    ap = argparse.ArgumentParser(description="differential fuzzing of c-mark against the goldmark oracle")
    ap.add_argument("--oracle", help="the oracle binary (needs Go to build; see the Makefile)")
    ap.add_argument("--driver", required=True, help="tools/mark_render built by the Makefile")
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
        docs = [generate(rng, opts.raw) for _ in range(args.count)]
        bad = runner.mismatches(opts, docs)
        total_docs += len(docs)
        total_bad += len(bad)
        print("%-16s seed %d: %d documents, %d mismatches" % (name, args.seed, len(docs), len(bad)), flush=True)
        seen = set()
        for m in bad[:args.shrink_max]:
            small = runner.shrink(opts, m.doc)
            if small in seen:
                continue
            seen.add(small)
            want = runner.oracle_one(opts, small)
            got, note = runner.render_c(opts, small)
            mm = Mismatch(opts, small, want, got, note)
            print("  mismatch %s\n    input: %s\n    want:  %s\n    got:   %s %s" %
                  (name, show(small), show(want), show(got), note), flush=True)
            if args.save:
                saved.append(save_case(args.save, mm, args.seed))
    for opts, doc, err in runner.oracle_errors[:5]:
        print("oracle error under %s on %s: %s" % (opts.name, show(doc), show(err, 300)))
    if saved:
        print("saved %d cases under %s" % (len(saved), args.save))
    print("total: %d documents, %d mismatches, %d oracle errors" %
          (total_docs, total_bad, len(runner.oracle_errors)))
    return 1 if total_bad or runner.oracle_errors else 0


if __name__ == "__main__":
    sys.exit(main())
