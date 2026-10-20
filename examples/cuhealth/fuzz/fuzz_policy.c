#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cuhealth.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char number[129];
    char wire[1024];
    size_t i;
    size_t n;
    unsigned long value;
    unsigned long expected;
    int valid;
    if (size > 128) size = 128;
    for (i = 0; i < size; ++i) number[i] = (char)data[i];
    number[size] = '\0';
    expected = 0;
    valid = number[0] != '\0';
    for (i = 0; number[i] != '\0'; ++i) {
        if (number[i] < '0' || number[i] > '9') { valid = 0; break; }
        expected = expected * 10 + (unsigned long)(number[i] - '0');
        if (expected > 1000000) { valid = 0; break; }
    }
    value = 123;
    assert(health_number(number, 1000000, &value) == valid);
    if (valid) assert(value == expected);
    else assert(value == 123);
    assert(health_wire(wire, sizeof wire, &n,
                       cu_str_cstr(size % 2 ? "HEAD" : "GET"),
                       cu_str_cstr("/status"), 1, (int)(size % 3) - 1,
                       "Thu, 01 Jan 1970 00:00:00 GMT"));
    assert(n <= sizeof wire);
    assert(memcmp(wire, "HTTP/1.1 ", 9) == 0);
    return 0;
}
