#include <stdlib.h>
#include "cu.h"

/* C89 has no thread-local storage. Where the compiler offers none, the
 * scratch arenas are process-wide and must be used from one thread only. */
#if defined(_MSC_VER)
#define CU_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__TINYC__)
#define CU_THREAD_LOCAL __thread
#else
#define CU_THREAD_LOCAL
#endif

#if defined(CU_ARENA_DEBUG) || defined(__SANITIZE_ADDRESS__)
#define CU_DEBUG_DEFAULT 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CU_DEBUG_DEFAULT 1
#endif
#endif
#ifndef CU_DEBUG_DEFAULT
#define CU_DEBUG_DEFAULT 0
#endif

#define CU_DEFAULT_BLOCK_SIZE 65536
#define CU_SCRATCH_COUNT 2

/* Its size is a multiple of every member's alignment, so offsets that are
 * multiples of it from a malloc'd base are aligned for any type. */
typedef union {
    long l;
    double d;
    long double ld;
    void *p;
    void (*f)(void);
} CuMaxAlign;

#define CU_ALIGN (sizeof(CuMaxAlign))

/* Data starts header_size() bytes after the block. */
struct CuBlock {
    CuBlock *prev;
    size_t cap;
    size_t used;
    size_t num; /* 1 for an arena's first block, then counting up */
};

static CU_THREAD_LOCAL CuArena scratch[CU_SCRATCH_COUNT];
static CU_THREAD_LOCAL int scratch_ready;

int cu_size_add(size_t a, size_t b, size_t *out)
{
    if (out == NULL)
        abort();
    if (a > (size_t)-1 - b)
        return 0;
    *out = a + b;
    return 1;
}

int cu_size_mul(size_t a, size_t b, size_t *out)
{
    if (out == NULL)
        abort();
    if (b != 0 && a > (size_t)-1 / b)
        return 0;
    *out = a * b;
    return 1;
}

static size_t header_size(void)
{
    return (sizeof(CuBlock) + CU_ALIGN - 1) / CU_ALIGN * CU_ALIGN;
}

static char *block_data(CuBlock *b)
{
    return (char *)b + header_size();
}

/* A fresh scope id. Unsigned, so it wraps rather than overflows; ids repeat
 * only after (size_t)-1 of them on one arena. 0 is never an id, so a zeroed
 * CuTemp or container never matches. */
static size_t next_scope_id(CuArena *a)
{
    a->scopes++;
    if (a->scopes == 0)
        a->scopes = 1;
    return a->scopes;
}

void cu_arena_init_with(CuArena *a, size_t block_size, int debug)
{
    if (a == NULL)
        abort();
    a->head = NULL;
    a->block_size = block_size;
    a->debug = debug;
    a->failed = 0;
    a->blocks = 0;
    a->held = 0;
    a->limit = (size_t)-1;
    a->refused = 0;
    a->attempts = 0;
    a->fail_at = (size_t)-1;
    a->scopes = 0;
    a->scope = next_scope_id(a);
}

void cu_arena_init(CuArena *a)
{
    cu_arena_init_with(a, CU_DEFAULT_BLOCK_SIZE, CU_DEBUG_DEFAULT);
}

static void free_blocks_until(CuArena *a, CuBlock *stop)
{
    CuBlock *b;
    CuBlock *prev;

    b = a->head;
    while (b != stop) {
        prev = b->prev;
        a->held -= header_size() + b->cap;
        free(b);
        b = prev;
    }
    a->head = stop;
}

void cu_arena_release(CuArena *a)
{
    if (a == NULL)
        abort();
    free_blocks_until(a, NULL);
    a->failed = 0;
    a->refused = 0;
    /* A new base scope: open temp scopes, and containers made before now,
     * no longer match, so ending or growing one aborts. */
    a->scope = next_scope_id(a);
}

