/* CBMC proof: in either arena mode, growing an allocation alone in its
 * block reallocates the block, and realloc may fail. On failure
 * cu_arena_grow returns NULL, marks the arena failed, and leaves held and
 * the old bytes as they were; on success the bytes move and held grows by
 * exactly the bytes added. The arena releases cleanly either way.
 *
 * No test reaches the failure: cu_arena_fail_at and the limit refuse
 * before realloc runs, and a real failure needs a request too big for the
 * sanitizers to return NULL. The first and last bytes carry symbolic
 * values; a loop over all of them would pass dcheck's unwind bound. */
#include <stdlib.h>
#include <string.h>
#include "cu.h"

#define OLD 16
#define NEW 40

int nondet_int(void);
char nondet_char(void);

/* Pushes never fail here, so the only failure is the realloc's. */
void *malloc(size_t n)
{
    return __CPROVER_allocate(n, 0);
}

/* Fails or succeeds nondeterministically. On success it copies the old
 * object and frees it, as the C library does. */
void *realloc(void *p, size_t n)
{
    char *q;
    size_t old;

    if (nondet_int())
        return NULL;
    q = (char *)__CPROVER_allocate(n, 0);
    old = __CPROVER_OBJECT_SIZE(p);
    memcpy(q, p, old < n ? old : n);
    free(p);
    return q;
}

int main(void)
{
    CuArena a;
    char first;
    char last;
    char *p;
    char *q;
    size_t held;

    cu_arena_init_with(&a, OLD, nondet_int() != 0);
    p = cu_arena_push_bytes(&a, OLD);
    __CPROVER_assert(p != NULL, "push succeeds");
    first = nondet_char();
    last = nondet_char();
    p[0] = first;
    p[OLD - 1] = last;
    held = a.held;
    q = (char *)cu_arena_grow(&a, p, OLD, NEW);
    if (q == NULL) {
        __CPROVER_assert(cu_arena_failed(&a), "a failed grow marks the arena");
        __CPROVER_assert(a.held == held, "a failed grow holds nothing more");
        __CPROVER_assert(p[0] == first && p[OLD - 1] == last, "a failed grow keeps the old bytes");
    } else {
        __CPROVER_assert(!cu_arena_failed(&a), "a grow that succeeds leaves the arena unmarked");
        __CPROVER_assert(a.held == held + (NEW - OLD), "held grows by the bytes added");
        __CPROVER_assert(q[0] == first && q[OLD - 1] == last, "the bytes move with the grow");
    }
    cu_arena_release(&a);
    return 0;
}
