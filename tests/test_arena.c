#include "death.h"
#include <assert.h>
#include <string.h>
#include "cu.h"
#include "align.h"

/* Runs each arena test in both modes: shared blocks (0) and one malloc per push (1). */

static void test_pushes_are_separate_and_aligned(int debug)
{
    CuArena a;
    char *bytes;
    double *ds;
    long *ls;
    size_t i;

    cu_arena_init_with(&a, 64, debug);
    bytes = cu_arena_push_bytes(&a, 3);
    ds = CU_PUSH(&a, double, 4);
    ls = CU_PUSH(&a, long, 100);
    assert(bytes != NULL && ds != NULL && ls != NULL);
    assert(is_max_aligned(ds) && is_max_aligned(ls));
    memset(bytes, 'x', 3);
    for (i = 0; i < 4; i++)
        ds[i] = (double)i;
    for (i = 0; i < 100; i++)
        ls[i] = (long)i;
    assert(bytes[0] == 'x' && bytes[2] == 'x');
    assert(ds[3] > 2.5 && ds[3] < 3.5);
    assert(ls[0] == 0 && ls[99] == 99);
    assert(cu_arena_failed(&a) == 0);
    cu_arena_release(&a);
}

static void test_zero_size_push_is_not_null(int debug)
{
    CuArena a;

    cu_arena_init_with(&a, 64, debug);
    assert(cu_arena_push_bytes(&a, 0) != NULL);
    assert(cu_arena_push(&a, 0, sizeof(long)) != NULL);
    assert(cu_arena_failed(&a) == 0);
    cu_arena_release(&a);
}

static void test_overflow_fails_and_is_sticky(int debug)
{
    CuArena a;

    cu_arena_init_with(&a, 64, debug);
    assert(cu_arena_push(&a, (size_t)-1, 2) == NULL);
    assert(cu_arena_failed(&a) == 1);
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    assert(cu_arena_failed(&a) == 1);
    cu_arena_release(&a);
    assert(cu_arena_failed(&a) == 0);
}

static void test_temp_scopes_nest(int debug)
{
    CuArena a;
    CuTemp outer;
    CuTemp inner;
    char *keep;
    char *p;

    cu_arena_init_with(&a, 64, debug);
    keep = cu_arena_push_bytes(&a, 4);
    assert(keep != NULL);
    memcpy(keep, "keep", 4);
    outer = cu_temp_begin(&a);
    p = cu_arena_push_bytes(&a, 200);
    assert(p != NULL);
    inner = cu_temp_begin(&a);
    p = cu_arena_push_bytes(&a, 10);
    assert(p != NULL);
    cu_temp_end(inner);
    p = cu_arena_push_bytes(&a, 10);
    assert(p != NULL);
    cu_temp_end(outer);
    assert(memcmp(keep, "keep", 4) == 0);
    cu_arena_release(&a);
}

static void test_temp_end_reuses_memory(void)
{
    CuArena a;
    CuTemp t;
    char *first;
    char *again;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 1) != NULL);
    t = cu_temp_begin(&a);
    first = cu_arena_push_bytes(&a, 8);
    cu_temp_end(t);
    again = cu_arena_push_bytes(&a, 8);
    assert(first != NULL && first == again);
    cu_arena_release(&a);
}

/* Normal mode packs byte pushes into one block; debug mode gives each push
 * its own allocation, so sanitizers see every object's bounds. */
static void test_debug_mode_separates_pushes(int debug)
{
    CuArena a;
    char *p1;
    char *p2;

    cu_arena_init_with(&a, 64, debug);
    p1 = cu_arena_push_bytes(&a, 1);
    p2 = cu_arena_push_bytes(&a, 1);
    assert(p1 != NULL && p2 != NULL);
    assert((p2 == p1 + 1) == !debug);
    cu_arena_release(&a);
}

static void test_release_then_reuse(int debug)
{
    CuArena a;

    cu_arena_init_with(&a, 64, debug);
    assert(cu_arena_push_bytes(&a, 100) != NULL);
    cu_arena_release(&a);
    assert(cu_arena_push_bytes(&a, 100) != NULL);
    cu_arena_release(&a);
}

static void test_default_init(void)
{
    CuArena a;
    long *p;

    cu_arena_init(&a);
    p = CU_PUSH(&a, long, 1);
    assert(p != NULL);
    *p = 5;
    assert(*p == 5);
    cu_arena_release(&a);
}

