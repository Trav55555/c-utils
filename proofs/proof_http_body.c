/* Bounded CBMC property of the body stage after a valid head: 0..4 body
 * bytes, 1..2 byte caller buffers, and two pipelined sentinel bytes.
 * Does not claim to prove head parsing, socket deadlines or all inputs. */
#include "cu.h"

size_t nondet_size_t(void);
unsigned char nondet_uchar(void);

int main(void)
{
    CuReader reader;
    CuHttpRequest request;
    CuHttpResult result;
    char wire[6];
    char slab[2];
    size_t length;
    size_t cap;
    size_t n;
    size_t total;
    size_t i;
    size_t j;

    length = nondet_size_t();
    cap = nondet_size_t();
    __CPROVER_assume(length <= 4 && cap >= 1 && cap <= 2);
    for (i = 0; i < sizeof wire; i++) wire[i] = (char)nondet_uchar();
    wire[length] = 'Z';
    wire[length + 1] = 'Q';
    cu_reader_memory(&reader, cu_str_n(wire, length + 2));
    request.reader = &reader;
    request.method = cu_str_empty();
    request.target = cu_str_empty();
    request.head = cu_str_empty();
    request.body_length = length;
    request.remaining = length;
    request.state = CU_HTTP_OK;
    request.error = 0;
    total = 0;
    result = CU_HTTP_OK;
    for (i = 0; i < 5; i++) {
        result = cu_http_read(&request, slab, cap, &n);
        if (result == CU_HTTP_OK) {
            __CPROVER_assert(n >= 1 && n <= cap && n <= length - total,
                             "published body bytes are bounded");
            for (j = 0; j < n; j++)
                __CPROVER_assert(slab[j] == wire[total + j], "body bytes are exact");
            total += n;
        } else {
            __CPROVER_assert(result == CU_HTTP_END && n == 0,
                             "complete body has terminal zero-byte result");
        }
        __CPROVER_assert(request.remaining == length - total,
                         "remaining counts only published bytes");
        __CPROVER_assert(reader.source.memory.len == length + 2 - total,
                         "body read does not consume the next frame");
    }
    __CPROVER_assert(result == CU_HTTP_END && total == length, "body completes");
    __CPROVER_assert(cu_reader_read(&reader, slab, 2, &n) == CU_INPUT_DATA && n == 2,
                     "next frame remains available");
    __CPROVER_assert(slab[0] == 'Z' && slab[1] == 'Q', "next frame bytes unchanged");
    return 0;
}
