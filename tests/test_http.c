#include "death.h"
#include "cu.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

/* Worked wire oracles: the first head ends before binary body, and the
 * second request begins exactly three bytes later. No parser-derived oracle. */
static const char first[] = "POST /a?q HTTP/1.1\r\nhOsT: x\r\ncOnTeNt-LeNgTh: 003\r\n\r\n";
static const char second[] = "GET / HTTP/1.1\r\nHost: y\r\n\r\n";

static void empty(const CuHttpRequest *q)
{
    assert(q->method.len == 0 && q->target.len == 0 && q->head.len == 0);
    assert(q->body_length == 0 && q->remaining == 0);
}

static void probe(const char *wire, size_t len, size_t cap, size_t fields,
                  size_t body, CuHttpResult want)
{
    CuReader r;
    CuHttpRequest q;
    char head[1024], buf[2];
    size_t n, left;
    cu_reader_memory(&r, cu_str_n(wire, len));
    memset(&q, 0xa5, sizeof q);
    assert(cap < sizeof head);
    head[cap] = (char)0xa5;
    assert(cu_http_begin(&q, &r, head, cap, fields, body) == want);
    assert((unsigned char)head[cap] == 0xa5);
    if (want != CU_HTTP_OK) {
        empty(&q);
        left = r.source.memory.len;
        assert(cu_http_read(&q, buf, sizeof buf, &n) == want && n == 0);
        assert(r.source.memory.len == left);
    }
}

static void boundaries(void)
{
    char wire[256], head[128], slab[2];
    CuReader r;
    CuHttpRequest q;
    size_t h, n, left;
    h = sizeof first - 1;
    memcpy(wire, first, h);
    memcpy(wire + h, "\0\xffZ", 3);
    memcpy(wire + h + 3, second, sizeof second - 1);
    cu_reader_memory(&r, cu_str_n(wire, h + 3 + sizeof second - 1));
    assert(cu_http_begin(&q, &r, head, h, 2, 3) == CU_HTTP_OK);
    assert(q.head.len == h && q.body_length == 3 && q.remaining == 3);
    assert(cu_str_eq(q.method, CU_LIT("POST")));
    assert(cu_str_eq(q.target, CU_LIT("/a?q")));
    assert(r.source.memory.len == 3 + sizeof second - 1);
    assert(cu_http_read(&q, slab, 2, &n) == CU_HTTP_OK && n == 2);
    assert(slab[0] == 0 && (unsigned char)slab[1] == 255);
    assert(cu_http_read(&q, slab, 2, &n) == CU_HTTP_OK && n == 1 && slab[0] == 'Z');
    left = r.source.memory.len;
    assert(left == sizeof second - 1);
    assert(cu_http_read(&q, slab, 2, &n) == CU_HTTP_END && n == 0);
    assert(cu_http_read(&q, slab, 2, &n) == CU_HTTP_END && r.source.memory.len == left);
    assert(cu_http_begin(&q, &r, head, sizeof head, 1, 0) == CU_HTTP_OK);
    assert(q.remaining == 0 && r.source.memory.len == 0);
    probe(first, h, h - 1, 2, 3, CU_HTTP_LIMIT);
    probe(first, h, h, 1, 3, CU_HTTP_LIMIT);
    probe(first, h, h, 2, 2, CU_HTTP_LIMIT);
    probe(second, sizeof second - 1, sizeof head, 0, 0, CU_HTTP_LIMIT);
    cu_reader_memory(&r, CU_LIT("POST / HTTP/1.1\r\nHost: x\r\n\r\nEXTRA"));
    assert(cu_http_begin(&q, &r, head, sizeof head, 1, 0) == CU_HTTP_OK);
    assert(q.body_length == 0 && r.source.memory.len == 5);
    assert(cu_http_read(&q, slab, sizeof slab, &n) == CU_HTTP_END);
    assert(r.source.memory.len == 5);
    probe("GET / HTTP/1.1\r\nHost: \tx \t\r\nX-Foo: \tvalue\r\nContent-Length: 000\r\n\r\n",
          sizeof("GET / HTTP/1.1\r\nHost: \tx \t\r\nX-Foo: \tvalue\r\nContent-Length: 000\r\n\r\n") - 1,
          sizeof head, 3, 0, CU_HTTP_OK);
}

