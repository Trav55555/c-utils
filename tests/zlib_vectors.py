"""Writes tests/zlib_vectors.h and fuzz/fuzz_zlib.seeds/ from Python's zlib.

Run from the repository root: python3 -I tests/zlib_vectors.py, or with
--check to compare what it would write with what is there, writing nothing.

Every expectation in the header is zlib's own verdict, so tests/test_zlib.c
checks cu_zlib_inflate against zlib without linking it:

- streams zlib wrote at every level and strategy, from inputs the test
  rebuilds by the same rules as make_input below;
- one-byte mutations of some of them, with zlib's verdict on each;
- streams built bit by bit here to reach the rules about Huffman codes,
  lengths and distances that zlib's own writer never breaks.
"""

import hashlib
import os
import random
import sys
import zlib

ZEROS, TEXT, RANDOM, RUNS, FAR = range(5)
WORDS = [b"the", b"agent", b"commit", b"tree", b"blob", b"merge", b"pack", b"delta",
         b"zlib", b"git"]


class Lcg:
    def __init__(self, seed):
        self.s = seed

    def next(self):
        self.s = (self.s * 1103515245 + 12345) & 0x7FFFFFFF
        return self.s >> 16


def make_input(kind, n):
    """Mirrors make_input in tests/test_zlib.c."""
    g = Lcg(n + kind)
    out = bytearray()
    if kind == ZEROS:
        out = bytearray(n)
    elif kind == TEXT:
        while len(out) < n:
            out += WORDS[g.next() % 10]
            out.append(ord("\n") if g.next() % 8 == 0 else ord(" "))
    elif kind == RANDOM:
        while len(out) < n:
            out.append(g.next() & 0xFF)
    elif kind == RUNS:
        while len(out) < n:
            run = 1 + g.next() % 300
            out += bytes([g.next() % 4]) * run
    elif kind == FAR:
        head = bytearray()
        while len(head) < 300:
            head.append(g.next() & 0xFF)
        out = head + bytearray(n - 600) + head
    return bytes(out[:n])


INPUTS = [(ZEROS, 0), (TEXT, 1), (TEXT, 64), (RANDOM, 64), (RUNS, 300), (TEXT, 3000),
          (RANDOM, 600), (RUNS, 5000), (ZEROS, 70000), (FAR, 30300), (TEXT, 20000)]
# Inputs this short are also checked under Cerberus.
SMALL = 64


def compress(data, level=6, wbits=15, mem=8, strategy=zlib.Z_DEFAULT_STRATEGY, flush=None):
    c = zlib.compressobj(level, zlib.DEFLATED, wbits, mem, strategy)
    if flush is None:
        return c.compress(data) + c.flush()
    half = len(data) // 2
    return c.compress(data[:half]) + c.flush(flush) + c.compress(data[half:]) + c.flush()


def configs(kind, n):
    """(level shown to the test, keyword arguments) for an input. Short
    inputs get every level; long ones fewer, to keep the header small."""
    if n > 10000 and kind == TEXT:
        return [(1, dict(level=1)), (9, dict(level=9))]
    levels = range(10) if n <= 1000 else (0, 1, 6, 9)
    out = [(level, dict(level=level)) for level in levels if level != 0 or n <= 5000]
    for strategy in (zlib.Z_FILTERED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE, zlib.Z_FIXED):
        out.append((-1, dict(strategy=strategy)))
    out.append((-1, dict(level=9, wbits=9)))
    if n <= 1000:
        out.append((-1, dict(level=9, wbits=12)))
    out.append((-1, dict(mem=1)))
    out.append((-1, dict(flush=zlib.Z_SYNC_FLUSH)))
    if n <= 1000:
        out.append((-1, dict(flush=zlib.Z_FULL_FLUSH)))
    return out


def inflate(z):
    """zlib's verdict: (data, bytes used) or None."""
    d = zlib.decompressobj()
    try:
        data = d.decompress(z)
    except zlib.error:
        return None
    if not d.eof:
        return None
    return data, len(z) - len(d.unused_data)


