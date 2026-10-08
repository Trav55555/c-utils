/* CBMC proof: for every string of up to 4 bytes and every argument, slicing
 * never makes a view outside its source, find reports a real match that is
 * the first one, and cut splits around it. */
#include "cu.h"

#define MAX_LEN 4

size_t nondet_size_t(void);
char nondet_char(void);

/* CBMC has no body for memchr and would treat its result as arbitrary, so
 * the proof supplies a reference one. */
void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p;
    size_t i;

    p = (const unsigned char *)s;
    for (i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    }
    return NULL;
}

static int inside(CuStr outer, CuStr inner)
{
    return inner.ptr >= outer.ptr && inner.len <= outer.len &&
           (size_t)(inner.ptr - outer.ptr) <= outer.len - inner.len;
}

static int matches_at(CuStr s, CuStr needle, size_t at)
{
    size_t k;

    if (needle.len > s.len || at > s.len - needle.len)
        return 0;
    for (k = 0; k < needle.len; k++) {
        if (s.ptr[at + k] != needle.ptr[k])
            return 0;
    }
    return 1;
}

int main(void)
{
    char buf[MAX_LEN];
    char nbuf[2];
    CuStr s;
    CuStr needle;
    CuStr before;
    CuStr after;
    size_t len;
    size_t nlen;
    size_t i;
    size_t j;
    size_t at;

    len = nondet_size_t();
    __CPROVER_assume(len <= MAX_LEN);
    for (i = 0; i < MAX_LEN; i++)
        buf[i] = nondet_char();
    s = cu_str_n(buf, len);

    i = nondet_size_t();
    j = nondet_size_t();
    __CPROVER_assert(inside(s, cu_str_prefix(s, i)), "prefix stays inside");
    __CPROVER_assert(inside(s, cu_str_skip(s, i)), "skip stays inside");
    __CPROVER_assert(inside(s, cu_str_substr(s, i, j)), "substr stays inside");
    __CPROVER_assert(inside(s, cu_str_trim(s)), "trim stays inside");

    nlen = nondet_size_t();
    __CPROVER_assume(nlen >= 1 && nlen <= 2);
    nbuf[0] = nondet_char();
    nbuf[1] = nondet_char();
    needle = cu_str_n(nbuf, nlen);
    if (cu_str_find(s, needle, &at)) {
        __CPROVER_assert(matches_at(s, needle, at), "find reports a real match");
        for (i = 0; i < at; i++)
            __CPROVER_assert(!matches_at(s, needle, i), "find reports the first match");
        __CPROVER_assert(cu_str_cut(s, needle, &before, &after), "cut agrees with find");
        __CPROVER_assert(before.len == at && before.ptr == s.ptr, "cut before is the prefix");
        __CPROVER_assert(after.ptr == s.ptr + at + nlen && after.len == len - at - nlen,
                         "cut after follows the separator");
    } else {
        for (i = 0; i <= len; i++)
            __CPROVER_assert(!matches_at(s, needle, i), "no match is missed");
    }
    return 0;
}
