#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

#define CU_PAGE_SLOTS 64
#define CU_NO_SLOT ((size_t)-1)
#define CU_LIVE ((size_t)-2)

/* A slot is its header followed by its value. A slot is live exactly when
 * next_free is CU_LIVE. Neither marker can be a slot index: every slot
 * takes more than one byte. */
typedef struct CuSlot {
    unsigned long gen; /* current generation; 0 only while never used */
    size_t next_free;  /* CU_LIVE; else the next reusable slot, or CU_NO_SLOT */
} CuSlot;

/* Its size is a multiple of every member's alignment. */
typedef union {
    long l;
    double d;
    long double ld;
    void *p;
    void (*f)(void);
} MaxAlign;

/* The largest power of two dividing MaxAlign's size: every alignment
 * divides it. Slots are multiples of it apart from a page aligned for any
 * type, so every slot is aligned for any type too. */
#define SLOT_ALIGN (sizeof(MaxAlign) & (0 - sizeof(MaxAlign)))

/* Where a value starts within its slot: after the header, aligned. */
#define VALUE_OFFSET ((sizeof(CuSlot) + SLOT_ALIGN - 1) & ~(SLOT_ALIGN - 1))

/* A page is one arena push of CU_PAGE_SLOTS slots; pages holds a pointer
 * to each. */
void cu_pool_init_with(CuPool *p, CuArena *a, size_t value_size, unsigned long max_gen)
{
    size_t stride;

    if (p == NULL || a == NULL || value_size == 0 || max_gen == 0)
        abort();
    p->arena = a;
    CU_VEC_INIT(&p->pages, a, char *);
    p->slots = 0;
    p->free_head = CU_NO_SLOT;
    p->len = 0;
    p->value_size = value_size;
    /* Rounded up with a mask, not a division: SLOT_ALIGN is a power of
     * two. A size too large leaves stride 0, and the first add fails. */
    if (!cu_size_add(VALUE_OFFSET, value_size, &stride) || !cu_size_add(stride, SLOT_ALIGN - 1, &stride))
        stride = 0;
    p->stride = stride & ~(SLOT_ALIGN - 1);
    p->max_gen = max_gen;
    p->scope = cu_arena_scope(a);
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

/* The slot at index, which must be below p->slots. */
static CuSlot *slot_of(const CuPool *p, size_t index)
{
    char *page;

    page = *CU_VEC_AT_FAST(&p->pages, char *, index / CU_PAGE_SLOTS);
    return (CuSlot *)(void *)(page + index % CU_PAGE_SLOTS * p->stride);
}

static void *value_of(CuSlot *s)
{
    return (char *)s + VALUE_OFFSET;
}

/* The live slot h names, or NULL. Checks the index against the slots made
 * before touching any slot, so a forged index is never dereferenced. */
static CuSlot *live_slot(const CuPool *p, CuHandle h)
{
    CuSlot *s;

    if (h.gen == 0 || h.index >= p->slots)
        return NULL;
    s = slot_of(p, h.index);
    if (s->next_free != CU_LIVE || s->gen != h.gen)
        return NULL;
    return s;
}

/* A fresh slot at the end, adding a page when the last one is full. */
static int new_slot(CuPool *p, size_t *index)
{
    char *page;
    CuSlot *s;

    if (p->slots % CU_PAGE_SLOTS == 0) {
        if (p->stride == 0)
            return 0;
        page = (char *)cu_arena_push(p->arena, CU_PAGE_SLOTS, p->stride);
        if (page == NULL)
            return 0;
        CU_VEC_PUSH(&p->pages, &page);
        if (cu_vec_failed(&p->pages))
            return 0;
    }
    *index = p->slots;
    s = slot_of(p, *index);
    s->gen = 0;
    s->next_free = CU_NO_SLOT;
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
    /* Checked even when a free slot needs no memory, so a wrong scope
     * aborts whatever the pool's state. */
    if (p->arena->scope != p->scope)
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
    s->next_free = CU_LIVE;
    memcpy(value_of(s), v, p->value_size);
    p->len++;
    h.index = index;
    h.gen = s->gen;
    return h;
}

void *cu_pool_get_(const CuPool *p, CuHandle h, size_t value_size)
{
    CuSlot *s;

    if (p == NULL || value_size != p->value_size)
        abort();
    s = live_slot(p, h);
    return s != NULL ? value_of(s) : NULL;
}

int cu_pool_remove(CuPool *p, CuHandle h)
{
    CuSlot *s;

    if (p == NULL)
        abort();
    s = live_slot(p, h);
    if (s == NULL)
        return 0;
    p->len--;
    /* At max_gen the slot is retired: reusing it would need a generation
     * past the limit, or wrapping to one an old handle may hold. */
    if (s->gen < p->max_gen) {
        s->next_free = p->free_head;
        p->free_head = h.index;
    } else {
        s->next_free = CU_NO_SLOT;
    }
    return 1;
}