static void test_scratch_avoids_conflicts(void)
{
    CuTemp s1;
    CuTemp s2;
    CuTemp s3;
    CuArena *conflicts[1];
    char *p;

    s1 = cu_scratch_begin1(NULL);
    assert(s1.arena != NULL);
    s2 = cu_scratch_begin1(s1.arena);
    assert(s2.arena != NULL && s2.arena != s1.arena);
    conflicts[0] = s2.arena;
    s3 = cu_scratch_begin(conflicts, 1);
    assert(s3.arena == s1.arena);
    p = cu_arena_push_bytes(s2.arena, 16);
    assert(p != NULL);
    cu_scratch_end(s3);
    cu_scratch_end(s2);
    cu_scratch_end(s1);
    cu_scratch_release();
}

/* A push that exactly fills the rest of a block stays in it. */
static void test_exact_fit_stays_in_block(void)
{
    CuArena a;
    size_t one;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 32) != NULL);
    one = a.held;
    assert(cu_arena_push_bytes(&a, 32) != NULL);
    assert(a.held == one);
    /* Nothing more fits, but a zero-size push still needs no new block. */
    assert(cu_arena_push_bytes(&a, 0) != NULL);
    assert(a.held == one);
    assert(cu_arena_push_bytes(&a, 1) != NULL);
    assert(a.held > one);
    cu_arena_release(&a);
}

/* A size that overflows once the block header is added fails cleanly. */
static void test_header_overflow_fails(int debug)
{
    CuArena a;

    cu_arena_init_with(&a, 64, debug);
    assert(cu_arena_push_bytes(&a, (size_t)-1) == NULL);
    assert(cu_arena_failed(&a) && a.held == 0);
    cu_arena_release(&a);
}

/* The limit counts whole blocks: a push fails when its block would pass
 * it, ending a scope or releasing gives the bytes back, and the arena is
 * marked failed exactly as for running out of memory. */
static void test_limit(int debug)
{
    CuArena a;
    CuTemp t;
    size_t one;

    /* Measure one block's cost with no limit. */
    cu_arena_init_with(&a, 64, debug);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    one = a.held;
    assert(one > 64);
    cu_arena_release(&a);
    assert(a.held == 0);

    cu_arena_set_limit(&a, one - 1);
    assert(cu_arena_push_bytes(&a, 64) == NULL);
    assert(cu_arena_failed(&a) && a.held == 0);
    /* refused names the exact cap that lets the push through. */
    assert(a.refused == one && a.held + a.refused == one);
    cu_arena_release(&a);

    cu_arena_set_limit(&a, one);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    assert(a.held == one);
    t = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 64) == NULL);
    assert(cu_arena_failed(&a));
    cu_temp_end(t);
    cu_arena_release(&a);
    assert(!cu_arena_failed(&a));

    /* A scope that ends returns its blocks' bytes. */
    cu_arena_set_limit(&a, 2 * one);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    t = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    assert(a.held == 2 * one);
    cu_temp_end(t);
    assert(a.held == one);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);

    /* A limit set below what is already held refuses new blocks. */
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    cu_arena_set_limit(&a, 1);
    assert(cu_arena_push_bytes(&a, 64) == NULL);
    assert(a.refused != 0);
    /* Releasing clears the refusal along with the failure. */
    cu_arena_release(&a);
    assert(a.refused == 0 && !cu_arena_failed(&a));

    /* No limit by default, and (size_t)-1 lifts one. */
    cu_arena_init_with(&a, 64, debug);
    assert(a.limit == (size_t)-1 && a.refused == 0);
    cu_arena_set_limit(&a, 0);
    assert(cu_arena_push_bytes(&a, 1) == NULL);
    cu_arena_set_limit(&a, (size_t)-1);
    assert(cu_arena_push_bytes(&a, 1) != NULL);
    cu_arena_release(&a);
}

/* cu_arena_fail_at fails exactly one block allocation, the one with that
 * attempt number; the ones before and after it succeed. A push that fits
 * in the current block makes no attempt, except in debug mode, where every
 * push is its own allocation. */
