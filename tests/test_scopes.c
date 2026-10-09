#include "death.h"
#include <assert.h>
#include "cu.h"

/* Temp scopes against the containers built on an arena. A container keeps
 * its memory in its arena, so growing it inside a scope that ends first
 * would free that memory under it. Each container remembers the scope it
 * was made in and aborts when it is grown under any other. */

static void test_scope_ids_nest(void)
{
    CuArena a;
    CuTemp outer;
    CuTemp inner;
    size_t base;

    cu_arena_init_with(&a, 64, 0);
    base = cu_arena_scope(&a);
    assert(base != 0);
    outer = cu_temp_begin(&a);
    assert(cu_arena_scope(&a) == outer.id && outer.id != base);
    inner = cu_temp_begin(&a);
    assert(cu_arena_scope(&a) == inner.id && inner.id != outer.id);
    cu_temp_end(inner);
    assert(cu_arena_scope(&a) == outer.id);
    cu_temp_end(outer);
    assert(cu_arena_scope(&a) == base);
    /* A reopened scope gets a new id, so the old one cannot pass for it. */
    inner = cu_temp_begin(&a);
    assert(inner.id != outer.id);
    cu_temp_end(inner);
    /* Releasing discards open scopes and starts a new base. */
    outer = cu_temp_begin(&a);
    cu_arena_release(&a);
    assert(cu_arena_scope(&a) != base && cu_arena_scope(&a) != outer.id);
    assert(cu_arena_scope(&a) != 0);
    cu_arena_release(&a);
}

/* 0 is never an id, so a zeroed CuTemp or container never matches; a
 * wrapping counter skips it. */
static void test_scope_id_skips_zero(void)
{
    CuArena a;
    CuTemp t;

    cu_arena_init_with(&a, 64, 0);
    a.scopes = (size_t)-1;
    t = cu_temp_begin(&a);
    assert(t.id != 0 && cu_arena_scope(&a) == t.id);
    cu_temp_end(t);
    a.scopes = (size_t)-1;
    cu_arena_release(&a);
    assert(cu_arena_scope(&a) != 0);
    cu_arena_release(&a);
}

/* A container made after a release works: release only retires what was
 * made before it. */
static void test_container_after_release(void)
{
    CuArena a;
    CuVec v;
    long x;

    cu_arena_init_with(&a, 64, 0);
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    cu_arena_release(&a);
    CU_VEC_INIT(&v, &a, long);
    x = 3;
    CU_VEC_PUSH(&v, &x);
    assert(!cu_vec_failed(&v) && *CU_VEC_AT(&v, long, 0) == 3);
    cu_arena_release(&a);
}

/* Containers made inside a scope work there, and reading an outer
 * container from inside a scope is fine. */
