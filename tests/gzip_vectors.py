"""Writes tests/gzip_vectors.h, and gzip seeds in fuzz/fuzz_zlib.seeds/,
from Python's zlib.

Run from the repository root: python3 -I tests/gzip_vectors.py, or with
--check to compare what it would write with what is there, writing nothing.

Every expectation is zlib's own verdict (a gzip-wrapped decompressobj), so
tests/test_gzip.c checks cu_crc32 and cu_gzip_inflate against zlib without
linking it: CRC-32s, members zlib wrote, members with every optional
header field, broken headers and trailers, and one-byte mutations.
"""

import os
import random
import sys
import zlib

HEADER = os.path.join("tests", "gzip_vectors.h")


def inflate(z):
    """zlib's verdict: (data, bytes used) or None."""
    d = zlib.decompressobj(31)
    try:
        data = d.decompress(z)
    except zlib.error:
        return None
    if not d.eof:
        return None
    return data, len(z) - len(d.unused_data)


def text(n, seed):
    rng = random.Random(seed)
    words = [b"want", b"have", b"done", b"ref", b"pack", b"commit", b"tree", b"blob"]
    out = bytearray()
    while len(out) < n:
        out += rng.choice(words) + (b"\n" if rng.randrange(6) == 0 else b" ")
    return bytes(out[:n])


def noise(n, seed):
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(n))


def deflate(data, level=6):
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def member(data, flags=0, extra=None, name=None, comment=None, hcrc=None, level=6,
           crc=None, size=None):
    """A gzip member built field by field."""
    head = bytearray(b"\x1f\x8b\x08")
    head.append(flags)
    head += b"\x00\x00\x00\x00\x00\x03"
    if extra is not None:
        head += len(extra).to_bytes(2, "little") + extra
    if name is not None:
        head += name + b"\x00"
    if comment is not None:
        head += comment + b"\x00"
    if flags & 2:
        head += (zlib.crc32(bytes(head)) & 0xFFFF if hcrc is None else hcrc).to_bytes(2, "little")
    crc = zlib.crc32(data) if crc is None else crc
    size = len(data) & 0xFFFFFFFF if size is None else size
    return bytes(head) + deflate(data, level) + crc.to_bytes(4, "little") + size.to_bytes(4, "little")


def cases():
    """(name, member bytes)."""
    import gzip
    out = []
    datas = [b"", b"hello\n", text(3000, 1), noise(600, 2), b"\0" * 70000]
    for i, data in enumerate(datas):
        for level in (0, 1, 6, 9) if len(data) < 10000 else (1, 6, 9):
            out.append(("zlib level %d, input %d" % (level, i),
                        gzip.compress(data, level, mtime=0)))
    hello = b"0032want 0123456789abcdef0123456789abcdef01234567\n0000"
    out.append(("text flag", member(hello, flags=1)))
    out.append(("name", member(hello, flags=8, name=b"request.bin")))
    out.append(("comment", member(hello, flags=16, comment=b"from git")))
    out.append(("extra", member(hello, flags=4, extra=b"ab\x02\x00cd")))
    out.append(("empty extra", member(hello, flags=4, extra=b"")))
    out.append(("header crc", member(hello, flags=2)))
    out.append(("every field", member(hello, flags=31, extra=b"xy\x00\x00", name=b"n",
                                      comment=b"c")))
    out.append(("bad header crc", member(hello, flags=2, hcrc=0x1234)))
    out.append(("bad data crc", member(hello, crc=zlib.crc32(hello) ^ 1)))
    out.append(("bad size", member(hello, size=len(hello) + 1)))
    out.append(("size off by 2^32", member(hello, size=len(hello) + (1 << 32) - 1 & 0xFFFFFFFF)))
    for bit in (0x20, 0x40, 0x80):
        out.append(("reserved flag %02x" % bit, member(hello, flags=bit)))
    good = member(hello)
    out.append(("bad magic", b"\x1f\x8c" + good[2:]))
    out.append(("method 7", good[:2] + b"\x07" + good[3:]))
    out.append(("name not ended", member(hello, flags=8, name=b"x")[:13]))
    out.append(("extra cut short", member(hello, flags=4, extra=b"abcdef")[:14]))
    out.append(("trailer cut short", good[:-1]))
    out.append(("header only", good[:10]))
    out.append(("nine bytes", good[:9]))
    out.append(("nothing", b""))
    out.append(("trailing bytes", good + b"next"))
    out.append(("two members", good + member(b"second")))
    return out