static void test_fail_at(int debug)
{
    CuArena a;
    size_t before;

    cu_arena_init_with(&a, 64, debug);
    assert(a.attempts == 0 && a.fail_at == (size_t)-1);
    cu_arena_fail_at(&a, debug ? 2 : 1);
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    assert(a.attempts == 1 && !cu_arena_failed(&a));
    /* Fits in the 64-byte block the first push made. */
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    assert(a.attempts == (debug ? 2u : 1u));
    before = a.attempts;
    assert(cu_arena_push_bytes(&a, 100) == NULL);
    assert(a.attempts == before + 1 && cu_arena_failed(&a));
    assert(cu_arena_push_bytes(&a, 100) != NULL);
    assert(a.attempts == before + 2);
    /* The count runs on across a release, and (size_t)-1 fails none. */
    cu_arena_release(&a);
    before = a.attempts;
    cu_arena_fail_at(&a, (size_t)-1);
    assert(cu_arena_push_bytes(&a, 100) != NULL);
    assert(a.attempts == before + 1 && !cu_arena_failed(&a));
    /* The failing attempt is the next one, even right after a release. */
    cu_arena_fail_at(&a, a.attempts);
    assert(cu_arena_push(&a, 1, 100) == NULL);
    cu_arena_release(&a);
}

/* Fills n bytes with a pattern that check_pattern recognises. */
static void fill_pattern(char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        p[i] = (char)(i * 7 + 1);
}

static int has_pattern(const char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (p[i] != (char)(i * 7 + 1))
            return 0;
    }
    return 1;
}

/* The newest allocation grows into the rest of its block: same pointer,
 * no block allocation, nothing more held. */
static void test_grow_in_place(void)
{
    CuArena a;
    char *p;
    char *q;
    size_t held;
    size_t attempts;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 3) != NULL);
    p = (char *)cu_arena_push(&a, 1, 16);
    assert(p != NULL);
    fill_pattern(p, 16);
    held = a.held;
    attempts = a.attempts;
    q = (char *)cu_arena_grow(&a, p, 16, 40);
    assert(q == p && has_pattern(q, 16));
    assert(a.held == held && a.attempts == attempts);
    /* The grown bytes are taken: the next push lands after them. */
    q = cu_arena_push_bytes(&a, 1);
    assert(q == p + 40);
    /* Not growing returns p unchanged. */
    assert(cu_arena_grow(&a, q, 1, 1) == q);
    assert(cu_arena_grow(&a, p, 40, 8) == p);
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);
}

/* An allocation that is not the newest one moves: a new push, a copy, and
 * the old bytes stay where they were. */
static void test_grow_moves_when_not_newest(int debug)
{
    CuArena a;
    char *p;
    char *q;
    char *later;

    cu_arena_init_with(&a, 64, debug);
    p = cu_arena_push_bytes(&a, 16);
    later = cu_arena_push_bytes(&a, 4);
    assert(p != NULL && later != NULL);
    fill_pattern(p, 16);
    memcpy(later, "keep", 4);
    q = (char *)cu_arena_grow(&a, p, 16, 24);
    assert(q != NULL && q != p && is_max_aligned(q));
    assert(has_pattern(q, 16) && has_pattern(p, 16));
    assert(memcmp(later, "keep", 4) == 0);
    /* NULL with old size 0 is a plain push. */
    q = (char *)cu_arena_grow(&a, NULL, 0, 8);
    assert(q != NULL && is_max_aligned(q));
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);
}

/* An allocation alone at the start of the newest block is grown by
 * reallocating the block: one attempt, and held grows by exactly the bytes
 * added. */
static void test_grow_reallocates_own_block(int debug)
{
    CuArena a;
    char *p;
    char *q;
    size_t held;
    size_t attempts;

    cu_arena_init_with(&a, 64, debug);
    p = cu_arena_push_bytes(&a, 64);
    assert(p != NULL);
    fill_pattern(p, 64);
    held = a.held;
    attempts = a.attempts;
    q = (char *)cu_arena_grow(&a, p, 64, 1000);
    assert(q != NULL && has_pattern(q, 64));
    assert(a.held == held + (1000 - 64) && a.attempts == attempts + 1);
    fill_pattern(q, 1000);
    q = (char *)cu_arena_grow(&a, q, 1000, 5000);
    assert(q != NULL && has_pattern(q, 1000));
    assert(a.held == held + (5000 - 64) && a.attempts == attempts + 2);
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);
    assert(a.held == 0);
}

/* Memory pushed before the innermost temp scope began is never grown in
 * place, so ending the scope cannot free part of it; it moves into the
 * scope instead. After that scope ends it grows in place again. */
