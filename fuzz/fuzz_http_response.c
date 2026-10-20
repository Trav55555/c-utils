/* Pure head framing, capacity and failure-atomicity checks; no network state. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const unsigned codes[] = {200, 204, 205, 304, 503, 199, 600, 299};
    CuHttpHeader fields[4];
    unsigned char *storage;
    CuHttpResponseResult result;
    size_t pos;
    size_t count;
    size_t cap;
    size_t length;
    size_t n;
    size_t i;
    size_t take;
    unsigned status;
    char view[1025];
    char expected[sizeof(size_t) * 8 + 64];
    char *found;

    if (size < 3)
        return 0;
    if (size > 4096)
        size = 4096;
    status = codes[data[0] % (sizeof codes / sizeof codes[0])];
    cap = (size_t)data[1] * 4;
    count = data[2] % 5;
    pos = 3;
    length = 0;
    for (i = 0; i < sizeof(size_t) && pos < size; i++)
        length = length * 256 + data[pos++];
    for (i = 0; i < count; i++) {
        take = pos < size ? data[pos++] : 0;
        if (take > size - pos)
            take = size - pos;
        fields[i].name = cu_str_n((const char *)data + pos, take);
        pos += take;
        take = pos < size ? data[pos++] : 0;
        if (take > size - pos)
            take = size - pos;
        fields[i].value = cu_str_n((const char *)data + pos, take);
        pos += take;
    }
    storage = (unsigned char *)malloc(cap + 2);
    assert(storage != NULL);
    memset(storage, 0xa5, cap + 2);
    n = 99;
    result = cu_http_response_head((char *)storage + 1, cap, status, length, fields, count, &n);
    assert(storage[0] == 0xa5 && storage[cap + 1] == 0xa5);
    if (result == CU_HTTP_RESPONSE_OK) {
        assert(n <= cap && n < sizeof view && n >= 16);
        assert(status >= 200 && status <= 599);
        memcpy(view, storage + 1, n);
        view[n] = '\0';
        assert(strlen(view) == n);
        assert(memcmp(view + n - 4, "\r\n\r\n", 4) == 0);
        (void)sprintf(expected, "HTTP/1.1 %u ", status);
        assert(strncmp(view, expected, strlen(expected)) == 0);
        found = strstr(view, "\r\nConnection: close\r\n");
        assert(found != NULL && strstr(found + 2, "\r\nConnection:") == NULL);
        if (status == 204 || status == 304) {
            assert(length == 0 && strstr(view, "\r\nContent-Length:") == NULL);
        } else {
            (void)sprintf(expected, "\r\nContent-Length: %zu\r\n", length);
            found = strstr(view, expected);
            assert(found != NULL && strstr(found + 2, "\r\nContent-Length:") == NULL);
            if (status == 205)
                assert(length == 0);
        }
        for (i = n + 1; i < cap + 2; i++)
            assert(storage[i] == 0xa5);
    } else {
        assert(result == CU_HTTP_RESPONSE_INVALID || result == CU_HTTP_RESPONSE_LIMIT);
        assert(n == 0);
        for (i = 0; i < cap + 2; i++)
            assert(storage[i] == 0xa5);
    }
    free(storage);
    return 0;
}
