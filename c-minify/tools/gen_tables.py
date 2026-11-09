#!/usr/bin/env python3
"""Generate c-minify's HTML tables from tdewolff/minify's Go sources.

    python3 tools/gen_tables.py path/to/minify-v2.20.5 src

reads html/hash.go and html/table.go under the first argument and writes
src/minify_html_hash.h (the hash constants) and src/minify_html_table.c
(the perfect-hash table and text, the tag and attribute traits, the JS
mimetypes and the entity map) under the second. Python 3 standard library
only. The traits and entities are sorted so the C side can binary-search
them; the Go side uses maps, so the order there carries no meaning.
"""
import os
import re
import sys

CONST_RE = re.compile(r'^\s*([A-Za-z_][A-Za-z0-9_]*)\s+Hash = (0x[0-9a-f]+)\s*// (\S+)\s*$')
TABLE_RE = re.compile(r'^\s*(0x[0-9a-f]+):\s*(0x[0-9a-f]+),\s*// \S+\s*$')
TEXT_RE = re.compile(r'^(?:const _Hash_text = |\s*)"([^"]*)"(?: \+)?\s*$')
TRAIT_RE = re.compile(r'^\s*([A-Za-z_][A-Za-z0-9_]*):\s*([A-Za-z| ]+?),\s*(?://.*)?$')
ENTITY_RE = re.compile(r'^\s*"([A-Za-z0-9]+)":\s*\[\]byte\("((?:[^"\\]|\\.)*)"\),\s*$')
JSMIME_RE = re.compile(r'^\s*"([^"]+)":\s*true,\s*$')

TAG_TRAITS = {'normalTag': 1, 'rawTag': 2, 'blockTag': 4, 'objectTag': 8, 'omitPTag': 16, 'keepPTag': 32}
ATTR_TRAITS = {'booleanAttr': 1, 'urlAttr': 2, 'trimAttr': 4}


def c_bytes(values):
    """A C string literal of the given byte values, every byte escaped so
    the literal never runs a hex escape into a following digit."""
    return '"' + ''.join('\\x%02x' % b for b in values) + '"'


def go_unquote(s):
    """The bytes of an interpreted Go string literal's body (the escapes
    table.go uses: \\n, \\t, \\\\, \\")."""
    out = bytearray()
    i = 0
    while i < len(s):
        c = s[i]
        if c != '\\':
            out += c.encode('utf-8')
            i += 1
            continue
        e = s[i + 1]
        table = {'n': 10, 't': 9, '\\': 92, '"': 34}
        if e not in table:
            raise SystemExit('unhandled Go escape in ' + s)
        out.append(table[e])
        i += 2
    return bytes(out)


def read_hash(path):
    consts = []
    table = {}
    text = []
    hash0 = None
    max_len = None
    table_size = None
    in_text = False
    with open(path, encoding='utf-8') as f:
        for line in f:
            m = CONST_RE.match(line)
            if m:
                consts.append((m.group(1), int(m.group(2), 16), m.group(3)))
                continue
            m = TABLE_RE.match(line)
            if m:
                table[int(m.group(1), 16)] = int(m.group(2), 16)
                continue
            m = re.match(r'^const _Hash_hash0 = (0x[0-9a-f]+)\s*$', line)
            if m:
                hash0 = int(m.group(1), 16)
                continue
            m = re.match(r'^const _Hash_maxLen = (\d+)\s*$', line)
            if m:
                max_len = int(m.group(1))
                continue
            m = re.match(r'^var _Hash_table = \[1 << (\d+)\]Hash\{\s*$', line)
            if m:
                table_size = 1 << int(m.group(1))
                continue
            if line.startswith('const _Hash_text = '):
                in_text = True
            if in_text:
                m = TEXT_RE.match(line)
                if m is None:
                    raise SystemExit('unexpected text line: ' + line.strip())
                text.append(m.group(1))
                if not line.rstrip().endswith('+'):
                    in_text = False
    text = ''.join(text)
    if hash0 is None or max_len is None or table_size is None or len(consts) < 200 or not text:
        raise SystemExit('could not parse ' + path)
    for name, value, word in consts:
        start, n = value >> 8, value & 0xff
        if text[start:start + n] != word or n != len(word):
            raise SystemExit('hash constant %s does not name %s' % (name, word))
    return consts, table, table_size, text, hash0, max_len


