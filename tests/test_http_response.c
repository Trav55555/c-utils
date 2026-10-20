#include "death.h"
#include "cu.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

static void literal(void)
{
    static const char expected[] =
        "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 5\r\n"
        "Content-Type: application/json\r\nX-Trace: a\tb\r\n\r\n";
    CuHttpHeader fields[2];
    unsigned char storage[sizeof expected + 4];
    size_t n;
    size_t i;
    CuHttpResponseResult result;

    fields[0].name = CU_LIT("Content-Type");
    fields[0].value = CU_LIT("application/json");
    fields[1].name = CU_LIT("X-Trace");
    fields[1].value = CU_LIT("a\tb");
    for (i = 0; i < sizeof storage; i++)
        storage[i] = 0xa5;
    errno = EDOM;
    result = cu_http_response_head((char *)storage + 1, sizeof expected - 1, 200, 5, fields, 2, &n);
    assert(result == CU_HTTP_RESPONSE_OK && n == sizeof expected - 1);
    assert(memcmp(storage + 1, expected, n) == 0 && errno == EDOM);
    assert(storage[0] == 0xa5 && storage[sizeof expected] == 0xa5);
    for (i = 0; i < sizeof storage; i++)
        storage[i] = 0xa5;
    n = 99;
    result = cu_http_response_head((char *)storage + 1, sizeof expected - 2, 200, 5, fields, 2, &n);
    assert(result == CU_HTTP_RESPONSE_LIMIT && n == 0 && errno == EDOM);
    for (i = 0; i < sizeof storage; i++)
        assert(storage[i] == 0xa5);
    assert(cu_http_response_head(NULL, 0, 200, 0, NULL, 0, &n) == CU_HTTP_RESPONSE_LIMIT && n == 0);
}

static void status_rules(void)
{
    char out[256];
    size_t n;
    unsigned status;
    size_t length;
    const char *expected;

    for (status = 200; status <= 599; status++) {
        length = status == 204 || status == 205 || status == 304 ? 0 : 17;
        assert(cu_http_response_head(out, sizeof out, status, length, NULL, 0, &n) == CU_HTTP_RESPONSE_OK);
        assert(n >= 4 && memcmp(out + n - 4, "\r\n\r\n", 4) == 0);
        assert(out[9] == (char)('0' + status / 100));
        assert(out[10] == (char)('0' + status / 10 % 10));
        assert(out[11] == (char)('0' + status % 10));
    }
    expected = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
    assert(cu_http_response_head(out, sizeof out, 204, 0, NULL, 0, &n) == CU_HTTP_RESPONSE_OK);
    assert(n == strlen(expected) && memcmp(out, expected, n) == 0);
    expected = "HTTP/1.1 304 Not Modified\r\nConnection: close\r\n\r\n";
    assert(cu_http_response_head(out, sizeof out, 304, 0, NULL, 0, &n) == CU_HTTP_RESPONSE_OK);
    assert(n == strlen(expected) && memcmp(out, expected, n) == 0);
    expected = "HTTP/1.1 205 Reset Content\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    assert(cu_http_response_head(out, sizeof out, 205, 0, NULL, 0, &n) == CU_HTTP_RESPONSE_OK);
    assert(n == strlen(expected) && memcmp(out, expected, n) == 0);
    expected = "HTTP/1.1 299 \r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    assert(cu_http_response_head(out, sizeof out, 299, 0, NULL, 0, &n) == CU_HTTP_RESPONSE_OK);
    assert(n == strlen(expected) && memcmp(out, expected, n) == 0);
}

static void reject(unsigned status, size_t length, const CuHttpHeader *fields, size_t count)
{
    char out[256];
    char before[256];
    size_t n;

    memset(out, 0xa5, sizeof out);
    memcpy(before, out, sizeof out);
    n = 99;
    errno = ERANGE;
    assert(cu_http_response_head(out, sizeof out, status, length, fields, count, &n) == CU_HTTP_RESPONSE_INVALID);
    assert(n == 0 && memcmp(out, before, sizeof out) == 0 && errno == ERANGE);
}