static void test_containers_inside_their_scope(void)
{
    CuArena a;
    CuTemp t;
    CuVec v;
    CuMap m;
    CuPool p;
    CuPlex px;
    CuBuilder b;
    CuStr s;
    long x;
    unsigned kind;
    CuHandle h;

    cu_arena_init_with(&a, 64, 0);
    x = 7;
    CU_MAP_INIT(&m, &a, long);
    assert(CU_MAP_UPSERT(&m, long, CU_LIT("k"), &x) != NULL);
    t = cu_temp_begin(&a);
    assert(*CU_MAP_GET(&m, long, CU_LIT("k")) == 7);
    CU_VEC_INIT(&v, &a, long);
    CU_VEC_PUSH(&v, &x);
    assert(!cu_vec_failed(&v) && *CU_VEC_AT(&v, long, 0) == 7);
    CU_POOL_INIT(&p, &a, long);
    h = CU_POOL_ADD(&p, &x);
    assert(*CU_POOL_GET(&p, long, h) == 7);
    cu_plex_init(&px, &a);
    kind = CU_PLEX_KIND(&px, long);
    assert(kind != 0 && !cu_link_is_null(CU_PLEX_ADD(&px, kind, &x)));
    cu_sb_init(&b, &a);
    cu_sb_char(&b, 'x');
    assert(cu_sb_finish(&b, &s) && cu_str_eq(s, CU_LIT("x")));
    cu_temp_end(t);
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
static void end_twice(void)
{
    CuArena a;
    CuTemp t;

    cu_arena_init_with(&a, 64, 0);
    t = cu_temp_begin(&a);
    cu_temp_end(t);
    cu_temp_end(t);
}

/* Ending the outer scope while the inner one is open aborts at once, with
 * no push in between to give it away. */
static void end_outer_first(void)
{
    CuArena a;
    CuTemp outer;
    CuTemp inner;

    cu_arena_init_with(&a, 4096, 0);
    assert(cu_arena_push_bytes(&a, 8) != NULL);
    outer = cu_temp_begin(&a);
    inner = cu_temp_begin(&a);
    (void)inner;
    cu_temp_end(outer);
}

/* Begun before any push, so only the scope id, not the block number,
 * shows that release discarded it. */
static void end_scope_after_release_of_empty_arena(void)
{
    CuArena a;
    CuTemp t;

    cu_arena_init_with(&a, 64, 0);
    t = cu_temp_begin(&a);
    cu_arena_release(&a);
    cu_temp_end(t);
}

/* Scratch arenas keep numbering scopes across cu_scratch_release, so a
 * stale scratch scope cannot match a new one. */
static void end_stale_scratch_scope(void)
{
    CuTemp old;
    CuTemp now;

    old = cu_scratch_begin1(NULL);
    cu_scratch_release();
    now = cu_scratch_begin1(NULL);
    assert(now.arena == old.arena);
    cu_scratch_end(old);
}

/* Made with no scope open, so before release gave the arena a new base
 * the vector's scope matched the arena's again and this push wrote into a
 * freed block. */
static void vec_push_after_release(void)
{
    CuArena a;
    CuVec v;
    long x;

    cu_arena_init_with(&a, 64, 0);
    CU_VEC_INIT(&v, &a, long);
    x = 1;
    CU_VEC_PUSH(&v, &x);
    cu_arena_release(&a);
    CU_VEC_PUSH(&v, &x);
}

static void builder_append_after_release(void)
{
    CuArena a;
    CuBuilder b;

    cu_arena_init_with(&a, 64, 0);
    cu_sb_init(&b, &a);
    cu_sb_char(&b, 'x');
    cu_arena_release(&a);
    cu_sb_char(&b, 'y');
}

/* A scope that ended cannot pass for a new scope at the same depth. */
static void end_stale_scope_at_same_depth(void)
{
    CuArena a;
    CuTemp old;
    CuTemp now;

    cu_arena_init_with(&a, 64, 0);
    old = cu_temp_begin(&a);
    cu_temp_end(old);
    now = cu_temp_begin(&a);
    (void)now;
    cu_temp_end(old);
}

static void vec_push_in_inner_scope(void)
{
    CuArena a;
    CuVec v;
    CuTemp t;
    long x;

    cu_arena_init_with(&a, 64, 0);
    CU_VEC_INIT(&v, &a, long);
    t = cu_temp_begin(&a);
    (void)t;
    x = 1;
    CU_VEC_PUSH(&v, &x);
}

static void vec_reserve_in_inner_scope(void)
{
    CuArena a;
    CuVec v;
    CuTemp t;

    cu_arena_init_with(&a, 64, 0);
    CU_VEC_INIT(&v, &a, long);
    t = cu_temp_begin(&a);
    (void)t;
    assert(cu_vec_reserve(&v, 1));
}

/* Made in one scope, grown after it ended and another began. */
static void vec_push_in_later_scope(void)
{
    CuArena a;
    CuVec v;
    CuTemp t;
    long x;

    cu_arena_init_with(&a, 64, 0);
    t = cu_temp_begin(&a);
    CU_VEC_INIT(&v, &a, long);
    cu_temp_end(t);
    t = cu_temp_begin(&a);
    (void)t;
    x = 1;
    CU_VEC_PUSH(&v, &x);
}

static void map_upsert_in_inner_scope(void)
{
    CuArena a;
    CuMap m;
    CuTemp t;
    long x;

    cu_arena_init_with(&a, 64, 0);
    CU_MAP_INIT(&m, &a, long);
    t = cu_temp_begin(&a);
    (void)t;
    x = 1;
    assert(CU_MAP_UPSERT(&m, long, CU_LIT("k"), &x) != NULL);
}

static void pool_add_in_inner_scope(void)
{
    CuArena a;
    CuPool p;
    CuTemp t;
    long x;

    cu_arena_init_with(&a, 64, 0);
    CU_POOL_INIT(&p, &a, long);
    x = 1;
    /* The first add makes the page, so the second needs no memory: only
     * the pool's own check can catch it. */
    assert(!cu_handle_is_null(CU_POOL_ADD(&p, &x)));
    t = cu_temp_begin(&a);
    (void)t;
    assert(!cu_handle_is_null(CU_POOL_ADD(&p, &x)));
}

static void plex_kind_in_inner_scope(void)
{
    CuArena a;
    CuPlex px;
    CuTemp t;

    cu_arena_init_with(&a, 64, 0);
    cu_plex_init(&px, &a);
    t = cu_temp_begin(&a);
    (void)t;
    assert(CU_PLEX_KIND(&px, long) != 0);
}

static void plex_add_in_inner_scope(void)
{
    CuArena a;
    CuPlex px;
    CuTemp t;
    unsigned kind;
    long x;

    cu_arena_init_with(&a, 64, 0);
    cu_plex_init(&px, &a);
    kind = CU_PLEX_KIND(&px, long);
    assert(kind != 0);
    t = cu_temp_begin(&a);
    (void)t;
    x = 1;
    assert(!cu_link_is_null(CU_PLEX_ADD(&px, kind, &x)));
}

static void builder_append_in_inner_scope(void)
{
    CuArena a;
    CuBuilder b;
    CuTemp t;

    cu_arena_init_with(&a, 64, 0);
    cu_sb_init(&b, &a);
    t = cu_temp_begin(&a);
    (void)t;
    cu_sb_char(&b, 'x');
}

static void death_tests(void)
{
    expect_abort(end_twice);
    expect_abort(end_outer_first);
    expect_abort(end_stale_scope_at_same_depth);
    expect_abort(end_scope_after_release_of_empty_arena);
    expect_abort(end_stale_scratch_scope);
    expect_abort(vec_push_after_release);
    expect_abort(builder_append_after_release);
    expect_abort(vec_push_in_inner_scope);
    expect_abort(vec_reserve_in_inner_scope);
    expect_abort(vec_push_in_later_scope);
    expect_abort(map_upsert_in_inner_scope);
    expect_abort(pool_add_in_inner_scope);
    expect_abort(plex_kind_in_inner_scope);
    expect_abort(plex_add_in_inner_scope);
    expect_abort(builder_append_in_inner_scope);
}
#endif

int main(void)
{
    test_scope_ids_nest();
    test_scope_id_skips_zero();
    test_container_after_release();
    test_containers_inside_their_scope();
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