class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def bits(self, value, count):
        """count bits of value, lowest first, as deflate packs numbers."""
        for i in range(count):
            self.acc |= (value >> i & 1) << self.n
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc = 0
                self.n = 0

    def code(self, value, length):
        """A Huffman code, first bit highest."""
        for i in range(length - 1, -1, -1):
            self.bits(value >> i & 1, 1)

    def finish(self):
        if self.n:
            self.out.append(self.acc)
            self.acc = 0
            self.n = 0
        return bytes(self.out)


def canonical(lengths):
    """Symbol -> (code, length), as RFC 1951 section 3.2.2 assigns them."""
    counts = [0] * 16
    for n in lengths:
        if n:
            counts[n] += 1
    code = 0
    nxt = [0] * 16
    for bits in range(1, 16):
        code = (code + counts[bits - 1]) << 1
        nxt[bits] = code
    out = {}
    for sym, n in enumerate(lengths):
        if n:
            out[sym] = (nxt[n], n)
            nxt[n] += 1
    return out


def wrap(body, data):
    """A zlib stream around deflate bytes body whose data is data."""
    return b"\x78\x01" + body + zlib.adler32(data).to_bytes(4, "big")


ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]
# A complete code-length code: 0..15 in four bits each, no repeat codes.
FLAT = [4] * 16 + [0, 0, 0]
# Another, with the repeat codes: 0..15 in five bits, 16 in two, 17 and 18 in three.
WITH_REPEATS = [5] * 16 + [2, 3, 3]


def dynamic(w, lit, dist, clen=FLAT, seq=None, hlit=None, hdist=None, final=1):
    """Writes a dynamic block header. seq, if given, is the code-length
    sequence as (symbol, extra value, extra bits); else each length is
    written plainly. Returns the literal/length and distance codes."""
    w.bits(final, 1)
    w.bits(2, 2)
    w.bits((len(lit) - 257) if hlit is None else hlit, 5)
    w.bits((len(dist) - 1) if hdist is None else hdist, 5)
    w.bits(19 - 4, 4)
    for sym in ORDER:
        w.bits(clen[sym], 3)
    cl = canonical(clen)
    if seq is None:
        seq = [(n, 0, 0) for n in lit + dist]
    for sym, extra, nbits in seq:
        w.code(*cl[sym])
        w.bits(extra, nbits)
    return canonical(lit), canonical(dist)