static void test_grow_respects_temp_scopes(void)
{
    CuArena a;
    CuTemp t;
    char *p;
    char *q;
    char *alone;

    cu_arena_init_with(&a, 64, 0);
    p = cu_arena_push_bytes(&a, 8);
    assert(p != NULL);
    fill_pattern(p, 8);
    t = cu_temp_begin(&a);
    q = (char *)cu_arena_grow(&a, p, 8, 16);
    assert(q != NULL && q != p && has_pattern(q, 8));
    cu_temp_end(t);
    /* The scope gave back what it took: p is the newest allocation again. */
    q = (char *)cu_arena_grow(&a, p, 8, 16);
    assert(q == p && has_pattern(p, 8));

    /* Pushed inside the scope, it grows in place there. */
    t = cu_temp_begin(&a);
    p = cu_arena_push_bytes(&a, 8);
    assert(p != NULL);
    q = (char *)cu_arena_grow(&a, p, 8, 16);
    assert(q == p);
    cu_temp_end(t);
    cu_arena_release(&a);

    /* Nor is a block reallocated whose allocation predates the scope. */
    alone = cu_arena_push_bytes(&a, 64);
    assert(alone != NULL);
    fill_pattern(alone, 64);
    t = cu_temp_begin(&a);
    q = (char *)cu_arena_grow(&a, alone, 64, 500);
    assert(q != NULL && q != alone && has_pattern(q, 64));
    cu_temp_end(t);
    assert(has_pattern(alone, 64));
    /* A scope begun at the start of a block reallocates it, and ending the
     * scope frees the block's bytes. */
    t = cu_temp_begin(&a);
    p = cu_arena_push_bytes(&a, 64);
    assert(p != NULL);
    q = (char *)cu_arena_grow(&a, p, 64, 500);
    assert(q != NULL);
    cu_temp_end(t);
    assert(has_pattern(alone, 64));
    assert(!cu_arena_failed(&a));
    cu_arena_release(&a);
}

/* A grow that needs memory fails like a push: NULL, the arena marked, and
 * p untouched and still valid. A limit refusal records the bytes the grow
 * needed, so held + refused is again the smallest cap that lets it pass. */
static void test_grow_failure(int debug)
{
    CuArena a;
    char *p;
    char *q;
    size_t held;

    cu_arena_init_with(&a, 64, debug);
    p = cu_arena_push_bytes(&a, 64);
    assert(p != NULL);
    fill_pattern(p, 64);
    held = a.held;
    cu_arena_fail_at(&a, a.attempts);
    assert(cu_arena_grow(&a, p, 64, 200) == NULL);
    assert(cu_arena_failed(&a) && a.held == held && has_pattern(p, 64));
    cu_arena_release(&a);

    p = cu_arena_push_bytes(&a, 64);
    assert(p != NULL);
    fill_pattern(p, 64);
    held = a.held;
    cu_arena_set_limit(&a, held + 100);
    assert(cu_arena_grow(&a, p, 64, 200) == NULL);
    assert(cu_arena_failed(&a) && a.refused == 200 - 64 && a.held == held && has_pattern(p, 64));
    cu_arena_set_limit(&a, a.held + a.refused);
    q = (char *)cu_arena_grow(&a, p, 64, 200);
    assert(q != NULL && has_pattern(q, 64) && a.held == a.limit);
    cu_arena_release(&a);

    /* The moving path fails the same way. */
    cu_arena_set_limit(&a, (size_t)-1);
    p = cu_arena_push_bytes(&a, 8);
    assert(p != NULL && cu_arena_push_bytes(&a, 1) != NULL);
    cu_arena_fail_at(&a, a.attempts);
    assert(cu_arena_grow(&a, p, 8, 100) == NULL && cu_arena_failed(&a));
    /* The size of the reallocated block overflowing fails too. */
    cu_arena_release(&a);
    p = cu_arena_push_bytes(&a, 64);
    assert(p != NULL);
    assert(cu_arena_grow(&a, p, 64, (size_t)-1) == NULL && cu_arena_failed(&a));
    cu_arena_release(&a);
}