static void invalid(void)
{
    static const char *owned[] = {
        "Content-Length", "cOnTeNt-LeNgTh", "Transfer-Encoding", "CONNECTION", "Trailer",
        "Upgrade", "TE", "Keep-Alive", "Proxy-Connection"
    };
    static const char *names[] = {"", "bad:name", "bad name", "x\tname", "x\r\nInjected", "(x)"};
    static const char *values[] = {" leading", "trailing ", "\tleading", "trailing\t", "a\rb", "a\nb", "\001", "\177"};
    char high[2];
    char nul[3];
    CuHttpHeader field;
    size_t i;

    reject(0, 0, NULL, 0);
    reject(199, 0, NULL, 0);
    reject(600, 0, NULL, 0);
    reject(204, 1, NULL, 0);
    reject(205, 1, NULL, 0);
    reject(304, 1, NULL, 0);
    field.value = CU_LIT("value");
    for (i = 0; i < sizeof owned / sizeof owned[0]; i++) {
        field.name = cu_str_cstr(owned[i]);
        reject(200, 1, &field, 1);
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        field.name = cu_str_cstr(names[i]);
        reject(200, 1, &field, 1);
    }
    field.name = CU_LIT("X-Value");
    for (i = 0; i < sizeof values / sizeof values[0]; i++) {
        field.value = cu_str_cstr(values[i]);
        reject(200, 1, &field, 1);
    }
    high[0] = 'a'; high[1] = (char)128;
    field.value = cu_str_n(high, sizeof high);
    reject(200, 1, &field, 1);
    field.name = cu_str_n(high, sizeof high);
    field.value = CU_LIT("valid");
    reject(200, 1, &field, 1);
    nul[0] = 'a'; nul[1] = '\0'; nul[2] = 'b';
    field.name = CU_LIT("X-Value"); field.value = cu_str_n(nul, sizeof nul);
    reject(200, 1, &field, 1);
}

static void maximum_and_empty(void)
{
    CuHttpHeader fields[2];
    const char *maximum;
    const char *tail;
    char out[256];
    size_t n;
    size_t prefix;

    maximum = sizeof(size_t) == 2 ? "65535" : sizeof(size_t) == 4 ? "4294967295" : "18446744073709551615";
    if (sizeof(size_t) == 2 || sizeof(size_t) == 4 || sizeof(size_t) == 8) {
        assert(cu_http_response_head(out, sizeof out, 200, (size_t)-1, NULL, 0, &n) == CU_HTTP_RESPONSE_OK);
        prefix = strlen("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: ");
        assert(memcmp(out + prefix, maximum, strlen(maximum)) == 0);
        assert(n == prefix + strlen(maximum) + 4);
    }
    fields[0].name = CU_LIT("X-Empty"); fields[0].value = cu_str_n(NULL, 0);
    fields[1].name = CU_LIT("X-Empty"); fields[1].value = CU_LIT("ok");
    assert(cu_http_response_head(out, sizeof out, 200, 0, fields, 2, &n) == CU_HTTP_RESPONSE_OK);
    tail = "X-Empty: \r\nX-Empty: ok\r\n\r\n";
    assert(memcmp(out + n - strlen(tail), tail, strlen(tail)) == 0);
    fields[0].name = CU_LIT("X"); fields[0].value = CU_LIT("Content-Length:");
    fields[1].name = CU_LIT("X-Content-Length"); fields[1].value = CU_LIT("v");
    assert(cu_http_response_head(out, sizeof out, 204, 0, fields, 2, &n) == CU_HTTP_RESPONSE_OK);
    tail = "X: Content-Length:\r\nX-Content-Length: v\r\n\r\n";
    assert(memcmp(out + n - strlen(tail), tail, strlen(tail)) == 0);
}

#ifdef CU_DEATH_TESTS
static void null_written(void)
{
    char out[64];
    CuHttpResponseResult result;
    result = cu_http_response_head(out, sizeof out, 200, 0, NULL, 0, NULL);
    (void)result;
}
static void null_head(void)
{
    size_t n;
    CuHttpResponseResult result;
    result = cu_http_response_head(NULL, 1, 200, 0, NULL, 0, &n);
    (void)result;
}
static void null_fields(void)
{
    char out[64]; size_t n;
    CuHttpResponseResult result;
    result = cu_http_response_head(out, sizeof out, 200, 0, NULL, 1, &n);
    (void)result;
}
#endif

int main(void)
{
    literal(); status_rules(); invalid(); maximum_and_empty();
#ifdef CU_DEATH_TESTS
    expect_abort(null_written); expect_abort(null_head); expect_abort(null_fields);
#endif
    return 0;
}