def mutations(base, count, seed):
    rng = random.Random(seed)
    out = []
    for k in range(count):
        z = base[1]
        at = rng.randrange(len(z))
        how = rng.randrange(3)
        if how == 0:
            m = z[:at] + bytes([z[at] ^ 1 << rng.randrange(8)]) + z[at + 1:]
        elif how == 1:
            m = z[:at] + bytes([rng.randrange(256)]) + z[at + 1:]
        else:
            m = z[:at]
        out.append(("%s, mutation %d" % (base[0], k), m))
    return out


def c_bytes(data):
    if not data:
        return '""'
    rows = [",".join("%d" % b for b in data[i:i + 24]) for i in range(0, len(data), 24)]
    return "{\n    " + ",\n    ".join(rows) + "\n}"


def render():
    """Small vectors for every checker, Cerberus included; the rest under
    #ifndef __cerb__, as in tests/zlib_vectors.h."""
    lines = ["/* Made by tests/gzip_vectors.py from zlib %s; do not edit. */" %
             zlib.ZLIB_RUNTIME_VERSION, ""]
    lines.append("struct crc_vector {\n    const unsigned char *data;\n    size_t len;\n"
                 "    unsigned long crc;\n};\n")
    lines.append("/* A member, and what zlib makes of it: whether it accepts it, the")
    lines.append(" * length and CRC-32 of its data, and the bytes it used. */")
    lines.append("struct member {\n    const char *name;\n    const unsigned char *z;\n"
                 "    size_t len;\n    int ok;\n    size_t data_len;\n    unsigned long data_crc;\n"
                 "    size_t used;\n};\n")
    crcs = [b"", b"a", b"123456789", b"\xff" * 64, text(1000, 3), noise(777, 4)]
    all_cases = cases()
    all_cases += mutations(("text flag", member(b"0009done\n0000", flags=1)), 120, 1952)
    all_cases += mutations(("every field", member(b"want x\n", flags=31, extra=b"xy", name=b"n",
                                                  comment=b"c")), 120, 1953)
    all_cases += mutations(("zlib level 9", __import__("gzip").compress(text(200, 5), 9,
                                                                         mtime=0)), 120, 1954)
    small = [(n, z) for k, (n, z) in enumerate(all_cases)
             if len(z) <= 64 and ("mutation" not in n or k % 30 == 0)]
    large = [(n, z) for (n, z) in all_cases if (n, z) not in small]

    def emit(prefix, crc_list, member_list):
        out = []
        for i, d in enumerate(crc_list):
            out.append("static const unsigned char %scrc%d[] = %s;" % (prefix, i, c_bytes(d)))
        out.append("static const struct crc_vector %scrcs[] = {" % prefix)
        out += ["    {%scrc%d, %d, 0x%08lxUL}," % (prefix, i, len(d), zlib.crc32(d))
                for i, d in enumerate(crc_list)]
        out.append("};\n")
        rows = []
        for i, (name, z) in enumerate(member_list):
            got = inflate(z)
            out.append("static const unsigned char %sm%d[] = %s;" % (prefix, i, c_bytes(z)))
            if got is None:
                rows.append('    {"%s", %sm%d, %d, 0, 0, 0, 0},' % (name, prefix, i, len(z)))
            else:
                rows.append('    {"%s", %sm%d, %d, 1, %d, 0x%08lxUL, %d},' %
                            (name, prefix, i, len(z), len(got[0]), zlib.crc32(got[0]), got[1]))
        out.append("static const struct member %smembers[] = {" % prefix)
        out += rows
        out.append("};\n")
        return out

    lines += emit("small_", crcs[:4], small)
    lines.append("#ifndef __cerb__")
    lines += emit("large_", crcs[4:], large)
    lines.append("#endif")
    lines.append("")
    return "\n".join(lines)


def main(argv):
    import gzip
    if argv not in ([], ["--check"]):
        sys.stderr.write("usage: gzip_vectors.py [--check]\n")
        return 2
    seeds = os.path.join("fuzz", "fuzz_zlib.seeds")
    files = [(HEADER, render().encode()),
             (os.path.join(seeds, "gzip_every_field"),
              member(text(300, 6), flags=31, extra=b"ab\x02\x00cd", name=b"n", comment=b"c")),
             (os.path.join(seeds, "gzip_level9"), gzip.compress(text(300, 7), 9, mtime=0))]
    if argv == ["--check"]:
        stale = []
        for path, data in files:
            try:
                with open(path, "rb") as f:
                    same = f.read() == data
            except OSError:
                same = False
            if not same:
                stale.append(path)
        print("gzip vectors: %s" % ("DIFFERENT: " + " ".join(stale) if stale else "up to date"))
        return 1 if stale else 0
    for path, data in files:
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
    print("gzip vectors written")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
