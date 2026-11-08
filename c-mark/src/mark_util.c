/* goldmark util/util.go as C. Each function names the Go function it
 * ports; the byte tables are goldmark's 256-entry tables written as range
 * tests (tools/gen_tables.py is for the two big tables only). */
#include "mark_util.h"
#include "mark_unicode.h"

/* ---- UTF-8 (Go's unicode/utf8) ---- */

/* utf8.DecodeRune */
unsigned long mark_decode_rune(const unsigned char *p, long n, long *size)
{
    unsigned char c;
    unsigned char lo = 0x80;
    unsigned char hi = 0xBF;
    long len;
    long i;
    unsigned long r;

    if (n <= 0) {
        *size = 0;
        return MARK_RUNE_ERROR;
    }
    c = p[0];
    if (c < 0x80) {
        *size = 1;
        return c;
    }
    *size = 1;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        r = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        r = c & 0x0F;
        if (c == 0xE0) lo = 0xA0;
        if (c == 0xED) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        r = c & 0x07;
        if (c == 0xF0) lo = 0x90;
        if (c == 0xF4) hi = 0x8F;
    } else {
        return MARK_RUNE_ERROR;
    }
    if (n < len || p[1] < lo || p[1] > hi) return MARK_RUNE_ERROR;
    r = r << 6 | (p[1] & 0x3F);
    for (i = 2; i < len; i++) {
        if (p[i] < 0x80 || p[i] > 0xBF) return MARK_RUNE_ERROR;
        r = r << 6 | (p[i] & 0x3F);
    }
    *size = len;
    return r;
}

/* utf8.RuneStart */
int mark_rune_start(unsigned char c)
{
    return (c & 0xC0) != 0x80;
}

/* utf8.ValidRune */
int mark_valid_rune(unsigned long r)
{
    if (r >= 0xD800UL && r <= 0xDFFFUL) return 0;
    return r <= 0x10FFFFUL;
}

/* util.ToValidRune */
unsigned long mark_to_valid_rune(unsigned long r)
{
    if (r == 0 || !mark_valid_rune(r)) return MARK_RUNE_ERROR;
    return r;
}

/* utf8.EncodeRune */
long mark_encode_rune(unsigned char *out, unsigned long r)
{
    if (!mark_valid_rune(r)) r = MARK_RUNE_ERROR;
    if (r < 0x80) {
        out[0] = (unsigned char)r;
        return 1;
    }
    if (r < 0x800) {
        out[0] = (unsigned char)(0xC0 | r >> 6);
        out[1] = (unsigned char)(0x80 | (r & 0x3F));
        return 2;
    }
    if (r < 0x10000) {
        out[0] = (unsigned char)(0xE0 | r >> 12);
        out[1] = (unsigned char)(0x80 | (r >> 6 & 0x3F));
        out[2] = (unsigned char)(0x80 | (r & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | r >> 18);
    out[1] = (unsigned char)(0x80 | (r >> 12 & 0x3F));
    out[2] = (unsigned char)(0x80 | (r >> 6 & 0x3F));
    out[3] = (unsigned char)(0x80 | (r & 0x3F));
    return 4;
}

/* util.ToRune: steps back from pos to the start of its rune and decodes. */
unsigned long mark_to_rune(const unsigned char *source, long len, long pos)
{
    long i = pos;
    long size;

    for (; i > 0; i--) {
        if (mark_rune_start(source[i])) break;
    }
    return mark_decode_rune(source + i, len - i, &size);
}

/* ---- Byte classes (util.spaceTable, punctTable, utf8lenTable) ---- */

/* util.IsSpace: tab, newline, carriage return and space (not \v or \f). */
int mark_byte_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* util.IsPunct: ASCII punctuation. */
int mark_byte_punct(unsigned char c)
{
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) ||
           (c >= 0x7B && c <= 0x7E);
}

int mark_byte_numeric(unsigned char c)
{
    return c >= '0' && c <= '9';
}

int mark_byte_hex(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int mark_byte_alnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* util.UTF8Len: the table's 99 marks a byte that cannot start a rune. */
int mark_utf8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return 2;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xF0 && c <= 0xF7) return 4;
    return 99;
}

/* util.IsSpaceRune. Go tests int32(r) <= 256 before indexing the byte
 * table, so 256 itself indexes entry 0, which is not a space. */
