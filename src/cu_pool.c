#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

#define CU_PAGE_SLOTS 64
#define CU_NO_SLOT ((size_t)-1)

typedef struct CuSlot {
    unsigned long gen; /* current generation; 0 only while never used */
    size_t next_free;  /* next reusable slot while free */
    int live;
} CuSlot;

/* Slot headers and values are separate arrays, so each value sits at a
 * multiple of value_size from an allocation aligned for any type. */
typedef struct CuPage {
    CuSlot *slots;
    char *values;
} CuPage;

void cu_pool_init_with(CuPool *p, CuArena *a, size_t value_size, unsigned long max_gen)
{
    if (p == NULL || a == NULL || value_size == 0 || max_gen == 0)
        abort();
    p->arena = a;
    CU_VEC_INIT(&p->pages, a, CuPage);
    p->slots = 0;
    p->free_head = CU_NO_SLOT;
    p->len = 0;
    p->value_size = value_size;
    p->max_gen = max_gen;
    p->failed = 0;
}

void cu_pool_init(CuPool *p, CuArena *a, size_t value_size)
{
    cu_pool_init_with(p, a, value_size, ULONG_MAX);
}

size_t cu_pool_len(const CuPool *p)
{
    if (p == NULL)
        abort();
    return p->len;
}

int cu_pool_failed(const CuPool *p)
{
    if (p == NULL)
        abort();
    return p->failed;
}

CuHandle cu_handle_null(void)
{
    CuHandle h;

    h.index = 0;
    h.gen = 0;
    return h;
}

int cu_handle_is_null(CuHandle h)
{
    return h.gen == 0;
}

static CuPage *page_of(const CuPool *p, size_t index)
{
    return CU_VEC_AT(&p->pages, CuPage, index / CU_PAGE_SLOTS);
}

static CuSlot *slot_of(const CuPool *p, size_t index)
{
    return &page_of(p, index)->slots[index % CU_PAGE_SLOTS];
}

static void *value_of(const CuPool *p, size_t index)
{
    return page_of(p, index)->values + index % CU_PAGE_SLOTS * p->value_size;
}

/* The live slot h names, or NULL. Checks the index against the slots made
 * before touching any slot, so a forged index is never dereferenced. */
static CuSlot *live_slot(const CuPool *p, CuHandle h)
{
    CuSlot *s;

    if (h.gen == 0 || h.index >= p->slots)
        return NULL;
    s = slot_of(p, h.index);
    if (!s->live || s->gen != h.gen)
        return NULL;
    return s;
}

/* A fresh slot at the end, adding a page when the last one is full. */
static int new_slot(CuPool *p, size_t *index)
{
    CuPage page;
    CuSlot *s;

    if (p->slots % CU_PAGE_SLOTS == 0) {
        page.slots = CU_PUSH(p->arena, CuSlot, CU_PAGE_SLOTS);
        page.values = (char *)cu_arena_push(p->arena, CU_PAGE_SLOTS, p->value_size);
        if (page.slots == NULL || page.values == NULL)
            return 0;
        CU_VEC_PUSH(&p->pages, &page);
        if (cu_vec_failed(&p->pages))
            return 0;
    }
    *index = p->slots;
    s = slot_of(p, *index);
    s->gen = 0;
    s->next_free = CU_NO_SLOT;
    s->live = 0;
    p->slots++;
    return 1;
}

CuHandle cu_pool_add_(CuPool *p, const void *v, size_t value_size)
{
    CuHandle h;
    CuSlot *s;
    size_t index;

    if (p == NULL || v == NULL || value_size != p->value_size)
        abort();
    if (p->failed)
        return cu_handle_null();
    if (p->free_head != CU_NO_SLOT) {
        index = p->free_head;
        p->free_head = slot_of(p, index)->next_free;
    } else if (!new_slot(p, &index)) {
        p->failed = 1;
        return cu_handle_null();
    }
    s = slot_of(p, index);
    s->gen++;
    s->live = 1;
    memcpy(value_of(p, index), v, p->value_size);
    p->len++;
    h.index = index;
    h.gen = s->gen;
    return h;
}

void *cu_pool_get_(const CuPool *p, CuHandle h, size_t value_size)
{
    if (p == NULL || value_size != p->value_size)
        abort();
    if (live_slot(p, h) == NULL)
        return NULL;
    return value_of(p, h.index);
}

int cu_pool_remove(CuPool *p, CuHandle h)
{
    CuSlot *s;

    if (p == NULL)
        abort();
    s = live_slot(p, h);
    if (s == NULL)
        return 0;
    s->live = 0;
    p->len--;
    /* At max_gen the slot is retired: reusing it would need a generation
     * past the limit, or wrapping to one an old handle may hold. */
    if (s->gen < p->max_gen) {
        s->next_free = p->free_head;
        p->free_head = h.index;
    }
    return 1;
}