def crafted():
    """(name, stream, whether zlib should accept it)."""
    cases = []

    def lit_lengths(used, n=257):
        lens = [0] * n
        for sym, length in used.items():
            lens[sym] = length
        return lens

    # Literals only, with no distance codes at all.
    w = BitWriter()
    lit = lit_lengths({ord("h"): 2, ord("i"): 2, 256: 1})
    lc, _ = dynamic(w, lit, [0])
    for c in b"hi":
        w.code(*lc[c])
    w.code(*lc[256])
    cases.append(("no distance codes", wrap(w.finish(), b"hi"), True))

    # One one-bit distance code, used for a match of length 3 at distance 1.
    w = BitWriter()
    lit = lit_lengths({ord("a"): 2, 257: 2, 256: 1}, 258)
    lc, dc = dynamic(w, lit, [1])
    w.code(*lc[ord("a")])
    w.code(*lc[257])
    w.code(*dc[0])
    w.code(*lc[256])
    cases.append(("one distance code", wrap(w.finish(), b"aaaa"), True))

    # The same with the distance code's other, unused bit string.
    w = BitWriter()
    lc, dc = dynamic(w, lit, [1])
    w.code(*lc[ord("a")])
    w.code(*lc[257])
    w.code(1, 1)
    w.code(*lc[256])
    cases.append(("missing distance code", wrap(w.finish(), b"aaaa"), False))

    # Only an end-of-block code, one bit long.
    w = BitWriter()
    lc, _ = dynamic(w, lit_lengths({256: 1}), [0])
    w.code(*lc[256])
    cases.append(("only end of block", wrap(w.finish(), b""), True))

    def block(lit_used, dist, name, accept=False, data=b"", **kw):
        """A dynamic block holding only an end of block, which is invalid
        only where its header is."""
        w = BitWriter()
        lens = lit_lengths(lit_used, kw.pop("n", 257))
        lc, _ = dynamic(w, lens, dist, **kw)
        if 256 in lc:
            w.code(*lc[256])
        cases.append((name, wrap(w.finish(), data), accept))

    block({ord("a"): 2, 256: 2}, [0], "incomplete literal code")
    block({ord("a"): 1, ord("b"): 1, 256: 1}, [0], "over-subscribed literal code")
    block({ord("a"): 1, 257: 1}, [0], "no end of block", n=258)
    block({ord("a"): 1, 256: 1}, [2, 2], "incomplete distance code")
    block({ord("a"): 1, 256: 1}, [1, 1, 1], "over-subscribed distance code")
    block({ord("a"): 1, 256: 1}, [1, 1], "complete distance code", accept=True)
    block({ord("a"): 1, 256: 1}, [0] * 30, "30 unused distance codes", accept=True)
    block({ord("a"): 1, 256: 1}, [1], "286 length codes", accept=True, n=286)
    # The header counts past what deflate allows; the lengths are fine.
    block({ord("a"): 1, 256: 1}, [1], "287 length codes", n=287)
    block({ord("a"): 1, 256: 1}, [1], "288 length codes", n=288)
    block({ord("a"): 1, 256: 1}, [1] + [0] * 30, "31 distance codes")
    block({ord("a"): 1, 256: 1}, [1] + [0] * 31, "32 distance codes")
    block({ord("a"): 1, 256: 1}, [1], "incomplete code-length code",
          clen=[4] * 15 + [0] * 4)
    block({ord("a"): 1, 256: 1}, [1], "over-subscribed code-length code",
          clen=[4] * 17 + [0] * 2)

    # Repeat codes, over 257 literal/length lengths and the distance lengths
    # after them, as one sequence. Here 'a' (97) and 255 are 2 bits, 256 one
    # bit, and the one distance code one bit:
    # 97 zeros (18, 11 + 86), 2, 157 zeros (18 with 138, 17 with 10, 17
    # with 9), then 2, 1, 1.
    zeros = [(18, 86, 7), (2, 0, 0), (18, 127, 7), (17, 7, 3), (17, 6, 3)]
    lit = lit_lengths({ord("a"): 2, 255: 2, 256: 1})

    def repeats(seq, dist, accept, name):
        w = BitWriter()
        lc, dc = dynamic(w, lit, dist, clen=WITH_REPEATS, seq=seq)
        w.code(*lc[ord("a")])
        w.code(*lc[256])
        cases.append((name, wrap(w.finish(), b"a"), accept))

    plain = zeros + [(2, 0, 0), (1, 0, 0), (1, 0, 0)]
    repeats(plain, [1], True, "zero runs")
    repeats([(16, 0, 2)] + plain, [1], False, "repeat with nothing before")
    repeats(zeros + [(2, 0, 0), (1, 0, 0), (16, 0, 2)], [1], False, "repeat past the end")
    repeats(zeros + [(2, 0, 0), (1, 0, 0), (17, 0, 3)], [1], False, "zeros past the end")
    repeats(zeros[:3] + [(18, 127, 7)], [1], False, "long zeros past the end")
    # 16 copies 256's 1 into all three distance lengths: a run may cross
    # from one code into the next. Three one-bit codes are over-subscribed,
    # so the next case makes the copied length 2 instead.
    repeats(zeros + [(2, 0, 0), (1, 0, 0), (16, 0, 2)], [1, 1, 1], False,
            "run across into distances, over-subscribed")
    lit2 = lit_lengths({ord("a"): 1, 255: 2, 256: 2})
    w = BitWriter()
    seq = zeros[:1] + [(1, 0, 0)] + zeros[2:] + [(2, 0, 0), (16, 0, 2), (1, 0, 0)]
    lc, dc = dynamic(w, lit2, [2, 2, 1], clen=WITH_REPEATS, seq=seq)
    w.code(*lc[ord("a")])
    w.code(*lc[256])
    cases.append(("run across into distances", wrap(w.finish(), b"a"), True))

    # The most lengths a header can ask for, 286 + 30 = 316, with runs that
    # end exactly at the last one or overshoot it by one. 'a' and 256 are
    # one bit each, and so is the first distance code; the 29 lengths left
    # are zeros.
    full = [(18, 86, 7), (1, 0, 0), (18, 127, 7), (18, 9, 7), (1, 0, 0), (18, 18, 7),
            (1, 0, 0)]
    lit286 = lit_lengths({ord("a"): 1, 256: 1}, 286)
    for tail, accept, name in (
            ([(18, 18, 7)], True, "18 ending at length 316"),
            ([(18, 19, 7)], False, "18 one past length 316"),
            ([(18, 9, 7), (17, 6, 3)], True, "17 ending at length 316"),
            ([(18, 9, 7), (17, 7, 3)], False, "17 one past length 316"),
            ([(18, 14, 7), (0, 0, 0), (16, 0, 2)], True, "16 ending at length 316"),
            ([(18, 15, 7), (0, 0, 0), (16, 0, 2)], False, "16 one past length 316")):
        w = BitWriter()
        lc, dc = dynamic(w, lit286, [1] + [0] * 29, clen=WITH_REPEATS, seq=full + tail)
        w.code(*lc[ord("a")])
        w.code(*lc[256])
        cases.append((name, wrap(w.finish(), b"a"), accept))

    # Fixed codes.
    fixed_lit = canonical([8] * 144 + [9] * 112 + [7] * 24 + [8] * 8)
    fixed_dist = canonical([5] * 32)

    def fixed(symbols, data, accept, name):
        w = BitWriter()
        w.bits(1, 1)
        w.bits(1, 2)
        for kind, value, extra, nbits in symbols:
            w.code(*(fixed_lit if kind == "l" else fixed_dist)[value])
            w.bits(extra, nbits)
        w.code(*fixed_lit[256])
        cases.append((name, wrap(w.finish(), data), accept))

    a = ("l", ord("a"), 0, 0)
    fixed([a, ("l", 286, 0, 0), ("d", 0, 0, 0)], b"a", False, "length code 286")
    fixed([a, ("l", 287, 0, 0), ("d", 0, 0, 0)], b"a", False, "length code 287")
    fixed([a, ("l", 257, 0, 0), ("d", 30, 0, 0)], b"aaaa", False, "distance code 30")
    fixed([a, ("l", 257, 0, 0), ("d", 31, 0, 0)], b"aaaa", False, "distance code 31")
    fixed([a, ("l", 257, 0, 0), ("d", 1, 0, 0)], b"aaaa", False, "distance too far back")
    fixed([("l", 257, 0, 0), ("d", 0, 0, 0)], b"", False, "match before any data")
    fixed([a, ("l", ord("b"), 0, 0), ("l", 257, 0, 0), ("d", 1, 0, 0)], b"ababa", True,
          "distance to the start")
    fixed([a, ("l", 285, 0, 0), ("d", 0, 0, 0)], b"a" * 259, True, "length 258")
    fixed([a, ("l", 284, 31, 5), ("d", 0, 0, 0)], b"a" * 259, True, "length 258 from code 284")
    fixed([a, ("l", 284, 30, 5), ("d", 0, 0, 0)], b"a" * 258, True, "length 257")
    fixed([], b"", True, "empty fixed block")
    big = [("l", c, 0, 0) for c in b"abcd"]
    fixed(big + [("l", 266, 1, 1), ("d", 4, 1, 1)], b"abcd" + (b"abcd" * 4)[2:16],
          False, "extra bits on distance code 4 too far")
    fixed(big + [("l", 266, 1, 1), ("d", 3, 0, 0)], b"abcd" + (b"abcd" * 5)[:14], True,
          "length 14 at distance 4")

    # Block types and stored blocks.
    w = BitWriter()
    w.bits(1, 1)
    w.bits(3, 2)
    cases.append(("block type 3", wrap(w.finish(), b""), False))
    cases.append(("stored", wrap(b"\x01\x02\x00\xfd\xffhi", b"hi"), True))
    cases.append(("stored, bad complement", wrap(b"\x01\x02\x00\xfd\xfehi", b"hi"), False))
    cases.append(("stored, empty", wrap(b"\x01\x00\x00\xff\xff", b""), True))
    cases.append(("stored, cut short", wrap(b"\x01\x03\x00\xfc\xffhi", b"hi"), False))
    # Bits after the block header are padding, whatever their value.
    cases.append(("stored, padding set", wrap(b"\xf9\x02\x00\xfd\xffhi", b"hi"), True))
    # A fixed block, then a stored one starting mid-byte.
    w = BitWriter()
    w.bits(0, 1)
    w.bits(1, 2)
    w.code(*fixed_lit[ord("x")])
    w.code(*fixed_lit[256])
    w.bits(1, 1)
    w.bits(0, 2)
    body = w.finish() + b"\x01\x00\xfe\xffy"
    cases.append(("fixed then stored", wrap(body, b"xy"), True))
    cases.append(("no final block", wrap(b"\x00\x00\x00\xff\xff", b""), False))

    # Headers and trailers.
    good = zlib.compress(b"hello", 9)
    cases.append(("level 9 header", good, True))
    cases.append(("bad check bits", bytes([0x78, 0xda ^ 1]) + good[2:], False))
    for cmf in (0x08, 0x18, 0x28, 0x38, 0x48, 0x58, 0x68, 0x78, 0x88, 0x77):
        flg = (31 - (cmf * 256) % 31) % 31
        cases.append(("header %02x%02x" % (cmf, flg), bytes([cmf, flg]) + good[2:],
                      cmf & 0x0F == 8 and cmf >> 4 <= 7))
    # FDICT, with the check bits still right.
    flg = 0x20 | (31 - (0x78 * 256 + 0x20) % 31) % 31
    cases.append(("preset dictionary", bytes([0x78, flg]) + good[2:], False))
    cases.append(("bad Adler-32", good[:-1] + bytes([good[-1] ^ 1]), False))
    cases.append(("trailing bytes", good + b"next", True))
    cases.append(("one byte", b"\x78", False))
    cases.append(("nothing", b"", False))
    return cases


