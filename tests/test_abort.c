/* Contract violations must abort. Each case runs in a child process; WASI
 * has no fork, so there the file builds to an empty test. */
#if defined(__unix__) && !defined(__wasi__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "cu.h"

static void expect_abort(void (*body)(void))
{
    pid_t pid;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        body();
        _exit(0);
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

/* Ending the outer scope frees the inner scope's block, and the next push
 * usually gets a block at the same address. The stale inner scope must still
 * be caught, not matched to the new block by address. */
static void end_stale_scope_after_reuse(void)
{
    CuArena a;
    CuTemp outer;
    CuTemp inner;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    outer = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 200) != NULL);
    inner = cu_temp_begin(&a);
    cu_temp_end(outer);
    assert(cu_arena_push_bytes(&a, 200) != NULL);
    cu_temp_end(inner);
}

static void end_scope_after_release(void)
{
    CuArena a;
    CuTemp t;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    t = cu_temp_begin(&a);
    cu_arena_release(&a);
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    cu_temp_end(t);
}

static void write_value(void (*write)(CuBuilder *, unsigned long), unsigned long v)
{
    CuArena a;
    CuBuilder b;

    cu_arena_init(&a);
    cu_sb_init(&b, &a);
    write(&b, v);
    cu_arena_release(&a);
}

static void u8_too_large(void)
{
    write_value(cu_sb_u8, 0x100UL);
}

static void u16le_too_large(void)
{
    write_value(cu_sb_u16le, 0x10000UL);
}

static void u16be_too_large(void)
{
    write_value(cu_sb_u16be, 0x10000UL);
}

/* 2^32, which only exists where long is wider than 32 bits; zero otherwise. */
static unsigned long two_to_32(void)
{
    return (1UL << 16) << 16;
}

static void u32le_too_large(void)
{
    write_value(cu_sb_u32le, two_to_32());
}

static void u32be_too_large(void)
{
    write_value(cu_sb_u32be, two_to_32());
}

static void slice_at_len(void)
{
    long arr[3] = {1, 2, 3};
    CuSlice s;

    s = CU_SLICE(arr, 3);
    (void)CU_SLICE_AT(s, long, 3);
}

static void slice_at_wrong_type(void)
{
    long arr[3] = {1, 2, 3};
    CuSlice s;

    s = CU_SLICE(arr, 3);
    (void)CU_SLICE_AT(s, char, 0);
}

static void slice_data_wrong_type(void)
{
    long arr[3] = {1, 2, 3};
    CuSlice s;

    s = CU_SLICE(arr, 3);
    (void)CU_SLICE_DATA(s, char);
}

static void slice_null_with_length(void)
{
    long *none;

    none = NULL;
    (void)CU_SLICE(none, 1);
}

/* No real array is this big, so the size must not wrap to a small one. */
static void slice_size_overflows(void)
{
    long arr[1];

    arr[0] = 0;
    (void)CU_SLICE(arr, (size_t)-1 / sizeof(long) + 1);
}

/* Bypasses the macro, which always passes a nonzero sizeof. */
static void slice_zero_size(void)
{
    long arr[1];

    arr[0] = 0;
    (void)cu_slice_n_(arr, 1, 0);
}

/* Runs one map call on a map of longs holding "k". */
static void with_long_map(void (*call)(CuMap *))
{
    CuArena a;
    CuMap m;
    long init;

    cu_arena_init(&a);
    CU_MAP_INIT(&m, &a, long);
    init = 1;
    if (CU_MAP_UPSERT(&m, long, CU_LIT("k"), &init) != NULL)
        call(&m);
    cu_arena_release(&a);
}

static void get_wrong_type(CuMap *m)
{
    (void)CU_MAP_GET(m, char, CU_LIT("k"));
}

static void upsert_wrong_type(CuMap *m)
{
    long init;

    init = 1;
    (void)CU_MAP_UPSERT(m, char, CU_LIT("k"), &init);
}

static void upsert_wrong_init(CuMap *m)
{
    char init;

    init = 1;
    (void)CU_MAP_UPSERT(m, long, CU_LIT("new"), &init);
}

static void next_wrong_type(CuMap *m)
{
    CuMapIter it;
    CuStr key;

    it = cu_map_iter(m);
    (void)CU_MAP_NEXT(&it, char, &key);
}

static void map_get_wrong_type(void)
{
    with_long_map(get_wrong_type);
}

static void map_upsert_wrong_type(void)
{
    with_long_map(upsert_wrong_type);
}

static void map_upsert_wrong_init(void)
{
    with_long_map(upsert_wrong_init);
}

static void map_next_wrong_type(void)
{
    with_long_map(next_wrong_type);
}

static void upsert_null_init(CuMap *m)
{
    (void)cu_map_upsert_(m, CU_LIT("new"), NULL, sizeof(long), sizeof(long));
}

static void next_null_key(CuMap *m)
{
    CuMapIter it;

    it = cu_map_iter(m);
    (void)cu_map_next_(&it, NULL, sizeof(long));
}

static void map_upsert_null_init(void)
{
    with_long_map(upsert_null_init);
}

static void map_next_null_key(void)
{
    with_long_map(next_null_key);
}

static void map_null_arena(void)
{
    CuMap m;

    cu_map_init(&m, NULL, sizeof(long));
}

static void map_zero_size(void)
{
    CuArena a;
    CuMap m;

    cu_arena_init(&a);
    cu_map_init(&m, &a, 0);
}

int main(void)
{
    expect_abort(end_stale_scope_after_reuse);
    expect_abort(end_scope_after_release);
    expect_abort(u8_too_large);
    expect_abort(u16le_too_large);
    expect_abort(u16be_too_large);
    expect_abort(slice_at_len);
    expect_abort(slice_at_wrong_type);
    expect_abort(slice_data_wrong_type);
    expect_abort(slice_null_with_length);
    expect_abort(slice_size_overflows);
    expect_abort(slice_zero_size);
    expect_abort(map_get_wrong_type);
    expect_abort(map_upsert_wrong_type);
    expect_abort(map_upsert_wrong_init);
    expect_abort(map_next_wrong_type);
    expect_abort(map_zero_size);
    expect_abort(map_upsert_null_init);
    expect_abort(map_next_null_key);
    expect_abort(map_null_arena);
    if (two_to_32() != 0) {
        expect_abort(u32le_too_large);
        expect_abort(u32be_too_large);
    }
    return 0;
}
#else
int main(void)
{
    return 0;
}
#endif
