/* CBMC proof: in either arena mode, growing an allocation alone in its
 * block reallocates the block, and realloc may fail. On failure
 * cu_arena_grow returns NULL, marks the arena failed, and leaves held and
 * the old bytes as they were; on success the bytes move and held grows by
 * exactly the bytes added. Releasing the arena returns held to 0 either
 * way, so the block was never dropped from the arena.
 *
 * No test reaches the failure: cu_arena_fail_at and the limit refuse
 * before realloc runs, and a real failure needs a request too big for the
 * sanitizers to return NULL. One byte, at a symbolic position below OLD,
 * carries a symbolic value, so every position is checked without a loop
 * over the bytes, which would pass dcheck's unwind bound. The allocators
 * below call __CPROVER_allocate, which --memory-leak-check does not track,
 * so held, not the leak check, shows that no block was lost. The fault
 * controls (tools/fault_controls.py) show each assertion failing. */
#include <stdlib.h>
#include <string.h>
#include "cu.h"

#define OLD 16
#define NEW 40

int nondet_int(void);
char nondet_char(void);
size_t nondet_size(void);

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
    char byte;
    size_t at;
    char *p;
    char *q;
    size_t held;

    cu_arena_init_with(&a, OLD, nondet_int() != 0);
    p = cu_arena_push_bytes(&a, OLD);
    __CPROVER_assert(p != NULL, "push succeeds");
    at = nondet_size();
    __CPROVER_assume(at < OLD);
    byte = nondet_char();
    p[at] = byte;
    held = a.held;
    q = (char *)cu_arena_grow(&a, p, OLD, NEW);
    if (q == NULL) {
        __CPROVER_assert(cu_arena_failed(&a), "a failed grow marks the arena");
        __CPROVER_assert(a.held == held, "a failed grow holds nothing more");
        __CPROVER_assert(p[at] == byte, "a failed grow keeps the old bytes");
    } else {
        __CPROVER_assert(!cu_arena_failed(&a), "a grow that succeeds leaves the arena unmarked");
        __CPROVER_assert(a.held == held + (NEW - OLD), "held grows by the bytes added");
        __CPROVER_assert(q[at] == byte, "the bytes move with the grow");
    }
    cu_arena_release(&a);
    __CPROVER_assert(a.held == 0, "release returns every held byte");
    return 0;
}