static void *push_aligned(CuArena *a, size_t size, size_t align)
{
    CuBlock *b;
    size_t pad;
    size_t room;
    size_t cap;
    size_t total;
    size_t num;
    char *p;

    if (a == NULL)
        abort();
    b = a->head;
    if (b != NULL && !a->debug) {
        pad = (align - b->used % align) % align;
        room = b->cap - b->used;
        if (pad <= room && size <= room - pad) {
            p = block_data(b) + b->used + pad;
            b->used += pad + size;
            return p;
        }
    }
    cap = size;
    if (!a->debug && cap < a->block_size)
        cap = a->block_size;
    if (!cu_size_add(header_size(), cap, &total) || !cu_size_add(a->blocks, 1, &num)) {
        a->failed = 1;
        return NULL;
    }
    /* Counting wraps only after 2^N attempts; the injected one fails. */
    if (a->attempts++ == a->fail_at) {
        a->failed = 1;
        return NULL;
    }
    if (a->held > a->limit || total > a->limit - a->held) {
        a->refused = total;
        a->failed = 1;
        return NULL;
    }
    b = (CuBlock *)malloc(total);
    if (b == NULL) {
        a->failed = 1;
        return NULL;
    }
    a->held += total;
    b->prev = a->head;
    b->cap = cap;
    b->used = size;
    b->num = num;
    a->blocks = num;
    a->head = b;
    return block_data(b);
}

void *cu_arena_push(CuArena *a, size_t count, size_t elem_size)
{
    size_t size;

    if (a == NULL)
        abort();
    if (!cu_size_mul(count, elem_size, &size)) {
        a->failed = 1;
        return NULL;
    }
    return push_aligned(a, size, CU_ALIGN);
}

char *cu_arena_push_bytes(CuArena *a, size_t n)
{
    return (char *)push_aligned(a, n, 1);
}

void cu_arena_set_limit(CuArena *a, size_t max_bytes)
{
    if (a == NULL)
        abort();
    a->limit = max_bytes;
}

void cu_arena_fail_at(CuArena *a, size_t n)
{
    if (a == NULL)
        abort();
    a->fail_at = n;
}

int cu_arena_failed(const CuArena *a)
{
    if (a == NULL)
        abort();
    return a->failed;
}

CuTemp cu_temp_begin(CuArena *a)
{
    CuTemp t;

    if (a == NULL)
        abort();
    t.arena = a;
    t.block = a->head != NULL ? a->head->num : 0;
    t.used = a->head != NULL ? a->head->used : 0;
    t.id = next_scope_id(a);
    t.parent = a->scope;
    a->scope = t.id;
    return t;
}

void cu_temp_end(CuTemp t)
{
    CuBlock *b;

    if (t.arena == NULL)
        abort();
    /* Only the innermost scope may end. This catches scopes ended out of
     * order, ended twice, or ended after the arena was released. */
    if (t.id != t.arena->scope)
        abort();
    /* Backstops for a forged CuTemp: its block must still be in the chain
     * and its position not past the current one. Block numbers are never
     * reused, so a freed block cannot match. */
    b = t.arena->head;
    while (b != NULL && b->num != t.block)
        b = b->prev;
    if (b == NULL && t.block != 0)
        abort();
    free_blocks_until(t.arena, b);
    if (b != NULL) {
        if (t.used > b->used)
            abort();
        b->used = t.used;
    }
    t.arena->scope = t.parent;
}

size_t cu_arena_scope(const CuArena *a)
{
    if (a == NULL)
        abort();
    return a->scope;
}

CuTemp cu_scratch_begin(CuArena **conflicts, size_t count)
{
    size_t i;
    size_t j;
    int clash;

    if (conflicts == NULL && count != 0)
        abort();
    if (!scratch_ready) {
        for (i = 0; i < CU_SCRATCH_COUNT; i++)
            cu_arena_init(&scratch[i]);
        scratch_ready = 1;
    }
    for (i = 0; i < CU_SCRATCH_COUNT; i++) {
        clash = 0;
        for (j = 0; j < count; j++) {
            if (conflicts[j] == &scratch[i])
                clash = 1;
        }
        if (!clash)
            return cu_temp_begin(&scratch[i]);
    }
    abort();
    return cu_temp_begin(&scratch[0]);
}

CuTemp cu_scratch_begin1(CuArena *conflict)
{
    CuArena *conflicts[1];

    conflicts[0] = conflict;
    return cu_scratch_begin(conflicts, 1);
}

void cu_scratch_end(CuTemp t)
{
    cu_temp_end(t);
}

void cu_scratch_release(void)
{
    size_t i;

    if (!scratch_ready)
        return;
    /* Released, not reinitialised: a reinitialised arena would number its
     * scopes from 1 again, and a stale scope could match a new one. */
    for (i = 0; i < CU_SCRATCH_COUNT; i++)
        cu_arena_release(&scratch[i]);
}
