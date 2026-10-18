#include <stdlib.h>
#include <string.h>
#include "cu.h"

/* ---- UTF-8 ---- */

/* The length of the valid UTF-8 sequence at p (n bytes available), or 0
 * if none starts there: no overlong forms, no surrogates, nothing past
 * U+10FFFF (RFC 3629, table 3-7 of the Unicode standard). */
static size_t utf8_len(const unsigned char *p, size_t n)
{
    unsigned char c;
    unsigned char lo;
    unsigned char hi;
    size_t len;
    size_t i;

    c = p[0];
    if (c < 0x80)
        return 1;
    lo = 0x80;
    hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        if (c == 0xE0)
            lo = 0xA0;
        if (c == 0xED)
            hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        if (c == 0xF0)
            lo = 0x90;
        if (c == 0xF4)
            hi = 0x8F;
    } else {
        return 0;
    }
    if (n < len || p[1] < lo || p[1] > hi)
        return 0;
    for (i = 2; i < len; i++)
        if (p[i] < 0x80 || p[i] > 0xBF)
            return 0;
    return len;
}

/* The UTF-8 length of code point cp (at most 0x10FFFF, not a surrogate);
 * with dst, also writes it there. */
static size_t put_utf8(char *dst, unsigned long cp)
{
    char u[4];
    size_t n;
    size_t i;

    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | cp >> 6);
        u[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | cp >> 12);
        u[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | cp >> 18);
        u[1] = (char)(0x80 | (cp >> 12 & 0x3F));
        u[2] = (char)(0x80 | (cp >> 6 & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (dst != NULL)
        for (i = 0; i < n; i++)
            dst[i] = u[i];
    return n;
}

/* ---- Numbers ---- */

static int is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

/* The length of the JSON number at the start of s, or 0 if none:
 * -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? */
static size_t number_len(const unsigned char *s, size_t n)
{
    size_t i;
    size_t start;

    i = 0;
    if (i < n && s[i] == '-')
        i++;
    if (i < n && s[i] == '0') {
        i++;
    } else if (i < n && s[i] >= '1' && s[i] <= '9') {
        while (i < n && is_digit(s[i]))
            i++;
    } else {
        return 0;
    }
    if (i < n && s[i] == '.') {
        start = ++i;
        while (i < n && is_digit(s[i]))
            i++;
        if (i == start)
            return 0;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-'))
            i++;
        start = i;
        while (i < n && is_digit(s[i]))
            i++;
        if (i == start)
            return 0;
    }
    return i;
}

/* ---- Sorting ---- */

static int compare_bytes(CuStr x, CuStr y)
{
    size_t n;
    int c;

    n = x.len < y.len ? x.len : y.len;
    c = n == 0 ? 0 : memcmp(x.ptr, y.ptr, n);
    if (c != 0)
        return c;
    return x.len < y.len ? -1 : x.len > y.len;
}

/* The writer's quoted key whose opening quote is at offset at of data.
 * Equal keys quote to equal text, since the escaping depends only on the
 * bytes. */
static CuStr quoted_at(const char *data, size_t at)
{
    size_t i;

    for (i = at + 1; data[i] != '"'; i++)
        if (data[i] == '\\')
            i++;
    return cu_str_n(data + at, i + 1 - at);
}

/* Whether element x sorts before element y. With keys, the elements index
 * the parser's keys; without, they are offsets of the writer's quoted keys
 * in data. */
static int before(const CuStr *keys, const char *data, size_t x, size_t y)
{
    if (keys != NULL)
        return compare_bytes(keys[x], keys[y]) < 0;
    return compare_bytes(quoted_at(data, x), quoted_at(data, y)) < 0;
}

static void sift_down(size_t *v, size_t root, size_t n, const CuStr *keys, const char *data)
{
    size_t child;
    size_t t;

    for (;;) {
        child = 2 * root + 1;
        if (child >= n)
            return;
        if (child + 1 < n && before(keys, data, v[child], v[child + 1]))
            child++;
        if (!before(keys, data, v[root], v[child]))
            return;
        t = v[root];
        v[root] = v[child];
        v[child] = t;
        root = child;
    }
}

/* Sorts v[0..n) in place as before orders them: heapsort, so it needs no
 * memory and takes O(n log n) whatever the input. */
static void heap_sort(size_t *v, size_t n, const CuStr *keys, const char *data)
{
    size_t i;
    size_t t;

    for (i = n / 2; i > 0; i--)
        sift_down(v, i - 1, n, keys, data);
    for (i = n; i > 1; i--) {
        t = v[0];
        v[0] = v[i - 1];
        v[i - 1] = t;
        sift_down(v, 0, i - 1, keys, data);
    }
}

/* ---- Parser ----
 *
 * Recursive descent over s from *pos, everything allocated in a, each
 * allocation exactly the size it ends up: a container counts its items
 * before it parses them, and a string measures its decoded bytes before it
 * copies them. So the tree is the CuJson values, the keys and their order,
 * and the decoded bytes, and nothing else. State is passed as arguments,
 * not kept in a struct, so the regions check can follow every pointer. */

static int fail_at(CuJsonError *err, size_t at, const char *reason)
{
    err->offset = at;
    err->reason = reason;
    return 0;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_ws(CuStr s, size_t *pos)
{
    while (*pos < s.len && is_space(s.ptr[*pos]))
        (*pos)++;
}

static int hex4(CuStr s, size_t at, unsigned long *out)
{
    unsigned long v;
    size_t i;
    char c;

    if (s.len - at < 4)
        return 0;
    v = 0;
    for (i = 0; i < 4; i++) {
        c = s.ptr[at + i];
        if (c >= '0' && c <= '9')
            v = v * 16 + (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v = v * 16 + (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v = v * 16 + (unsigned long)(c - 'A' + 10);
        else
            return 0;
    }
    *out = v;
    return 1;
}

/* The code point meant by the escape whose backslash is at *pos. */
static int escape(CuStr s, size_t *pos, unsigned long *cp, CuJsonError *err)
{
    unsigned long low;
    size_t at;

    at = *pos;
    if (at + 1 >= s.len)
        return fail_at(err, at, "unterminated string");
    *pos = at + 2;
    switch (s.ptr[at + 1]) {
    case '"': *cp = '"'; return 1;
    case '\\': *cp = '\\'; return 1;
    case '/': *cp = '/'; return 1;
    case 'b': *cp = '\b'; return 1;
    case 'f': *cp = '\f'; return 1;
    case 'n': *cp = '\n'; return 1;
    case 'r': *cp = '\r'; return 1;
    case 't': *cp = '\t'; return 1;
    case 'u': break;
    default: return fail_at(err, at, "bad escape");
    }
    if (!hex4(s, at + 2, cp))
        return fail_at(err, at, "bad \\u escape");
    *pos = at + 6;
    if (*cp >= 0xDC00 && *cp <= 0xDFFF)
        return fail_at(err, at, "lone low surrogate");
    if (*cp >= 0xD800 && *cp <= 0xDBFF) {
        if (s.len - *pos < 6 || s.ptr[*pos] != '\\' || s.ptr[*pos + 1] != 'u' ||
            !hex4(s, *pos + 2, &low) || low < 0xDC00 || low > 0xDFFF)
            return fail_at(err, at, "lone high surrogate");
        *cp = 0x10000 + ((*cp - 0xD800) << 10) + (low - 0xDC00);
        *pos += 6;
    }
    return 1;
}

/* Checks the string whose opening quote is at *pos, moves *pos past its
 * closing quote and stores its decoded length in *len; with dst, also
 * writes the decoded bytes there. */
static int scan_string(CuStr s, size_t *pos, char *dst, size_t *len, CuJsonError *err)
{
    const unsigned char *u;
    unsigned long cp;
    size_t start;
    size_t k;
    size_t n;
    unsigned char c;

    u = (const unsigned char *)s.ptr;
    start = *pos;
    (*pos)++;
    k = 0;
    for (;;) {
        if (*pos >= s.len)
            return fail_at(err, start, "unterminated string");
        c = u[*pos];
        if (c == '"')
            break;
        if (c == '\\') {
            if (!escape(s, pos, &cp, err))
                return 0;
            k += put_utf8(dst == NULL ? NULL : dst + k, cp);
            continue;
        }
        if (c < 0x20)
            return fail_at(err, *pos, "raw control character in string");
        n = utf8_len(u + *pos, s.len - *pos);
        if (n == 0)
            return fail_at(err, *pos, "invalid UTF-8");
        if (dst != NULL)
            memcpy(dst + k, s.ptr + *pos, n);
        k += n;
        *pos += n;
    }
    (*pos)++;
    *len = k;
    return 1;
}

/* The string whose opening quote is at *pos, decoded into a. */
static int string(CuArena *a, CuStr s, size_t *pos, CuStr *out, CuJsonError *err)
{
    char *dst;
    size_t start;
    size_t len;

    start = *pos;
    if (!scan_string(s, pos, NULL, &len, err))
        return 0;
    dst = cu_arena_push_bytes(a, len);
    if (dst == NULL)
        return fail_at(err, start, "out of memory");
    /* The same bytes again, so this pass cannot fail. */
    if (!scan_string(s, &start, dst, &len, err))
        abort();
    *out = cu_str_n(dst, len);
    return 1;
}

/* How many values the array or object at s[start] holds, from the commas
 * at its top level. Exact when it is well formed: the parse stops with an
 * error before it reaches a value this count misses. Scans to the
 * container's end, so a byte nested d deep is scanned d + 1 times, and
 * CU_JSON_MAX_DEPTH bounds the cost. */
static size_t count_items(CuStr s, size_t start)
{
    size_t depth;
    size_t commas;
    size_t i;
    int any;
    char c;

    depth = 0;
    commas = 0;
    any = 0;
    for (i = start + 1; i < s.len; i++) {
        c = s.ptr[i];
        if (is_space(c))
            continue;
        if (depth == 0 && (c == ']' || c == '}'))
            break;
        any = 1;
        if (c == '"') {
            for (i++; i < s.len && s.ptr[i] != '"'; i++)
                if (s.ptr[i] == '\\')
                    i++;
        } else if (c == '[' || c == '{') {
            depth++;
        } else if (c == ']' || c == '}') {
            depth--;
        } else if (c == ',' && depth == 0) {
            commas++;
        }
    }
    return any ? commas + 1 : 0;
}

/* The indices of keys[0..n) in byte order, in a, through *order; fails
 * on a repeat. */
static int order_keys(CuArena *a, const CuStr *keys, size_t n, size_t **order, size_t at,
                      CuJsonError *err)
{
    size_t *idx;
    size_t i;

    *order = NULL;
    if (n == 0)
        return 1;
    idx = CU_PUSH(a, size_t, n);
    if (idx == NULL)
        return fail_at(err, at, "out of memory");
    for (i = 0; i < n; i++)
        idx[i] = i;
    heap_sort(idx, n, keys, NULL);
    for (i = 1; i < n; i++)
        if (cu_str_eq(keys[idx[i - 1]], keys[idx[i]]))
            return fail_at(err, at, "duplicate key");
    *order = idx;
    return 1;
}

static int value(CuArena *a, CuStr s, size_t *pos, size_t depth, CuJson *out, CuJsonError *err);

/* The array or object whose bracket is at *pos. */
static int container(CuArena *a, CuStr s, size_t *pos, size_t depth, CuJson *out,
                     CuJsonError *err)
{
    CuJson spare;
    CuJson *items;
    CuStr *keys;
    size_t *order;
    size_t count;
    size_t start;
    size_t n;
    int object;
    char close;

    start = *pos;
    object = s.ptr[start] == '{';
    close = object ? '}' : ']';
    if (depth >= CU_JSON_MAX_DEPTH)
        return fail_at(err, start, "nested too deeply");
    count = count_items(s, start);
    items = NULL;
    keys = NULL;
    if (count > 0) {
        items = CU_PUSH(a, CuJson, count);
        if (object)
            keys = CU_PUSH(a, CuStr, count);
        if (items == NULL || (object && keys == NULL))
            return fail_at(err, start, "out of memory");
    }
    (*pos)++;
    n = 0;
    skip_ws(s, pos);
    if (*pos < s.len && s.ptr[*pos] == close) {
        (*pos)++;
    } else {
        for (;;) {
            if (object) {
                skip_ws(s, pos);
                if (*pos >= s.len || s.ptr[*pos] != '"')
                    return fail_at(err, *pos, "expected a key");
                if (!string(a, s, pos, &keys[n], err))
                    return 0;
                skip_ws(s, pos);
                if (*pos >= s.len || s.ptr[*pos] != ':')
                    return fail_at(err, *pos, "expected ':'");
                (*pos)++;
            }
            if (n == count) {
                /* The count saw no value here, only space and then the
                 * end or a close of the other kind, so this fails. */
                if (value(a, s, pos, depth + 1, &spare, err))
                    abort();
                return 0;
            }
            if (!value(a, s, pos, depth + 1, &items[n], err))
                return 0;
            n++;
            skip_ws(s, pos);
            if (*pos < s.len && s.ptr[*pos] == ',') {
                (*pos)++;
                continue;
            }
            if (*pos < s.len && s.ptr[*pos] == close) {
                (*pos)++;
                break;
            }
            return fail_at(err, *pos, object ? "expected ',' or '}'" : "expected ',' or ']'");
        }
    }
    if (n != count)
        abort();
    order = NULL;
    if (object && !order_keys(a, keys, n, &order, start, err))
        return 0;
    out->kind = object ? CU_JSON_OBJECT : CU_JSON_ARRAY;
    out->n = n;
    out->items = items;
    out->keys = keys;
    out->order = order;
    return 1;
}

static int literal(CuStr s, size_t *pos, const char *word, CuJsonError *err)
{
    size_t n;

    n = strlen(word);
    if (s.len - *pos < n || memcmp(s.ptr + *pos, word, n) != 0)
        return fail_at(err, *pos, "unexpected character");
    *pos += n;
    return 1;
}

static int value(CuArena *a, CuStr s, size_t *pos, size_t depth, CuJson *out, CuJsonError *err)
{
    char *dst;
    size_t n;
    char c;

    out->truth = 0;
    out->text = cu_str_empty();
    out->items = NULL;
    out->keys = NULL;
    out->order = NULL;
    out->n = 0;
    skip_ws(s, pos);
    if (*pos >= s.len)
        return fail_at(err, *pos, "expected a value");
    c = s.ptr[*pos];
    switch (c) {
    case '{':
    case '[':
        return container(a, s, pos, depth, out, err);
    case '"':
        out->kind = CU_JSON_STRING;
        return string(a, s, pos, &out->text, err);
    case 'n':
        out->kind = CU_JSON_NULL;
        return literal(s, pos, "null", err);
    case 't':
        out->kind = CU_JSON_BOOL;
        out->truth = 1;
        return literal(s, pos, "true", err);
    case 'f':
        out->kind = CU_JSON_BOOL;
        return literal(s, pos, "false", err);
    default:
        break;
    }
    n = number_len((const unsigned char *)s.ptr + *pos, s.len - *pos);
    if (n == 0)
        return fail_at(err, *pos, "unexpected character");
    dst = cu_arena_push_bytes(a, n);
    if (dst == NULL)
        return fail_at(err, *pos, "out of memory");
    memcpy(dst, s.ptr + *pos, n);
    out->kind = CU_JSON_NUMBER;
    out->text = cu_str_n(dst, n);
    *pos += n;
    return 1;
}

int cu_json_parse(CuArena *a, CuStr src, CuJson *out, CuJsonError *err)
{
    CuJson v;
    size_t pos;

    if (a == NULL || out == NULL || err == NULL || (src.ptr == NULL && src.len != 0))
        abort();
    err->offset = 0;
    err->reason = NULL;
    pos = 0;
    if (!value(a, src, &pos, 0, &v, err))
        return 0;
    skip_ws(src, &pos);
    if (pos != src.len)
        return fail_at(err, pos, "bytes after the value");
    *out = v;
    return 1;
}

const CuJson *cu_json_find(const CuJson *obj, CuStr key)
{
    size_t lo;
    size_t hi;
    size_t mid;
    int c;

    if (obj == NULL || obj->kind != CU_JSON_OBJECT)
        abort();
    lo = 0;
    hi = obj->n;
    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        c = compare_bytes(obj->keys[obj->order[mid]], key);
        if (c == 0)
            return &obj->items[obj->order[mid]];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

int cu_json_to_u64(const CuJson *v, CuU64 *out)
{
    CuU64 x;
    CuU64 x2;
    CuU64 x8;
    size_t i;
    char c;

    if (v == NULL || out == NULL)
        abort();
    if (v->kind != CU_JSON_NUMBER || v->text.len == 0)
        return 0;
    x = cu_u64(0, 0);
    for (i = 0; i < v->text.len; i++) {
        c = v->text.ptr[i];
        if (c < '0' || c > '9')
            return 0;
        /* x * 10 + digit, each step checked. */
        if (!cu_u64_add(x, x, &x2) || !cu_u64_add(x2, x2, &x8) || !cu_u64_add(x8, x8, &x8) ||
            !cu_u64_add(x8, x2, &x) || !cu_u64_add(x, cu_u64(0, (unsigned long)(c - '0')), &x))
            return 0;
    }
    *out = x;
    return 1;
}

int cu_json_to_long(const CuJson *v, long *out)
{
    if (v == NULL || out == NULL)
        abort();
    if (v->kind != CU_JSON_NUMBER)
        return 0;
    return cu_parse_long(v->text, 10, out);
}

/* ---- Writer ---- */

void cu_json_out_init(CuJsonOut *o, CuArena *a)
{
    if (o == NULL || a == NULL)
        abort();
    cu_sb_init(&o->b, a);
    CU_VEC_INIT(&o->keys, a, size_t);
    o->depth = 0;
    o->want_value = 0;
    o->done = 0;
    o->failed = 0;
}

/* Where a value may go: after a key in an object, anywhere in an array,
 * or at the top once. Writes the comma an array needs. */
static void before_value(CuJsonOut *o)
{
    size_t top;

    if (o == NULL || o->done)
        abort();
    if (o->depth == 0)
        return;
    top = o->depth - 1;
    if (o->object[top]) {
        if (!o->want_value)
            abort();
        o->want_value = 0;
        return;
    }
    if (o->items[top])
        cu_sb_char(&o->b, ',');
    o->items[top] = 1;
}

static void after_value(CuJsonOut *o)
{
    if (o->depth == 0)
        o->done = 1;
}

static void begin(CuJsonOut *o, int object)
{
    before_value(o);
    if (o->depth == CU_JSON_MAX_DEPTH)
        abort();
    o->object[o->depth] = (unsigned char)object;
    o->items[o->depth] = 0;
    o->first[o->depth] = cu_vec_len(&o->keys);
    o->depth++;
    cu_sb_char(&o->b, object ? '{' : '[');
}

/* Fails the writer if the object at level depth repeated a key, then
 * forgets its keys. */
static void check_keys(CuJsonOut *o, size_t depth)
{
    size_t *at;
    size_t first;
    size_t n;
    size_t i;

    first = o->first[depth];
    n = cu_vec_len(&o->keys) - first;
    if (n > 1 && !o->b.failed && !cu_vec_failed(&o->keys)) {
        at = CU_VEC_DATA(&o->keys, size_t) + first;
        heap_sort(at, n, NULL, o->b.data);
        for (i = 1; i < n; i++)
            if (cu_str_eq(quoted_at(o->b.data, at[i - 1]), quoted_at(o->b.data, at[i])))
                o->failed = 1;
    }
    while (cu_vec_len(&o->keys) > first && CU_VEC_POP(&o->keys, &i))
        continue;
}

static void end(CuJsonOut *o, int object)
{
    if (o == NULL || o->depth == 0 || o->object[o->depth - 1] != object || o->want_value)
        abort();
    o->depth--;
    if (object)
        check_keys(o, o->depth);
    cu_sb_char(&o->b, object ? '}' : ']');
    after_value(o);
}

void cu_json_begin_object(CuJsonOut *o)
{
    begin(o, 1);
}

void cu_json_end_object(CuJsonOut *o)
{
    end(o, 1);
}

void cu_json_begin_array(CuJsonOut *o)
{
    begin(o, 0);
}

void cu_json_end_array(CuJsonOut *o)
{
    end(o, 0);
}

/* s quoted and escaped; bytes that are not UTF-8 fail the writer. */
static void quote(CuJsonOut *o, CuStr s)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *u;
    size_t i;
    size_t len;
    unsigned char c;

    if (s.ptr == NULL && s.len != 0)
        abort();
    u = (const unsigned char *)s.ptr;
    cu_sb_char(&o->b, '"');
    for (i = 0; i < s.len; i += len) {
        c = u[i];
        len = 1;
        if (c == '"' || c == '\\') {
            cu_sb_char(&o->b, '\\');
            cu_sb_char(&o->b, (char)c);
        } else if (c == '\n') {
            cu_sb_str(&o->b, cu_str_cstr("\\n"));
        } else if (c == '\r') {
            cu_sb_str(&o->b, cu_str_cstr("\\r"));
        } else if (c == '\t') {
            cu_sb_str(&o->b, cu_str_cstr("\\t"));
        } else if (c < 0x20) {
            cu_sb_str(&o->b, cu_str_cstr("\\u00"));
            cu_sb_char(&o->b, hex[c >> 4]);
            cu_sb_char(&o->b, hex[c & 0xF]);
        } else {
            len = utf8_len(u + i, s.len - i);
            if (len == 0) {
                o->failed = 1;
                break;
            }
            cu_sb_str(&o->b, cu_str_n(s.ptr + i, len));
        }
    }
    cu_sb_char(&o->b, '"');
}

void cu_json_key(CuJsonOut *o, CuStr key)
{
    size_t top;

    if (o == NULL || o->depth == 0 || !o->object[o->depth - 1] || o->want_value)
        abort();
    top = o->depth - 1;
    if (o->items[top])
        cu_sb_char(&o->b, ',');
    o->items[top] = 1;
    CU_VEC_PUSH(&o->keys, &o->b.len);
    quote(o, key);
    cu_sb_char(&o->b, ':');
    o->want_value = 1;
}

void cu_json_string(CuJsonOut *o, CuStr s)
{
    before_value(o);
    quote(o, s);
    after_value(o);
}

void cu_json_u64(CuJsonOut *o, CuU64 v)
{
    char digits[20];
    unsigned long hi;
    unsigned long lo;
    unsigned long t;
    unsigned long q1;
    unsigned long r;
    size_t n;

    before_value(o);
    /* Long division by 10 in 16-bit steps, so no step exceeds 32 bits. */
    v = cu_u64(v.hi, v.lo);
    hi = v.hi;
    lo = v.lo;
    n = 0;
    do {
        r = hi % 10;
        hi /= 10;
        t = r << 16 | lo >> 16;
        q1 = t / 10;
        t = (t % 10) << 16 | (lo & 0xFFFF);
        lo = q1 << 16 | t / 10;
        digits[n++] = (char)('0' + t % 10);
    } while (hi != 0 || lo != 0);
    while (n > 0)
        cu_sb_char(&o->b, digits[--n]);
    after_value(o);
}

void cu_json_long(CuJsonOut *o, long v)
{
    before_value(o);
    cu_sb_long(&o->b, v);
    after_value(o);
}

void cu_json_number(CuJsonOut *o, CuStr text)
{
    if (text.ptr == NULL && text.len != 0)
        abort();
    before_value(o);
    if (text.len == 0 || number_len((const unsigned char *)text.ptr, text.len) != text.len) {
        o->failed = 1;
        cu_sb_char(&o->b, '0');
    } else {
        cu_sb_str(&o->b, text);
    }
    after_value(o);
}

void cu_json_bool(CuJsonOut *o, int v)
{
    before_value(o);
    cu_sb_str(&o->b, cu_str_cstr(v ? "true" : "false"));
    after_value(o);
}

void cu_json_null(CuJsonOut *o)
{
    before_value(o);
    cu_sb_str(&o->b, cu_str_cstr("null"));
    after_value(o);
}

int cu_json_out_finish(CuJsonOut *o, CuStr *out)
{
    int ok;

    if (o == NULL || out == NULL || !o->done)
        abort();
    ok = cu_sb_finish(&o->b, out);
    if (!ok || o->failed || cu_vec_failed(&o->keys)) {
        *out = cu_str_empty();
        return 0;
    }
    return 1;
}