int mark_rune_space(unsigned long r)
{
    if (r <= 256 && mark_byte_space((unsigned char)r)) return 1;
    return mark_is_space(r);
}

/* util.IsPunctRune, with the same 256 quirk as IsSpaceRune. */
int mark_rune_punct(unsigned long r)
{
    if (r <= 256 && mark_byte_punct((unsigned char)r)) return 1;
    return mark_is_punct(r);
}

/* util.IsBlank */
int mark_is_blank(const unsigned char *s, long len)
{
    long i;

    for (i = 0; i < len; i++) {
        if (!mark_byte_space(s[i])) return 0;
    }
    return 1;
}

/* util.TabWidth */
long mark_tab_width(long pos)
{
    return 4 - pos % 4;
}

/* util.TrimLeftSpaceLength */
long mark_trim_left_space_len(const unsigned char *s, long len)
{
    long i = 0;

    for (; i < len; i++) {
        if (!mark_byte_space(s[i])) break;
    }
    return i;
}

/* util.TrimRightSpaceLength */
long mark_trim_right_space_len(const unsigned char *s, long len)
{
    long i = len - 1;

    for (; i >= 0; i--) {
        if (!mark_byte_space(s[i])) break;
    }
    if (i < 0) return len;
    return len - 1 - i;
}

/* util.spaces: the set TrimLeftSpace/TrimRightSpace (the byte versions)
 * trim, which unlike IsSpace includes \v and \f. */
static int is_spaces_set(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == 0x0C || c == '\r';
}

/* util.TrimLeftSpace then util.TrimRightSpace (the byte versions). */
CuStr mark_trim_spaces(CuStr s)
{
    size_t start = 0;
    size_t end = s.len;

    while (start < end && is_spaces_set((unsigned char)s.ptr[start])) start++;
    while (end > start && is_spaces_set((unsigned char)s.ptr[end - 1])) end--;
    return cu_str_substr(s, start, end);
}

/* ---- URLs and emails (util.urlTable, emailTable) ---- */

/* urlTable bit 1: allowed after the scheme, [^<>\x00-\x20] */
static int url_body_byte(unsigned char c)
{
    return c > 0x20 && c != '<' && c != '>';
}

/* urlTable bit 4: [A-Za-z0-9.+-] */
static int url_scheme_byte(unsigned char c)
{
    return mark_byte_alnum(c) || c == '.' || c == '+' || c == '-';
}

/* util.FindURLIndex: [A-Za-z][A-Za-z0-9.+-]{1,31}:[^<>\x00-\x20]* */
long mark_find_url_index(const unsigned char *b, long len)
{
    long i = 0;

    if (!(len > 0 && ((b[0] >= 'A' && b[0] <= 'Z') || (b[0] >= 'a' && b[0] <= 'z')))) return -1;
    i++;
    for (; i < len; i++) {
        if (!url_scheme_byte(b[i])) break;
    }
    if (i == 1 || i > 33 || i >= len) return -1;
    if (b[i] != ':') return -1;
    i++;
    for (; i < len; i++) {
        if (!url_body_byte(b[i])) break;
    }
    return i;
}

/* emailTable bit 1: [a-zA-Z0-9.!#$%&'*+/=?^_`{|}~-] */
static int email_local_byte(unsigned char c)
{
    if (mark_byte_alnum(c)) return 1;
    return c == '!' || (c >= '#' && c <= '\'') || c == '*' || c == '+' || c == '-' || c == '.' || c == '/' ||
           c == '=' || c == '?' || (c >= '^' && c <= '~');
}

static int label_byte(unsigned char c)
{
    return mark_byte_alnum(c) || c == '-';
}

/* One label of util.emailDomainRegexp after its first alphanumeric:
 * (?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])? matched greedily, so it ends at the
 * last alphanumeric within the next 62 bytes of label characters. */
static long email_label_rest(const unsigned char *b, long len, long i)
{
    long run = 0;
    long k;

    while (i + run < len && run < 62 && label_byte(b[i + run])) run++;
    for (k = run - 1; k >= 0; k--) {
        if (mark_byte_alnum(b[i + k])) return i + k + 1;
    }
    return i;
}

/* util.FindEmailIndex, with emailDomainRegexp
 * ^[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*
 * written out: every part after the first character is optional, so the
 * match is the greedy choice at each step. */