/* cu_arena_extend grows only in place, and otherwise changes nothing. */
static void test_extend(void)
{
    CuArena a;
    char *p;
    char *q;
    size_t held;
    size_t attempts;

    cu_arena_init_with(&a, 64, 0);
    p = cu_arena_push_bytes(&a, 16);
    assert(p != NULL);
    held = a.held;
    attempts = a.attempts;
    assert(cu_arena_extend(&a, p, 16, 40));
    q = cu_arena_push_bytes(&a, 1);
    assert(q == p + 40);
    assert(!cu_arena_extend(&a, p, 40, 48));
    assert(cu_arena_extend(&a, p, 40, 8));
    assert(!cu_arena_extend(&a, q, 1, 100));
    assert(!cu_arena_extend(&a, q, 0, 1));
    assert(a.held == held && a.attempts == attempts && !cu_arena_failed(&a));
    cu_arena_release(&a);
}

/* A zero-size allocation made before a scope began sits exactly at the
 * scope's start. Growing it must not take the scope's memory, which the
 * scope's end would hand out again. */
static void test_grow_zero_size_before_scope(void)
{
    CuArena a;
    CuTemp t;
    char *z;
    char *g;
    char *later;

    /* z at an odd offset: a fresh, aligned push cannot start there, so
     * g != z shows the grow did not happen in place. */
    cu_arena_init_with(&a, 4096, 0);
    assert(cu_arena_push_bytes(&a, 7) != NULL);
    z = cu_arena_push_bytes(&a, 0);
    assert(z != NULL);
    t = cu_temp_begin(&a);
    g = (char *)cu_arena_grow(&a, z, 0, 16);
    assert(g != NULL && g != z);
    cu_temp_end(t);
    /* Outside the scope, a zero-size allocation still gets fresh memory. */
    g = (char *)cu_arena_grow(&a, z, 0, 16);
    assert(g != NULL);
    memcpy(g, "outer-data-xxxx", 16);
    later = cu_arena_push_bytes(&a, 16);
    assert(later != NULL && later != g);
    assert(memcmp(g, "outer-data-xxxx", 16) == 0);
    cu_arena_release(&a);
}

/* A scope that began at the start of an existing block does not own that
 * block, so growth inside the scope must not reallocate it: the scope's end
 * would keep the larger block. */
static void test_grow_does_not_realloc_block_from_before_scope(void)
{
    CuArena a;
    CuTemp t;
    char *p;
    size_t before;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 0) != NULL);
    before = a.held;
    t = cu_temp_begin(&a);
    p = cu_arena_push_bytes(&a, 64);
    assert(p != NULL);
    fill_pattern(p, 64);
    p = (char *)cu_arena_grow(&a, p, 64, 100000);
    assert(p != NULL && has_pattern(p, 64));
    cu_temp_end(t);
    assert(a.held == before);
    cu_arena_release(&a);
}

/* A limit lowered inside a scope still holds after the scope ends: the
 * scope's block is freed rather than cached past it. */
static void test_scope_end_respects_lowered_limit(void)
{
    CuArena a;
    CuTemp t;

    cu_arena_init_with(&a, 1024, 0);
    assert(cu_arena_push_bytes(&a, 1024) != NULL);
    assert(cu_arena_push_bytes(&a, 1024) != NULL);
    t = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 4000) != NULL);
    cu_arena_set_limit(&a, 2500);
    cu_temp_end(t);
    assert(a.cached == 0 && a.held + a.cached <= a.limit);
    cu_arena_release(&a);
}

/* Ending a scope keeps its largest block for the next block the arena
 * needs, instead of freeing it and asking malloc again. A kept block is
 * cached, not held: held counts live blocks only. Reusing it is a block
 * allocation attempt like any other. */