def c_bytes(name, data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join("%d" % b for b in data[i:i + 24]) + ",")
    if not data:
        # An empty initializer is not C89, and a lone zero is no stream.
        return "static const unsigned char %s[] = \"\";\n" % name
    return "static const unsigned char %s[] = {\n%s\n};\n" % (name, "\n".join(lines))


def c_str(s):
    return '"' + "".join(c if c.isalnum() or c in " -,." else "\\%03o" % ord(c) for c in s) + '"'


def main(check):
    out = ['/* Made by tests/zlib_vectors.py from zlib %s; do not edit. */\n' %
           zlib.ZLIB_RUNTIME_VERSION]
    valid = []  # (input index, level, stream)
    for index, (kind, n) in enumerate(INPUTS):
        seen = set()
        for level, kw in configs(kind, n):
            z = compress(make_input(kind, n), **kw)
            if z in seen:
                continue
            seen.add(z)
            got = inflate(z)
            assert got == (make_input(kind, n), len(z))
            valid.append((index, level, z))

    def emit(rows, small):
        body = []
        for i, (index, level, z) in rows:
            body.append(c_bytes("z%d" % i, z))
        table = ",\n".join("    {%d, %d, z%d, %d}" % (index, level, i, len(z))
                           for i, (index, level, z) in rows)
        name = "small_streams" if small else "large_streams"
        body.append("static const struct stream %s[] = {\n%s\n};\n" % (name, table))
        return body

    numbered = list(enumerate(valid))
    small = [r for r in numbered if INPUTS[r[1][0]][1] <= SMALL]
    large = [r for r in numbered if INPUTS[r[1][0]][1] > SMALL]
    out.append("struct stream {\n    int input;\n    int level; /* -1: not plain zlib.compress */\n"
               "    const unsigned char *z;\n    size_t len;\n};\n\n")
    out.append("static const struct input {\n    int kind;\n    size_t n;\n} inputs[] = {\n")
    out.append(",\n".join("    {%d, %d}" % kn for kn in INPUTS) + "\n};\n\n")
    out += emit(small, True)
    out.append("\n#ifndef __cerb__\n")
    out += emit(large, False)
    out.append("#endif\n\n")

    # Mutations, each of one stream: flip a bit, set a byte, or cut short.
    # The header is left alone, since zlib would then decode with a smaller
    # window than the one this inflater always allows.
    rng = random.Random(1950)
    bases = [i for i, (index, level, z) in numbered
             if INPUTS[index][1] in (64, 3000, 5000) and (level in (1, 6, 9) or level == -1)
             and z[0] == 0x78]
    mutations = []
    for _ in range(600):
        base = rng.choice(bases)
        z = valid[base][2]
        how = rng.randrange(3)
        at = rng.randrange(2, len(z))
        if how == 0:
            v = rng.randrange(8)
            m = z[:at] + bytes([z[at] ^ (1 << v)]) + z[at + 1:]
        elif how == 1:
            v = rng.randrange(256)
            m = z[:at] + bytes([v]) + z[at + 1:]
        else:
            v = 0
            m = z[:at]
        got = inflate(m)
        if got is None:
            mutations.append((base, how, at, v, 0, 0, 0, ""))
        else:
            data, used = got
            mutations.append((base, how, at, v, 1, used, len(data),
                              hashlib.sha1(data).hexdigest()))
    out.append("/* how: 0 flips bit v of byte at, 1 sets byte at to v, 2 cuts the stream\n"
               " * to at bytes. ok, used, out_len and sha1 are zlib's verdict. */\n")
    out.append("struct mutation {\n    int base;\n    int how;\n    size_t at;\n    unsigned v;\n"
               "    int ok;\n    size_t used;\n    size_t out_len;\n    const char *sha1;\n};\n\n")
    out.append("#ifndef __cerb__\nstatic const struct mutation mutations[] = {\n")
    out.append(",\n".join('    {%d, %d, %d, %d, %d, %d, %d, "%s"}' % m for m in mutations))
    out.append("\n};\n#endif\n\n")
    accepted = sum(1 for m in mutations if m[4])
    print("mutations: %d, zlib accepted %d" % (len(mutations), accepted))

    cases = crafted()
    rows = []
    for i, (name, z, accept) in enumerate(cases):
        got = inflate(z)
        assert (got is not None) == accept, name
        out.append(c_bytes("case%d" % i, z))
        if got is None:
            rows.append('    {%s, case%d, %d, 0, 0, 0, ""}' % (c_str(name), i, len(z)))
        else:
            data, used = got
            rows.append('    {%s, case%d, %d, 1, %d, %d, "%s"}' %
                        (c_str(name), i, len(z), used, len(data),
                         hashlib.sha1(data).hexdigest()))
    out.append("\n/* Streams built bit by bit. ok, used, out_len and sha1 are zlib's\n"
               " * verdict. */\n")
    out.append("struct crafted {\n    const char *name;\n    const unsigned char *z;\n"
               "    size_t len;\n    int ok;\n    size_t used;\n    size_t out_len;\n"
               "    const char *sha1;\n};\n\n")
    out.append("static const struct crafted crafted[] = {\n" + ",\n".join(rows) + "\n};\n")

    files = [(os.path.join("tests", "zlib_vectors.h"), "".join(out).encode())]
    seeds = os.path.join("fuzz", "fuzz_zlib.seeds")
    for name, z in (("text_level1", compress(make_input(TEXT, 300), level=1)),
                    ("text_level9", compress(make_input(TEXT, 300), level=9)),
                    ("runs_fixed", compress(make_input(RUNS, 300), strategy=zlib.Z_FIXED)),
                    ("random_huffman", compress(make_input(RANDOM, 200),
                                                strategy=zlib.Z_HUFFMAN_ONLY)),
                    ("stored", compress(make_input(TEXT, 64), level=0)),
                    ("sync_flush", compress(make_input(TEXT, 300), flush=zlib.Z_SYNC_FLUSH))):
        files.append((os.path.join(seeds, name), z))
    print("%d streams, %d crafted" % (len(valid), len(cases)))
    if check:
        stale = []
        for path, data in files:
            try:
                with open(path, "rb") as f:
                    same = f.read() == data
            except OSError:
                same = False
            if not same:
                stale.append(path)
        for path in stale:
            print("differs: %s" % path)
        return 1 if stale else 0
    os.makedirs(seeds, exist_ok=True)
    for path, data in files:
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
    return 0


if __name__ == "__main__":
    if sys.argv[1:] not in ([], ["--check"]):
        sys.exit("usage: python3 -I tests/zlib_vectors.py [--check]")
    sys.exit(main(sys.argv[1:] == ["--check"]))