long mark_find_email_index(const unsigned char *b, long len)
{
    long i = 0;

    for (; i < len; i++) {
        if (!email_local_byte(b[i])) break;
    }
    if (i == 0) return -1;
    if (i >= len || b[i] != '@') return -1;
    i++;
    if (i >= len) return -1;
    if (!mark_byte_alnum(b[i])) return -1;
    i = email_label_rest(b, len, i + 1);
    while (i + 1 < len && b[i] == '.' && mark_byte_alnum(b[i + 1])) {
        i = email_label_rest(b, len, i + 2);
    }
    return i;
}

/* ---- HTML escaping (util.htmlEscapeTable) ---- */

/* util.EscapeHTMLByte */
const char *mark_escape_html_byte(unsigned char c)
{
    switch (c) {
    case '"':
        return "&quot;";
    case '&':
        return "&amp;";
    case '<':
        return "&lt;";
    case '>':
        return "&gt;";
    default:
        return NULL;
    }
}

/* util.EscapeHTML */
void mark_escape_html(CuBuilder *b, CuStr s)
{
    size_t i;

    for (i = 0; i < s.len; i++) {
        const char *e = mark_escape_html_byte((unsigned char)s.ptr[i]);

        if (e != NULL) cu_sb_str(b, cu_str_cstr(e));
        else cu_sb_char(b, s.ptr[i]);
    }
}

/* ---- Reference resolution for URLs ---- */

/* The CopyOnWriteBuffer pattern: a stage that never writes returns its
 * input unchanged; one that did write appends the tail and returns the new
 * bytes. */
/** regions: returns view(*b); returns view(v) */
static CuStr cow_finish(CuBuilder *b, CuStr v, int copied, size_t n)
{
    CuStr out;

    if (!copied) return v;
    cu_sb_str(b, cu_str_skip(v, n));
    if (!cu_sb_finish(b, &out)) return cu_str_empty();
    return out;
}

/* util.UnescapePunctuations */
static CuStr unescape_punctuations(CuArena *a, CuStr v)
{
    const unsigned char *s = (const unsigned char *)v.ptr;
    size_t limit = v.len;
    size_t n = 0;
    size_t i = 0;
    int copied = 0;
    CuBuilder b;

    cu_sb_init(&b, a);
    while (i < limit) {
        if (i < limit - 1 && s[i] == '\\' && mark_byte_punct(s[i + 1])) {
            cu_sb_str(&b, cu_str_substr(v, n, i));
            cu_sb_char(&b, (char)s[i + 1]);
            copied = 1;
            i += 2;
            n = i;
            continue;
        }
        i++;
    }
    return cow_finish(&b, v, copied, n);
}

/* strconv.ParseUint(s, 16, 32) as ResolveNumericReferences uses it: a
 * value over 32 bits becomes the maximum, which rune() makes negative and
 * ToValidRune turns into U+FFFD, so saturating is enough. */
static unsigned long parse_hex_32(const unsigned char *s, size_t len)
{
    unsigned long v = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned long d;

        if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
        else if (s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
        else d = s[i] - 'A' + 10;
        if (v > 0x0FFFFFFFUL) return 0xFFFFFFFFUL;
        v = v << 4 | d;
    }
    return v;
}

/* strconv.ParseUint(s, 0, 32) on a string of at most 7 decimal digits:
 * base 0 reads a leading 0 as octal, and an 8 or 9 in an octal number is a
 * syntax error whose result is 0 (goldmark ignores the error). */
static unsigned long parse_base0_32(const unsigned char *s, size_t len)
{
    unsigned long v = 0;
    unsigned long base = 10;
    size_t i = 0;

    if (len > 1 && s[0] == '0') {
        base = 8;
        i = 1;
    }
    for (; i < len; i++) {
        unsigned long d = s[i] - '0';

        if (d >= base) return 0;
        v = v * base + d;
    }
    return v;
}

static void append_rune(CuBuilder *b, unsigned long r)
{
    unsigned char buf[4];
    long n = mark_encode_rune(buf, r);

    cu_sb_str(b, cu_str_n((const char *)buf, (size_t)n));
}