static void test_scope_end_caches_largest_block(void)
{
    CuArena a;
    CuTemp t;
    size_t one;
    size_t big;
    size_t attempts;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    one = a.held;
    assert(a.cached == 0);
    t = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 1000) != NULL);
    big = a.held - one;
    assert(cu_arena_push_bytes(&a, 200) != NULL);
    assert(cu_arena_push_bytes(&a, 100) != NULL);
    cu_temp_end(t);
    assert(a.held == one && a.cached == big);

    /* A push that fits the cached block takes it. */
    t = cu_temp_begin(&a);
    attempts = a.attempts;
    assert(cu_arena_push_bytes(&a, 500) != NULL);
    assert(a.attempts == attempts + 1 && a.held == one + big && a.cached == 0);
    /* The rest of the cached block serves later pushes. */
    assert(cu_arena_push_bytes(&a, 400) != NULL);
    assert(a.held == one + big);
    cu_temp_end(t);
    assert(a.held == one && a.cached == big);

    /* An injected failure leaves the cache alone. */
    t = cu_temp_begin(&a);
    cu_arena_fail_at(&a, a.attempts);
    assert(cu_arena_push_bytes(&a, 500) == NULL);
    assert(a.held == one && a.cached == big);
    assert(cu_arena_push_bytes(&a, 500) != NULL && a.cached == 0);
    cu_temp_end(t);

    /* A push too big for it gets a fresh block, and the smaller cached
     * block is freed rather than kept beside it. */
    t = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 5000) != NULL);
    assert(a.cached == 0);
    cu_temp_end(t);
    assert(a.held == one && a.cached > big);

    /* Releasing frees the cached block too. */
    cu_arena_release(&a);
    assert(a.held == 0 && a.cached == 0);
    cu_arena_release(&a);
}

/* Held plus cached bytes never pass the limit: a cached block is reused
 * only if the limit allows it, and dropped before a fresh block would pass
 * the limit beside it. refused still names the cap the push needs. */
static void test_cache_respects_limit(void)
{
    CuArena a;
    CuTemp t;
    size_t one;
    size_t big;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    one = a.held;
    t = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 1000) != NULL);
    big = a.held - one;
    cu_temp_end(t);
    assert(a.cached == big);
    /* The cached block does not fit under this cap, but a small block
     * does: the cache goes, and the push gets a block of its own size. */
    cu_arena_set_limit(&a, 2 * one);
    t = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    assert(a.held == 2 * one && a.cached == 0);
    cu_temp_end(t);
    assert(a.held + a.cached <= a.limit);
    /* A refusal names the push's own block, not the cached one. */
    assert(a.cached == one);
    cu_arena_set_limit(&a, one + 10);
    assert(cu_arena_push_bytes(&a, 64) == NULL);
    assert(a.refused == one && a.cached == 0);
    cu_arena_release(&a);

    /* Lowering the limit below held + cached drops the cached block. */
    cu_arena_set_limit(&a, (size_t)-1);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    t = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 64) != NULL);
    cu_temp_end(t);
    assert(a.cached == one);
    cu_arena_set_limit(&a, 2 * one);
    assert(a.cached == one);
    cu_arena_set_limit(&a, 2 * one - 1);
    assert(a.cached == 0);
    cu_arena_release(&a);
}

/* Debug mode frees every block at scope end, so sanitizers still see each
 * use of memory from an ended scope. */
static void test_debug_mode_caches_nothing(void)
{
    CuArena a;
    CuTemp t;

    cu_arena_init_with(&a, 64, 1);
    t = cu_temp_begin(&a);
    assert(cu_arena_push_bytes(&a, 1000) != NULL);
    cu_temp_end(t);
    assert(a.held == 0 && a.cached == 0);
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
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

static void grow_null_with_old_size(void)
{
    CuArena a;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_grow(&a, NULL, 1, 8) != NULL);
}

/* Contract violations abort. */
static void death_tests(void)
{
    expect_abort(grow_null_with_old_size);
    expect_abort(end_stale_scope_after_reuse);
    expect_abort(end_scope_after_release);
}
#endif

int main(void)
{
    int debug;

    for (debug = 0; debug <= 1; debug++) {
        test_pushes_are_separate_and_aligned(debug);
        test_zero_size_push_is_not_null(debug);
        test_overflow_fails_and_is_sticky(debug);
        test_temp_scopes_nest(debug);
        test_release_then_reuse(debug);
        test_debug_mode_separates_pushes(debug);
        test_limit(debug);
        test_fail_at(debug);
        test_header_overflow_fails(debug);
        test_grow_moves_when_not_newest(debug);
        test_grow_reallocates_own_block(debug);
        test_grow_failure(debug);
    }
    test_grow_in_place();
    test_scope_end_caches_largest_block();
    test_grow_zero_size_before_scope();
    test_extend();
    test_grow_does_not_realloc_block_from_before_scope();
    test_scope_end_respects_lowered_limit();
    test_cache_respects_limit();
    test_debug_mode_caches_nothing();
    test_grow_respects_temp_scopes();
    test_temp_end_reuses_memory();
    test_exact_fit_stays_in_block();
    test_default_init();
    test_scratch_avoids_conflicts();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
