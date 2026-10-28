/* Bounded memory-only production framing and cursor/accounting checks. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cu.h"

static int same_name(const uint8_t *s, size_t n, const char *name)
{
    size_t i;
    if (n != strlen(name)) return 0;
    for (i = 0; i < n; i++)
        if (cu_ascii_to_lower((char)s[i]) != name[i]) return 0;
    return 1;
}

/* Independent model of RFC 9112 6.1/6.3 over a complete head that the
 * parser read to its end: split the lines, split each Transfer-Encoding
 * value at commas, trim OWS and drop empty elements. Returns 0 without
 * Transfer-Encoding, 1 for chunked exactly once and last without
 * Content-Length, and -1 otherwise (a 400). */
static int te_model(const uint8_t *h, size_t len)
{
    size_t line, eol, colon, a, b, e;
    size_t elements = 0, chunked = 0;
    int te = 0, cl = 0, last = 0;
    line = 0;
    while (h[line] != '\r') line++;
    line += 2;
    while (line + 2 < len) {
        eol = line;
        while (h[eol] != '\r') eol++;
        colon = line;
        while (h[colon] != ':') colon++;
        if (same_name(h + line, colon - line, "content-length")) cl = 1;
        if (same_name(h + line, colon - line, "transfer-encoding")) {
            te = 1;
            for (a = colon + 1; a <= eol; a = e + 1) {
                e = a;
                while (e < eol && h[e] != ',') e++;
                b = e;
                while (a < b && (h[a] == ' ' || h[a] == '\t')) a++;
                while (b > a && (h[b - 1] == ' ' || h[b - 1] == '\t')) b--;
                if (a == b) continue;
                elements++;
                last = same_name(h + a, b - a, "chunked");
                if (last) chunked++;
            }
        }
        line = eol + 2;
    }
    if (!te) return 0;
    return elements > 0 && last && chunked == 1 && !cl ? 1 : -1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuReader reader;
    CuHttpRequest q;
    CuHttpResult result, again;
    char head[512], slab[17];
    size_t cap, fields, max_body, left, n, total, before, offset;
    if (size > 4096) size = 4096;
    cap = size ? 1 + (size_t)data[0] * 2 : sizeof head;
    fields = size > 1 ? data[1] : 16;
    max_body = size > 2 ? data[2] : 256;
    cu_reader_memory(&reader, cu_str_n((const char *)data, size));
    result = cu_http_begin(&q, &reader, head, cap, fields, max_body);
    left = reader.source.memory.len;
    if (left > size) __builtin_trap();
    /* OK and UNSUPPORTED are reached only after every field line is read:
     * neither may hide a malformed Transfer-Encoding, and only a chunked
     * one is unsupported rather than accepted. */
    if ((result == CU_HTTP_OK || result == CU_HTTP_UNSUPPORTED) &&
        (te_model(data, size - left) < 0 ||
         (result == CU_HTTP_OK && te_model(data, size - left) != 0)))
        __builtin_trap();
    if (result == CU_HTTP_OK) {
        if (q.head.ptr != head || q.head.len > cap || q.head.len < 4 ||
            q.head.len != size - left || q.body_length > max_body ||
            q.remaining != q.body_length || q.method.ptr != head ||
            q.method.len == 0 || q.target.len == 0 ||
            q.method.len > q.head.len ||
            q.target.ptr < head || q.target.ptr > head + q.head.len ||
            q.target.len > (size_t)(head + q.head.len - q.target.ptr) ||
            memcmp(head + q.head.len - 4, "\r\n\r\n", 4) != 0)
            __builtin_trap();
        total = 0;
        offset = size - left;
        do {
            before = reader.source.memory.len;
            result = cu_http_read(&q, slab, sizeof slab, &n);
            if (result == CU_HTTP_OK) {
                if (n == 0 || n > sizeof slab || n > q.body_length - total ||
                    n > before || reader.source.memory.len != before - n ||
                    memcmp(slab, data + offset + total, n) != 0)
                    __builtin_trap();
                total += n;
                if (q.remaining != q.body_length - total) __builtin_trap();
            } else if (n != 0) __builtin_trap();
        } while (result == CU_HTTP_OK);
        if (result == CU_HTTP_END && total != q.body_length) __builtin_trap();
        if (result == CU_HTTP_TRUNCATED &&
            (total >= q.body_length || reader.source.memory.len != 0))
            __builtin_trap();
    } else if (q.method.len || q.target.len || q.head.len ||
               q.body_length || q.remaining) __builtin_trap();
    before = reader.source.memory.len;
    again = cu_http_read(&q, slab, sizeof slab, &n);
    if (again != result || n != 0 || reader.source.memory.len != before)
        __builtin_trap();
    return 0;
}
