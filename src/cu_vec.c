#include <stdlib.h>
#include <string.h>
#include "cu.h"

/* Returned by CU_VEC_DATA before any storage exists, so it is never NULL.
 * Aligned for any element type; nothing is ever read from or written to it
 * because len is 0. */
static union {
    long l;
    double d;
    long double ld;
    void *p;
} empty_storage;

static void check_size(const CuVec *v, size_t elem_size)
{
    if (v == NULL || elem_size != v->elem_size)
        abort();
}

void cu_vec_init(CuVec *v, CuArena *a, size_t elem_size)
{
    if (v == NULL || a == NULL || elem_size == 0)
        abort();
    v->arena = a;
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem_size = elem_size;
    v->failed = 0;
}

int cu_vec_reserve(CuVec *v, size_t extra)
{
    size_t need;
    size_t cap;
    char *data;

    if (v == NULL)
        abort();
    if (!cu_size_add(v->len, extra, &need)) {
        v->failed = 1;
        return 0;
    }
    if (need <= v->cap)
        return 1;
    cap = v->cap != 0 ? v->cap : 4;
    while (cap < need) {
        if (!cu_size_mul(cap, 2, &cap)) {
            cap = need;
            break;
        }
    }
    data = (char *)cu_arena_push(v->arena, cap, v->elem_size);
    if (data == NULL) {
        v->failed = 1;
        return 0;
    }
    if (v->len != 0)
        memcpy(data, v->data, v->len * v->elem_size);
    v->data = data;
    v->cap = cap;
    return 1;
}

size_t cu_vec_len(const CuVec *v)
{
    if (v == NULL)
        abort();
    return v->len;
}

int cu_vec_failed(const CuVec *v)
{
    if (v == NULL)
        abort();
    return v->failed;
}

void cu_vec_clear(CuVec *v)
{
    if (v == NULL)
        abort();
    v->len = 0;
}

void cu_vec_push_(CuVec *v, const void *elem, size_t elem_size)
{
    check_size(v, elem_size);
    if (elem == NULL)
        abort();
    if (v->failed || !cu_vec_reserve(v, 1))
        return;
    memcpy(v->data + v->len * v->elem_size, elem, v->elem_size);
    v->len++;
}

int cu_vec_pop_(CuVec *v, void *out, size_t elem_size)
{
    check_size(v, elem_size);
    if (out == NULL)
        abort();
    if (v->len == 0)
        return 0;
    v->len--;
    memcpy(out, v->data + v->len * v->elem_size, v->elem_size);
    return 1;
}

void *cu_vec_at_(const CuVec *v, size_t i, size_t elem_size)
{
    check_size(v, elem_size);
    if (i >= v->len)
        abort();
    return v->data + i * v->elem_size;
}

void *cu_vec_data_(const CuVec *v, size_t elem_size)
{
    check_size(v, elem_size);
    if (v->data == NULL)
        return &empty_storage;
    return v->data;
}

CuSlice cu_vec_slice(const CuVec *v)
{
    CuSlice s;

    if (v == NULL)
        abort();
    s.ptr = v->data != NULL ? v->data : (char *)&empty_storage;
    s.len = v->len;
    s.elem_size = v->elem_size;
    return s;
}

CuSlice cu_slice_n_(void *p, size_t n, size_t elem_size)
{
    CuSlice s;
    size_t bytes;

    if (elem_size == 0 || !cu_size_mul(n, elem_size, &bytes))
        abort();
    if (p == NULL) {
        if (n != 0)
            abort();
        p = &empty_storage;
    }
    s.ptr = (char *)p;
    s.len = n;
    s.elem_size = elem_size;
    return s;
}

CuSlice cu_slice_sub(CuSlice s, size_t start, size_t end)
{
    if (end > s.len)
        end = s.len;
    if (start > end)
        start = end;
    s.ptr += start * s.elem_size;
    s.len = end - start;
    return s;
}

void *cu_slice_at_(CuSlice s, size_t i, size_t elem_size)
{
    if (elem_size != s.elem_size || i >= s.len)
        abort();
    return s.ptr + i * s.elem_size;
}

void *cu_slice_data_(CuSlice s, size_t elem_size)
{
    if (elem_size != s.elem_size)
        abort();
    return s.ptr;
}