/* A long field value must be scanned linearly and must not write at cap.
 * Cerberus explores the same shape at a smaller explicit bound. */
static void long_field(void)
{
    static const char prefix[] = "GET / HTTP/1.1\r\nHost: not a validated authority /\r\nX-Long: ";
#ifdef __cerb__
    char wire[128], head[129];
#else
    char wire[4096], head[4097];
#endif
    CuReader reader;
    CuHttpRequest request;
    size_t i;

    memcpy(wire, prefix, sizeof prefix - 1);
    for (i = sizeof prefix - 1; i < sizeof wire - 4; i++) wire[i] = 'x';
    memcpy(wire + sizeof wire - 4, "\r\n\r\n", 4);
    head[sizeof wire] = (char)0xa5;
    cu_reader_memory(&reader, cu_str_n(wire, sizeof wire));
    assert(cu_http_begin(&request, &reader, head, sizeof wire, 2, 0) == CU_HTTP_OK);
    assert(request.head.len == sizeof wire && reader.source.memory.len == 0);
    assert((unsigned char)head[sizeof wire] == 0xa5);
}

static void truncations(void)
{
    char wire[128], head[128], buf[8];
    size_t i, h, n, total;
    CuReader r;
    CuHttpRequest q;
    h = sizeof first - 1;
    memcpy(wire, first, h);
    memcpy(wire + h, "abc", 3);
    for (i = 0; i < h + 3; i++) {
        cu_reader_memory(&r, cu_str_n(wire, i));
        if (i < h) {
            assert(cu_http_begin(&q, &r, head, sizeof head, 2, 3) ==
                   (i == 0 ? CU_HTTP_END : CU_HTTP_TRUNCATED));
            empty(&q);
        } else {
            assert(cu_http_begin(&q, &r, head, sizeof head, 2, 3) == CU_HTTP_OK);
            total = 0;
            while (cu_http_read(&q, buf, sizeof buf, &n) == CU_HTTP_OK) total += n;
            assert(total == i - h && q.state == CU_HTTP_TRUNCATED && n == 0);
            assert(cu_http_read(&q, buf, sizeof buf, &n) == CU_HTTP_TRUNCATED);
        }
    }
}