/* util.ResolveNumericReferences */
static CuStr resolve_numeric_references(CuArena *a, CuStr v)
{
    const unsigned char *s = (const unsigned char *)v.ptr;
    size_t limit = v.len;
    size_t n = 0;
    size_t i;
    int copied = 0;
    CuBuilder b;

    cu_sb_init(&b, a);
    for (i = 0; i < limit; i++) {
        if (s[i] == '&') {
            size_t pos = i;
            size_t next = i + 1;

            if (next < limit && s[next] == '#') {
                size_t nnext = next + 1;

                if (nnext < limit) {
                    unsigned char nc = s[nnext];

                    if (nc == 'x' || nc == 'X') {
                        size_t start = nnext + 1;

                        for (i = start; i < limit && mark_byte_hex(s[i]); i++) {
                        }
                        if (i > start && i < limit && s[i] == ';') {
                            cu_sb_str(&b, cu_str_substr(v, n, pos));
                            copied = 1;
                            n = i + 1;
                            append_rune(&b, mark_to_valid_rune(parse_hex_32(s + start, i - start)));
                            continue;
                        }
                    } else if (nc >= '0' && nc <= '9') {
                        size_t start = nnext;

                        for (i = start; i < limit && mark_byte_numeric(s[i]); i++) {
                        }
                        if (i > start && i < limit && i - start < 8 && s[i] == ';') {
                            cu_sb_str(&b, cu_str_substr(v, n, pos));
                            copied = 1;
                            n = i + 1;
                            append_rune(&b, mark_to_valid_rune(parse_base0_32(s + start, i - start)));
                            continue;
                        }
                    }
                }
            }
            i = next - 1;
        }
    }
    return cow_finish(&b, v, copied, n);
}

/* util.ResolveEntityNames */
static CuStr resolve_entity_names(CuArena *a, CuStr v)
{
    const unsigned char *s = (const unsigned char *)v.ptr;
    size_t limit = v.len;
    size_t n = 0;
    size_t i;
    int copied = 0;
    CuBuilder b;

    cu_sb_init(&b, a);
    for (i = 0; i < limit; i++) {
        if (s[i] == '&') {
            size_t pos = i;
            size_t next = i + 1;

            if (!(next < limit && s[next] == '#')) {
                size_t start = next;

                for (i = start; i < limit && mark_byte_alnum(s[i]); i++) {
                }
                if (i > start && i < limit && s[i] == ';') {
                    const MarkEntity *e = mark_entity_lookup(cu_str_substr(v, start, i));

                    if (e != NULL) {
                        cu_sb_str(&b, cu_str_substr(v, n, pos));
                        copied = 1;
                        n = i + 1;
                        cu_sb_str(&b, cu_str_n(e->chars, e->chars_len));
                        continue;
                    }
                }
            }
            i = next - 1;
        }
    }
    return cow_finish(&b, v, copied, n);
}

/* util.urlEscapeTable: a-zA-Z0-9 and ;/?:@&=+$,-_.!~*'()# pass through. */
static int url_escape_pass(unsigned char c)
{
    if (mark_byte_alnum(c)) return 1;
    return c == '!' || c == '#' || c == '$' || (c >= '&' && c <= ';') || c == '=' || (c >= '?' && c <= '@') ||
           c == '_' || c == '~';
}

/* net/url.QueryEscape on one byte: unreserved bytes stay, every other byte
 * becomes %XX (a space never reaches here; URLEscape writes %20 itself). */
static void query_escape_byte(CuBuilder *b, unsigned char c)
{
    static const char hex[] = "0123456789ABCDEF";

    if (mark_byte_alnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
        cu_sb_char(b, (char)c);
        return;
    }
    cu_sb_char(b, '%');
    cu_sb_char(b, hex[c >> 4]);
    cu_sb_char(b, hex[c & 0x0F]);
}

/* util.URLEscape. The oddities are goldmark's: the %XX test reads v[i+1]
 * twice and never v[i+2]; a lead byte whose sequence runs past the end is
 * dropped, and when that happens before anything was copied the whole
 * input is returned as it is. */
