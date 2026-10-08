/* CBMC proof: for every string of up to 5 bytes and every position,
 * including positions past the end, each reader succeeds exactly when
 * enough bytes remain, returns the multiply-and-add value of those bytes,
 * and leaves pos and out unchanged on failure. Every 32-bit value written
 * in either order reads back unchanged. */
#include "cu.h"

#define MAX_LEN 5

size_t nondet_size_t(void);
char nondet_char(void);
unsigned long nondet_ulong(void);
int nondet_int(void);

/* Allocation never fails here, so the builder in the round trip always
 * succeeds; builder failure is covered by tests/test_fmt.c. */
void *malloc(size_t n)
{
    return __CPROVER_allocate(n, 0);
}

static unsigned long ref(const char *p, size_t n, int big)
{
    unsigned long v;
    unsigned long scale;
    size_t k;

    v = 0;
    scale = 1;
    for (k = 0; k < n; k++) {
        v += (unsigned long)(unsigned char)p[big ? n - 1 - k : k] * scale;
        scale *= 256;
    }
    return v;
}

int main(void)
{
    char buf[MAX_LEN];
    CuStr s;
    size_t len;
    size_t pos;
    size_t p;
    size_t n;
    size_t i;
    int big;
    int ok;
    unsigned long v;
    unsigned long w;
    CuArena a;
    CuBuilder b;
    CuStr out;

    for (i = 0; i < MAX_LEN; i++)
        buf[i] = nondet_char();
    len = nondet_size_t();
    __CPROVER_assume(len <= MAX_LEN);
    s = cu_str_n(buf, len);
    pos = nondet_size_t();
    big = nondet_int() != 0;
    switch (nondet_int() & 3) {
    case 0:
        n = 1;
        p = pos;
        v = 7;
        ok = cu_rd_u8(s, &p, &v);
        break;
    case 1:
        n = 2;
        p = pos;
        v = 7;
        ok = big ? cu_rd_u16be(s, &p, &v) : cu_rd_u16le(s, &p, &v);
        break;
    default:
        n = 4;
        p = pos;
        v = 7;
        ok = big ? cu_rd_u32be(s, &p, &v) : cu_rd_u32le(s, &p, &v);
        break;
    }
    if (n == 1)
        big = 0;
    __CPROVER_assert(ok == (pos <= len && n <= len - pos), "read succeeds exactly when bytes remain");
    if (ok) {
        __CPROVER_assert(p == pos + n, "read advances by its width");
        __CPROVER_assert(v == ref(buf + pos, n, big), "read assembles the bytes");
    } else {
        __CPROVER_assert(p == pos && v == 7, "failed read changes nothing");
    }

    w = nondet_ulong();
    __CPROVER_assume((w >> 16 >> 16) == 0);
    cu_arena_init_with(&a, 64, 0);
    cu_sb_init(&b, &a);
    if (big)
        cu_sb_u32be(&b, w);
    else
        cu_sb_u32le(&b, w);
    __CPROVER_assert(cu_sb_finish(&b, &out) && out.len == 4, "write appends four bytes");
    __CPROVER_assert(ref(out.ptr, 4, big) == w, "written bytes encode the value");
    p = 0;
    ok = big ? cu_rd_u32be(out, &p, &v) : cu_rd_u32le(out, &p, &v);
    __CPROVER_assert(ok && p == 4 && v == w, "written value reads back");
    cu_arena_release(&a);
    return 0;
}
