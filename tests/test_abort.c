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

int main(void)
{
    expect_abort(end_stale_scope_after_reuse);
    expect_abort(end_scope_after_release);
    expect_abort(u8_too_large);
    expect_abort(u16le_too_large);
    expect_abort(u16be_too_large);
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
