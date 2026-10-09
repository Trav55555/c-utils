#include <limits.h>
#include <stdlib.h>
#include "cu.h"

void cu_plex_init(CuPlex *px, CuArena *a)
{
    if (px == NULL || a == NULL)
        abort();
    px->arena = a;
    CU_VEC_INIT(&px->kinds, a, CuPool);
    px->failed = 0;
}

int cu_plex_failed(const CuPlex *px)
{
    if (px == NULL)
        abort();
    return px->failed;
}

CuLink cu_link_null(void)
{
    CuLink link;

    link.kind = 0;
    link.h = cu_handle_null();
    return link;
}

int cu_link_is_null(CuLink link)
{
    return link.kind == 0;
}

int cu_link_eq(CuLink a, CuLink b)
{
    if (a.kind == 0 || b.kind == 0)
        return a.kind == b.kind;
    return a.kind == b.kind && a.h.index == b.h.index && a.h.gen == b.h.gen;
}

unsigned cu_plex_kind(CuPlex *px, size_t node_size)
{
    CuPool pool;

    if (px == NULL || node_size == 0)
        abort();
    if (cu_arena_scope(px->arena) != px->kinds.scope)
        abort();
    if (px->failed)
        return 0;
    if (cu_vec_len(&px->kinds) >= UINT_MAX) {
        px->failed = 1;
        return 0;
    }
    cu_pool_init(&pool, px->arena, node_size);
    CU_VEC_PUSH(&px->kinds, &pool);
    if (cu_vec_failed(&px->kinds)) {
        px->failed = 1;
        return 0;
    }
    return (unsigned)cu_vec_len(&px->kinds);
}

/* The pool for a kind the caller names; an unknown kind is a programmer
 * error. */
static CuPool *pool_of(const CuPlex *px, unsigned kind)
{
    if (px == NULL || kind == 0 || kind > cu_vec_len(&px->kinds))
        abort();
    return CU_VEC_AT(&px->kinds, CuPool, kind - 1);
}

CuLink cu_plex_add_(CuPlex *px, unsigned kind, const void *v, size_t node_size)
{
    CuPool *pool;
    CuLink link;

    pool = pool_of(px, kind);
    if (cu_arena_scope(px->arena) != pool->scope)
        abort();
    if (px->failed)
        return cu_link_null();
    link.h = cu_pool_add_(pool, v, node_size);
    if (cu_handle_is_null(link.h)) {
        px->failed = 1;
        return cu_link_null();
    }
    link.kind = kind;
    return link;
}

void *cu_plex_get_(const CuPlex *px, unsigned kind, CuLink link, size_t node_size)
{
    CuPool *pool;

    pool = pool_of(px, kind);
    if (link.kind != kind) {
        /* Still check the size, so a type mix-up aborts on every path. */
        if (node_size != pool->value_size)
            abort();
        return NULL;
    }
    return cu_pool_get_(pool, link.h, node_size);
}

int cu_plex_remove(CuPlex *px, CuLink link)
{
    if (px == NULL)
        abort();
    if (link.kind == 0 || link.kind > cu_vec_len(&px->kinds))
        return 0;
    return cu_pool_remove(CU_VEC_AT(&px->kinds, CuPool, link.kind - 1), link.h);
}