def read_table(path, consts):
    by_name = {name: value for name, value, _ in consts}
    tags, attrs, jsmimes, entities = [], [], [], []
    section = None
    with open(path, encoding='utf-8') as f:
        for line in f:
            s = line.strip()
            if s.startswith('var tagMap'):
                section = 'tag'
                continue
            if s.startswith('var attrMap'):
                section = 'attr'
                continue
            if s.startswith('var jsMimetypes'):
                section = 'js'
                continue
            if s.startswith('var EntitiesMap'):
                section = 'entity'
                continue
            if s.startswith('var TextRevEntitiesMap'):
                section = 'rev'
                continue
            if s == '}':
                section = None
                continue
            if section in ('tag', 'attr'):
                m = TRAIT_RE.match(line)
                if m is None:
                    if s == '' or s.startswith('//'):
                        continue
                    raise SystemExit('unexpected %s line: %s' % (section, s))
                names = [t.strip() for t in m.group(2).split('|')]
                known = TAG_TRAITS if section == 'tag' else ATTR_TRAITS
                bits = 0
                for t in names:
                    bits |= known[t]
                (tags if section == 'tag' else attrs).append((by_name[m.group(1)], bits, m.group(1)))
            elif section == 'js':
                m = JSMIME_RE.match(line)
                if m:
                    jsmimes.append(m.group(1))
            elif section == 'entity':
                m = ENTITY_RE.match(line)
                if m is None:
                    raise SystemExit('unexpected entity line: ' + s)
                entities.append((m.group(1), go_unquote(m.group(2))))
            elif section == 'rev':
                if s != "'<': []byte(\"&lt;\"),":
                    raise SystemExit('TextRevEntitiesMap changed: ' + s)
    if len(tags) < 100 or len(attrs) < 100 or len(jsmimes) != 2 or len(entities) < 1000:
        raise SystemExit('could not parse ' + path)
    tags.sort()
    attrs.sort()
    entities.sort(key=lambda e: e[0].encode('ascii'))
    return tags, attrs, jsmimes, entities


def c_name(go_name):
    return 'MH_' + go_name.upper()


def write_hash_header(consts, out_path):
    with open(out_path, 'w', encoding='ascii') as f:
        f.write('/* Generated by tools/gen_tables.py from html/hash.go\n')
        f.write(' * (tdewolff/minify v2.20.5, the Hash constants). Do not edit;\n')
        f.write(' * regenerate with: python3 tools/gen_tables.py REF src */\n')
        f.write('#ifndef MINIFY_HTML_HASH_H\n#define MINIFY_HTML_HASH_H\n\n')
        f.write('/* Hash: a perfect hash of the tag and attribute names html.go knows;\n')
        f.write(' * the high bits index the name in the hash text and the low byte is\n')
        f.write(' * its length. 0 is no name. */\n')
        for name, value, word in consts:
            f.write('#define %s 0x%xUL /* %s */\n' % (c_name(name), value, word))
        f.write('\n#endif\n')