static void malformed(void)
{
    static const char *bad[] = {
        "Host: x\r\nContent-Length: 1\r\nContent-Length: 1",
        "Host: x\r\nContent-Length: 1\r\nContent-Length: 2",
        "Host: x\r\nContent-Length: 1,1", "Host: x\r\nContent-Length:",
        "Host: x\r\nContent-Length: +1", "Host: x\r\nContent-Length: 1 0",
        "Host: x\r\nContent-Length: 99999999999999999999999999999999",
        "Host : x", "Host: x\r\n folded: x", "Host: x\nX: y",
        "Host: x\rX: y", "Host:", "Host: x\r\nhost: y", "X: y",
        "Host: x\r\n@: y", "Host: x\r\nX: \177", "Host: x\r\nX: \200"
    };
    static const char *unsupported[] = {
        "Host: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 1",
        "Host: x\r\nContent-Length: 1\r\ntRaNsFeR-EnCoDiNg: identity",
        "Host: x\r\nTransfer-Encoding:", "Host: x\r\nTransfer-Encoding: nonsense",
        "Host: x\r\nExpect: 100-continue", "Host: x\r\nUpgrade: h2c"
    };
    static const char *lines[] = {
        " GET / HTTP/1.1", "G@T / HTTP/1.1", "GET * HTTP/1.1",
        "GET /#x HTTP/1.1", "GET / HTTP/1.0", "GET / x HTTP/1.1",
        "GET /\t HTTP/1.1", "GET /\200 HTTP/1.1", "\r\nGET / HTTP/1.1"
    };
    char wire[512];
    size_t i;
    for (i = 0; i < sizeof bad / sizeof *bad; i++) {
        sprintf(wire, "GET / HTTP/1.1\r\n%s\r\n\r\n", bad[i]);
        probe(wire, strlen(wire), sizeof wire, 20, (size_t)-1, CU_HTTP_BAD_REQUEST);
    }
    for (i = 0; i < sizeof unsupported / sizeof *unsupported; i++) {
        sprintf(wire, "GET / HTTP/1.1\r\n%s\r\n\r\n", unsupported[i]);
        probe(wire, strlen(wire), sizeof wire, 20, 20, CU_HTTP_UNSUPPORTED);
    }
    for (i = 0; i < sizeof lines / sizeof *lines; i++) {
        sprintf(wire, "%s\r\nHost: x\r\n\r\n", lines[i]);
        probe(wire, strlen(wire), sizeof wire, 20, 20, CU_HTTP_BAD_REQUEST);
    }
    probe("CONNECT / HTTP/1.1\r\nHost: x\r\n\r\n", 31, 512, 20, 20, CU_HTTP_UNSUPPORTED);
    memcpy(wire, second, sizeof second);
    wire[5] = 0;
    probe(wire, sizeof second - 1, 512, 20, 20, CU_HTTP_BAD_REQUEST);
}

static void io_errors(void)
{
    CuReader r;
    CuHttpRequest q;
    char head[128], buf[1];
    size_t n;
    int e;
    for (e = 0; e <= 1; e++) {
        cu_reader_memory(&r, CU_LIT(""));
        r.state = CU_INPUT_ERROR;
        r.error = e ? EIO : 0;
        assert(cu_http_begin(&q, &r, head, sizeof head, 2, 3) == CU_HTTP_IO);
        empty(&q);
        errno = ERANGE;
        assert(cu_http_read(&q, buf, 1, &n) == CU_HTTP_IO && n == 0);
        assert(errno == r.error && cu_http_errno(&q) == r.error);
        cu_reader_memory(&r, cu_str_n(first, sizeof first - 1));
        assert(cu_http_begin(&q, &r, head, sizeof head, 2, 3) == CU_HTTP_OK);
        r.state = CU_INPUT_ERROR;
        r.error = e ? EIO : 0;
        assert(cu_http_read(&q, buf, 1, &n) == CU_HTTP_IO && n == 0);
        errno = ERANGE;
        assert(cu_http_read(&q, buf, 1, &n) == CU_HTTP_IO && errno == r.error);
    }
}
#ifdef CU_DEATH_TESTS
static void bad_begin(void)
{
    CuReader r;
    CuHttpRequest q;
    char h[1];
    CuHttpResult result;
    cu_reader_memory(&r, CU_LIT(""));
    result = cu_http_begin(&q, &r, h, 0, 1, 1);
    (void)result;
}
static void bad_read(void)
{
    CuHttpRequest q;
    CuReader r;
    char h[128], b[1];
    size_t n;
    CuHttpResult result;
    cu_reader_memory(&r, CU_LIT("GET / HTTP/1.1\r\nHost: x\r\n\r\n"));
    result = cu_http_begin(&q, &r, h, sizeof h, 1, 0);
    assert(result == CU_HTTP_OK);
    result = cu_http_read(&q, b, 0, &n);
    (void)result;
}
#endif
int main(void)
{
    boundaries(); long_field(); truncations(); malformed(); io_errors();
#ifdef CU_DEATH_TESTS
    expect_abort(bad_begin); expect_abort(bad_read);
#endif
    return 0;
}