void mark_url_escape(CuBuilder *out, CuStr v, int resolve_reference)
{
    CuArena *a = out->arena;
    const unsigned char *s;
    size_t limit;
    size_t n = 0;
    size_t i = 0;
    int copied = 0;
    CuBuilder b;

    if (resolve_reference) {
        v = unescape_punctuations(a, v);
        v = resolve_numeric_references(a, v);
        v = resolve_entity_names(a, v);
    }
    s = (const unsigned char *)v.ptr;
    limit = v.len;
    cu_sb_init(&b, a);
    while (i < limit) {
        unsigned char c = s[i];
        long u8len;
        size_t stop;
        size_t k;

        if (url_escape_pass(c)) {
            i++;
            continue;
        }
        if (c == '%' && i + 2 < limit && mark_byte_hex(s[i + 1]) && mark_byte_hex(s[i + 1])) {
            i += 3;
            continue;
        }
        u8len = mark_utf8_len(c);
        if (u8len == 99) {
            i++;
            continue;
        }
        if (c == ' ') {
            cu_sb_str(&b, cu_str_substr(v, n, i));
            cu_sb_str(&b, CU_LIT("%20"));
            copied = 1;
            i++;
            n = i;
            continue;
        }
        if (u8len > (long)limit) u8len = (long)limit - 1;
        if (u8len == 0) {
            i++;
            n = i;
            continue;
        }
        cu_sb_str(&b, cu_str_substr(v, n, i));
        copied = 1;
        stop = i + (size_t)u8len;
        if (stop > limit) {
            i++;
            n = i;
            continue;
        }
        for (k = i; k < stop; k++) query_escape_byte(&b, s[k]);
        i = stop;
        n = i;
    }
    if (!copied) {
        cu_sb_str(out, v);
        return;
    }
    if (n < limit) cu_sb_str(&b, cu_str_skip(v, n));
    {
        CuStr r;

        if (cu_sb_finish(&b, &r)) cu_sb_str(out, r);
    }
}

/* ---- Link reference labels ---- */

/* util.DoFullUnicodeCaseFolding */
static CuStr full_case_folding(CuArena *a, CuStr v)
{
    const unsigned char *s = (const unsigned char *)v.ptr;
    size_t len = v.len;
    size_t n = 0;
    size_t i;
    int copied = 0;
    CuBuilder b;

    cu_sb_init(&b, a);
    for (i = 0; i < len; i++) {
        unsigned char c = s[i];
        unsigned long r;
        long size;
        const MarkFolding *f;
        size_t k;

        if (c < 0xB5) {
            if (c >= 'A' && c <= 'Z') {
                cu_sb_str(&b, cu_str_substr(v, n, i));
                cu_sb_char(&b, (char)(c + 32));
                copied = 1;
                n = i + 1;
            }
            continue;
        }
        if (!mark_rune_start(c)) continue;
        r = mark_decode_rune(s + i, (long)(len - i), &size);
        if (r == MARK_RUNE_ERROR) continue;
        f = mark_folding_lookup(r);
        if (f == NULL) continue;
        cu_sb_str(&b, cu_str_substr(v, n, i));
        copied = 1;
        for (k = 0; k < f->count; k++) append_rune(&b, f->to[k]);
        i += (size_t)size - 1;
        n = i + 1;
    }
    return cow_finish(&b, v, copied, n);
}

/* util.ReplaceSpaces(v, ' '). Its buffer starts only when a run of spaces
 * ends, so an input whose only spaces are trailing comes back unchanged. */
static void replace_spaces(CuBuilder *out, CuStr v)
{
    const unsigned char *s = (const unsigned char *)v.ptr;
    long start = -1;
    int have = 0;
    size_t i;

    for (i = 0; i < v.len; i++) {
        int iss = mark_byte_space(s[i]);

        if (start < 0 && iss) {
            start = (long)i;
            continue;
        } else if (start >= 0 && iss) {
            continue;
        } else if (start >= 0) {
            if (!have) {
                cu_sb_str(out, cu_str_prefix(v, (size_t)start));
                have = 1;
            }
            cu_sb_char(out, ' ');
            start = -1;
        }
        if (have) cu_sb_char(out, (char)s[i]);
    }
    if (start >= 0 && have) cu_sb_char(out, ' ');
    if (!have) cu_sb_str(out, v);
}

/* util.ToLinkReference */
void mark_to_link_reference(CuBuilder *out, CuStr label)
{
    CuStr v = mark_trim_spaces(label);

    v = full_case_folding(out->arena, v);
    replace_spaces(out, v);
}
