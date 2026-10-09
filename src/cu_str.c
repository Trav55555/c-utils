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

int cu_str_find(CuStr s, CuStr needle, size_t *at)
{
    size_t i;
    size_t last;
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
