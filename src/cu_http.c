#include "cu.h"
#include <stdlib.h>
#include <errno.h>

static int http_token(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int http_name(const char *s, size_t n, const char *name)
{
    size_t i;
    if (n != strlen(name)) return 0;
    for (i = 0; i < n; i++) {
        if (cu_ascii_to_lower(s[i]) != name[i]) return 0;
    }
    return 1;
}

static CuHttpResult http_state(CuHttpRequest *r, CuHttpResult state)
{
    r->state = state;
    if (state == CU_HTTP_IO) {
        r->error = cu_reader_errno(r->reader);
        errno = r->error;
    }
    return state;
}

/* Each byte is scanned a constant number of times. The acquisition loop
 * deliberately reads single bytes: it cannot consume a pipelined request. */
CuHttpResult cu_http_begin(CuHttpRequest *r, CuReader *reader, char *head,
                           size_t cap, size_t max_fields, size_t max_body)
{
    size_t used, n, end, p, m, t, target_end, start, colon, a, b;
    size_t fields, length, digit;
    int host, cl, unsupported, too_large;
    unsigned char c;
    CuInputStatus status;
    if (r == NULL || reader == NULL || head == NULL || cap == 0) abort();
    r->reader = reader;
    r->method = cu_str_empty();
    r->target = cu_str_empty();
    r->head = cu_str_empty();
    r->body_length = 0;
    r->remaining = 0;
    r->state = CU_HTTP_OK;
    r->error = 0;
    used = 0;
    for (;;) {
        if (used == cap) return http_state(r, CU_HTTP_LIMIT);
        status = cu_reader_read(reader, head + used, 1, &n);
        if (status == CU_INPUT_ERROR) return http_state(r, CU_HTTP_IO);
        if (status == CU_INPUT_END)
            return http_state(r, used == 0 ? CU_HTTP_END : CU_HTTP_TRUNCATED);
        c = (unsigned char)head[used];
        if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c >= 127)
            return http_state(r, CU_HTTP_BAD_REQUEST);
        if ((c == '\n' && (used == 0 || head[used - 1] != '\r')) ||
            (used != 0 && head[used - 1] == '\r' && c != '\n'))
            return http_state(r, CU_HTTP_BAD_REQUEST);
        used++;
        if (used >= 4 && memcmp(head + used - 4, "\r\n\r\n", 4) == 0) break;
    }
    end = 0;
    while (head[end] != '\r') end++;
    p = 0;
    while (p < end && http_token((unsigned char)head[p])) p++;
    m = p;
    if (m == 0 || p == end || head[p++] != ' ')
        return http_state(r, CU_HTTP_BAD_REQUEST);
    t = p;
    if (p == end || head[p] != '/') return http_state(r, CU_HTTP_BAD_REQUEST);
    while (p < end && head[p] != ' ') {
        c = (unsigned char)head[p++];
        if (c <= 32 || c >= 127 || c == '#') return http_state(r, CU_HTTP_BAD_REQUEST);
    }
    target_end = p;
    if (p == end || end - p != 9 || memcmp(head + p, " HTTP/1.1", 9) != 0)
        return http_state(r, CU_HTTP_BAD_REQUEST);
    unsupported = m == 7 && memcmp(head, "CONNECT", 7) == 0;
    p = end + 2;
    host = cl = too_large = 0;
    fields = length = 0;
    while (p < used - 2) {
        if (fields == max_fields) return http_state(r, CU_HTTP_LIMIT);
        fields++;
        start = p;
        while (p < used && http_token((unsigned char)head[p])) p++;
        colon = p;
        if (p == start || p == used || head[p++] != ':')
            return http_state(r, CU_HTTP_BAD_REQUEST);
        a = p;
        while (head[p] != '\r') {
            c = (unsigned char)head[p++];
            if (c != '\t' && (c < 32 || c >= 127))
                return http_state(r, CU_HTTP_BAD_REQUEST);
        }
        b = p;
        p += 2;
        while (a < b && (head[a] == ' ' || head[a] == '\t')) a++;
        while (b > a && (head[b - 1] == ' ' || head[b - 1] == '\t')) b--;
        if (http_name(head + start, colon - start, "host")) {
            if (host || a == b) return http_state(r, CU_HTTP_BAD_REQUEST);
            host = 1;
        } else if (http_name(head + start, colon - start, "content-length")) {
            if (cl || a == b) return http_state(r, CU_HTTP_BAD_REQUEST);
            cl = 1;
            for (; a < b; a++) {
                if (head[a] < '0' || head[a] > '9')
                    return http_state(r, CU_HTTP_BAD_REQUEST);
                digit = (size_t)(head[a] - '0');
                if (length > ((size_t)-1 - digit) / 10)
                    return http_state(r, CU_HTTP_BAD_REQUEST);
                length = length * 10 + digit;
            }
            too_large = length > max_body;
        } else if (http_name(head + start, colon - start, "transfer-encoding") ||
                   http_name(head + start, colon - start, "expect") ||
                   http_name(head + start, colon - start, "upgrade")) {
            unsupported = 1;
        }
    }
    if (!host) return http_state(r, CU_HTTP_BAD_REQUEST);
    if (unsupported) return http_state(r, CU_HTTP_UNSUPPORTED);
    if (too_large) return http_state(r, CU_HTTP_LIMIT);
    r->method = cu_str_n(head, m);
    r->target = cu_str_n(head + t, target_end - t);
    r->head = cu_str_n(head, used);
    r->body_length = r->remaining = length;
    return CU_HTTP_OK;
}

CuHttpResult cu_http_read(CuHttpRequest *r, void *buf, size_t cap, size_t *n)
{
    CuInputStatus status;
    if (r == NULL || buf == NULL || cap == 0 || n == NULL) abort();
    *n = 0;
    if (r->state != CU_HTTP_OK) {
        if (r->state == CU_HTTP_IO) errno = r->error;
        return r->state;
    }
    if (r->remaining == 0) return http_state(r, CU_HTTP_END);
    if (cap > r->remaining) cap = r->remaining;
    status = cu_reader_read(r->reader, buf, cap, n);
    if (status == CU_INPUT_ERROR) return http_state(r, CU_HTTP_IO);
    if (status == CU_INPUT_END) return http_state(r, CU_HTTP_TRUNCATED);
    r->remaining -= *n;
    return CU_HTTP_OK;
}

int cu_http_errno(const CuHttpRequest *r)
{
    if (r == NULL) abort();
    return r->error;
}
