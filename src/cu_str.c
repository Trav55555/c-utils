#include <stdlib.h>
#include <string.h>
#include "cu.h"

CuStr cu_str_n(const char *ptr, size_t len)
{
    CuStr s;

    if (ptr == NULL) {
        if (len != 0)
            abort();
        ptr = "";
    }
    s.ptr = ptr;
    s.len = len;
    return s;
}

CuStr cu_str_cstr(const char *s)
{
    if (s == NULL)
        return cu_str_n("", 0);
    return cu_str_n(s, strlen(s));
}

CuStr cu_str_empty(void)
{
    return cu_str_n("", 0);
}

int cu_str_eq(CuStr a, CuStr b)
{
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

char cu_str_at(CuStr s, size_t i)
{
    if (i >= s.len)
        abort();
    return s.ptr[i];
}

CuStr cu_str_prefix(CuStr s, size_t n)
{
    if (n < s.len)
        s.len = n;
    return s;
}

CuStr cu_str_skip(CuStr s, size_t n)
{
    if (n > s.len)
        n = s.len;
    s.ptr += n;
    s.len -= n;
    return s;
}

CuStr cu_str_substr(CuStr s, size_t start, size_t end)
{
    if (end > s.len)
        end = s.len;
    if (start > end)
        start = end;
    s.ptr += start;
    s.len = end - start;
    return s;
}

CuStr cu_str_trim(CuStr s)
{
    while (s.len > 0 && cu_ascii_is_space(s.ptr[0])) {
        s.ptr++;
        s.len--;
    }
    while (s.len > 0 && cu_ascii_is_space(s.ptr[s.len - 1]))
        s.len--;
    return s;
}

int cu_str_starts_with(CuStr s, CuStr prefix)
{
    return prefix.len <= s.len && memcmp(s.ptr, prefix.ptr, prefix.len) == 0;
}

int cu_str_ends_with(CuStr s, CuStr suffix)
{
    return suffix.len <= s.len &&
           memcmp(s.ptr + (s.len - suffix.len), suffix.ptr, suffix.len) == 0;
}

/* The maximal suffix of x[0..m) under the byte order, or under the
 * reverse order when reverse is set: returns where it starts and stores
 * its period in *period (Crochemore and Perrin). Positions are kept one
 * higher than in the published form, so nothing wraps below 0. */
static size_t maximal_suffix(const unsigned char *x, size_t m, int reverse, size_t *period)
{
    size_t ms; /* start of the suffix so far, plus one */
    size_t j;
    size_t k;
    size_t p;
    unsigned char a;
    unsigned char b;

    ms = 0;
    j = 0;
    k = 1;
    p = 1;
    while (j + k < m) {
        a = x[j + k];
        b = x[ms - 1 + k];
        if (a == b) {
            if (k != p) {
                k++;
            } else {
                j += p;
                k = 1;
            }
        } else if ((a < b) != (reverse != 0)) {
            j += k;
            k = 1;
            p = j + 1 - ms;
        } else {
            ms = j + 1;
            j++;
            k = 1;
            p = 1;
        }
    }
    *period = p;
    return ms;
}

/* Two-Way string matching (Crochemore and Perrin, 1991): the first match
 * of x[0..m) in y[0..n) at or after start, in O(n + m) time and O(1)
 * memory, whatever the bytes. Needs 2 <= m <= n. Returns 1 and stores the
 * offset in *at, or returns 0. */
static int two_way(const unsigned char *y, size_t n, const unsigned char *x, size_t m, size_t start,
                   size_t *at)
{
    size_t suffix;
    size_t period;
    size_t rev;
    size_t rev_period;
    size_t memory;
    size_t i;
    size_t j;

    /* The critical factorization x = x[0..suffix) x[suffix..m). period is
     * the period of the right part, so suffix + period <= m. */
    suffix = maximal_suffix(x, m, 0, &period);
    rev = maximal_suffix(x, m, 1, &rev_period);
    if (rev >= suffix) {
        suffix = rev;
        period = rev_period;
    }
    j = start;
    if (memcmp(x, x + period, suffix) == 0) {
        /* x is periodic: after a shift by the period, the bytes before
         * memory are known to match and are not compared again. */
        memory = 0;
        while (j <= n - m) {
            i = suffix > memory ? suffix : memory;
            while (i < m && x[i] == y[i + j])
                i++;
            if (i < m) {
                j += i - suffix + 1;
                memory = 0;
                continue;
            }
            i = suffix;
            while (i > memory && x[i - 1] == y[i - 1 + j])
                i--;
            if (i <= memory) {
                *at = j;
                return 1;
            }
            j += period;
            memory = m - period;
        }
    } else {
        /* Otherwise a mismatch on the left part allows a shift past it. */
        period = (suffix > m - suffix ? suffix : m - suffix) + 1;
        while (j <= n - m) {
            i = suffix;
            while (i < m && x[i] == y[i + j])
                i++;
            if (i < m) {
                j += i - suffix + 1;
                continue;
            }
            i = suffix;
            while (i > 0 && x[i - 1] == y[i - 1 + j])
                i--;
            if (i == 0) {
                *at = j;
                return 1;
            }
            j += period;
        }
    }
    return 0;
}

/* memchr for the needle's first byte, then memcmp: fast on ordinary text.
 * Each candidate costs at most needle.len compares, so when candidates
 * come too often (cand / 8 > i / needle.len, about cand * needle.len >
 * 8 * i) the rest of the search switches to Two-Way, keeping the whole
 * search linear on any input. */
int cu_str_find(CuStr s, CuStr needle, size_t *at)
{
    size_t i;
    size_t last;
    size_t cand;
    const char *hit;

    if (at == NULL)
        abort();
    if (needle.len == 0) {
        *at = 0;
        return 1;
    }
    if (needle.len > s.len)
        return 0;
    last = s.len - needle.len;
    i = 0;
    cand = 0;
    while (i <= last) {
        hit = (const char *)memchr(s.ptr + i, needle.ptr[0], last - i + 1);
        if (hit == NULL)
            return 0;
        i = (size_t)(hit - s.ptr);
        if (memcmp(s.ptr + i, needle.ptr, needle.len) == 0) {
            *at = i;
            return 1;
        }
        i++;
        cand++;
        if (cand > 16 && cand / 8 > i / needle.len)
            return two_way((const unsigned char *)s.ptr, s.len, (const unsigned char *)needle.ptr,
                           needle.len, i, at);
    }
    return 0;
}

int cu_str_cut(CuStr s, CuStr sep, CuStr *before, CuStr *after)
{
    size_t at;

    if (before == NULL || after == NULL)
        abort();
    if (!cu_str_find(s, sep, &at)) {
        *before = s;
        *after = cu_str_empty();
        return 0;
    }
    *before = cu_str_prefix(s, at);
    *after = cu_str_skip(s, at + sep.len);
    return 1;
}

CuStr cu_str_copy(CuArena *a, CuStr s)
{
    char *p;

    if (a == NULL)
        abort();
    if (s.len == 0)
        return cu_str_empty();
    p = cu_arena_push_bytes(a, s.len);
    if (p == NULL)
        return cu_str_empty();
    memcpy(p, s.ptr, s.len);
    return cu_str_n(p, s.len);
}

const char *cu_str_cstr_copy(CuArena *a, CuStr s)
{
    size_t n;
    char *p;

    if (a == NULL)
        abort();
    if (!cu_size_add(s.len, 1, &n)) {
        a->failed = 1;
        return NULL;
    }
    p = cu_arena_push_bytes(a, n);
    if (p == NULL)
        return NULL;
    memcpy(p, s.ptr, s.len);
    p[s.len] = '\0';
    return p;
}

int cu_ascii_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

int cu_ascii_is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int cu_ascii_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

char cu_ascii_to_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}
