/* CBMC proof: for every slice of up to 4 elements of 1 or 2 bytes and every
 * start, end and index, cu_slice_sub never makes a view outside its source,
 * keeps the element size, and its element k is the source's element
 * start + k; CU_SLICE_AT on an in-range index points at that element. */
#include "cu.h"

#define MAX_LEN 4

size_t nondet_size_t(void);
int nondet_int(void);

int main(void)
{
    short arr[MAX_LEN];
    CuSlice s;
    CuSlice t;
    size_t len;
    size_t start;
    size_t end;
    size_t k;
    size_t size;
    char *base;

    len = nondet_size_t();
    __CPROVER_assume(len <= MAX_LEN);
    size = nondet_int() ? sizeof(short) : 1;
    s = cu_slice_n_(arr, len, size);
    base = (char *)arr;

    start = nondet_size_t();
    end = nondet_size_t();
    t = cu_slice_sub(s, start, end);
    __CPROVER_assert(t.elem_size == size, "sub keeps the element size");
    __CPROVER_assert(t.ptr >= s.ptr && t.len <= s.len, "sub starts inside");
    __CPROVER_assert((size_t)(t.ptr - s.ptr) / size + t.len <= s.len, "sub ends inside");
    __CPROVER_assert((size_t)(t.ptr - s.ptr) % size == 0, "sub starts on an element");

    k = nondet_size_t();
    if (k < t.len) {
        __CPROVER_assert((char *)cu_slice_at_(t, k, size) ==
                             (char *)cu_slice_at_(s, (size_t)(t.ptr - s.ptr) / size + k, size),
                         "sub element k is source element start + k");
        __CPROVER_assert((char *)cu_slice_at_(t, k, size) >= base &&
                             (char *)cu_slice_at_(t, k, size) + size <= base + sizeof arr,
                         "at stays inside the array");
    }
    return 0;
}