def write_table(consts, table, table_size, text, hash0, max_len, tags, attrs, jsmimes, entities, out_path):
    with open(out_path, 'w', encoding='ascii') as f:
        f.write('/* Generated by tools/gen_tables.py from html/hash.go and html/table.go\n')
        f.write(' * (tdewolff/minify v2.20.5). Do not edit; regenerate with:\n')
        f.write(' * python3 tools/gen_tables.py REF src */\n')
        f.write('#include "minify_html_table.h"\n')
        f.write('#include <string.h>\n\n')
        f.write('#define HASH0 0x%xUL\n' % hash0)
        f.write('#define HASH_MAX_LEN %d\n' % max_len)
        f.write('#define HASH_TABLE_SIZE %d\n\n' % table_size)
        f.write('/* hash.go _Hash_text, as character constants: it is longer than the\n')
        f.write(' * 509 bytes a C89 string literal may hold. */\n')
        f.write('static const char hash_text[%d] = {\n' % len(text))
        for i in range(0, len(text), 20):
            f.write('    %s,\n' % ', '.join("'%s'" % c for c in text[i:i + 20]))
        f.write('};\n')
        f.write('\n/* hash.go _Hash_table */\n')
        f.write('static const unsigned long hash_table[HASH_TABLE_SIZE] = {\n')
        for i in range(table_size):
            f.write('    0x%xUL,%s' % (table.get(i, 0), '\n' if i % 8 == 7 else ' '))
        f.write('};\n\n')
        f.write('/* hash.go ToHash: 32-bit FNV-1a, two probes. */\n')
        f.write('unsigned long minify_html_to_hash(const unsigned char *s, size_t n)\n{\n')
        f.write('    unsigned long h = HASH0;\n    unsigned long i;\n    size_t k;\n\n')
        f.write('    if (n == 0 || n > HASH_MAX_LEN) return 0;\n')
        f.write('    for (k = 0; k < n; k++) {\n')
        f.write('        h ^= s[k];\n')
        f.write('        h = (h * 16777619UL) & 0xffffffffUL;\n')
        f.write('    }\n')
        f.write('    i = hash_table[h & (HASH_TABLE_SIZE - 1)];\n')
        f.write('    if ((i & 0xff) == n && memcmp(hash_text + (i >> 8), s, n) == 0) return i;\n')
        f.write('    i = hash_table[(h >> 16) & (HASH_TABLE_SIZE - 1)];\n')
        f.write('    if ((i & 0xff) == n && memcmp(hash_text + (i >> 8), s, n) == 0) return i;\n')
        f.write('    return 0;\n}\n\n')
        for kind, rows in (('tag', tags), ('attr', attrs)):
            f.write('/* table.go %sMap, sorted by hash. */\n' % kind)
            f.write('static const MinifyHtmlTraits %s_traits[%d] = {\n' % (kind, len(rows)))
            for value, bits, name in rows:
                f.write('    {0x%xUL, %d}, /* %s */\n' % (value, bits, name.lower()))
            f.write('};\n\n')
        f.write('static unsigned traits_of(const MinifyHtmlTraits *rows, size_t n, unsigned long hash)\n{\n')
        f.write('    size_t lo = 0;\n    size_t hi = n;\n\n')
        f.write('    while (lo < hi) {\n')
        f.write('        size_t mid = lo + (hi - lo) / 2;\n\n')
        f.write('        if (rows[mid].hash == hash) return rows[mid].traits;\n')
        f.write('        if (rows[mid].hash < hash) lo = mid + 1;\n')
        f.write('        else hi = mid;\n')
        f.write('    }\n')
        f.write('    return 0;\n}\n\n')
        f.write('unsigned minify_html_tag_traits(unsigned long hash)\n{\n')
        f.write('    return traits_of(tag_traits, sizeof tag_traits / sizeof tag_traits[0], hash);\n}\n\n')
        f.write('unsigned minify_html_attr_traits(unsigned long hash)\n{\n')
        f.write('    return traits_of(attr_traits, sizeof attr_traits / sizeof attr_traits[0], hash);\n}\n\n')
        f.write('/* table.go jsMimetypes */\n')
        f.write('int minify_html_js_mimetype(const unsigned char *s, size_t n)\n{\n')
        for mime in jsmimes:
            f.write('    if (n == %d && memcmp(s, "%s", %d) == 0) return 1;\n' % (len(mime), mime, len(mime)))
        f.write('    return 0;\n}\n\n')
        f.write('#define ENTITY_COUNT %d\n\n' % len(entities))
        f.write('/* table.go EntitiesMap, sorted by name in byte order. */\n')
        f.write('static const MinifyEntity entities[ENTITY_COUNT] = {\n')
        for name, value in entities:
            f.write('    {"%s", %d, %s, %d},\n' % (name, len(name), c_bytes(value), len(value)))
        f.write('};\n\n')
        f.write('const MinifyEntity *minify_html_entity(const unsigned char *name, size_t n)\n{\n')
        f.write('    size_t lo = 0;\n    size_t hi = ENTITY_COUNT;\n\n')
        f.write('    while (lo < hi) {\n')
        f.write('        size_t mid = lo + (hi - lo) / 2;\n')
        f.write('        const MinifyEntity *e = &entities[mid];\n')
        f.write('        size_t k = n < e->name_len ? n : e->name_len;\n')
        f.write('        int c = memcmp(name, e->name, k);\n\n')
        f.write('        if (c == 0) {\n')
        f.write('            if (n == e->name_len) return e;\n')
        f.write('            c = n < e->name_len ? -1 : 1;\n')
        f.write('        }\n')
        f.write('        if (c < 0) hi = mid;\n')
        f.write('        else lo = mid + 1;\n')
        f.write('    }\n')
        f.write('    return NULL;\n}\n')


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    ref, out = sys.argv[1], sys.argv[2]
    consts, table, size, text, hash0, max_len = read_hash(os.path.join(ref, 'html', 'hash.go'))
    tags, attrs, jsmimes, entities = read_table(os.path.join(ref, 'html', 'table.go'), consts)
    write_hash_header(consts, os.path.join(out, 'minify_html_hash.h'))
    write_table(consts, table, size, text, hash0, max_len, tags, attrs, jsmimes, entities,
                os.path.join(out, 'minify_html_table.c'))
    print('%d names, %d tags, %d attributes, %d entities' % (len(consts), len(tags), len(attrs), len(entities)))


if __name__ == '__main__':
    main()
