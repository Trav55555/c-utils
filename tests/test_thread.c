#include "death.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
#include "cu.h"

#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
/* No threads here: every start and init fails with ENOSYS, and callers keep
 * a serial path. The threaded tests run natively and on i386. */
static void noop(void *context)
{
    (void)context;
}

int main(void)
{
    CuArena a;
    CuThread *t;
    CuMutex *m;
    CuCond *c;

    cu_arena_init(&a);
    t = NULL;
    m = NULL;
    c = NULL;
    errno = 0;
    assert(!cu_thread_start(&t, &a, 0, noop, NULL) && errno == ENOSYS && t == NULL);
    errno = 0;
    assert(!cu_mutex_init(&m, &a) && errno == ENOSYS && m == NULL);
    errno = 0;
    assert(!cu_cond_init(&c, &a) && errno == ENOSYS && c == NULL);
    cu_arena_release(&a);
    return 0;
}
#else

#define WORKERS 8
#define ROUNDS 20000

typedef struct {
    CuMutex *mutex;
    CuCond *changed;
    long count;
    int ready;
} Shared;

/* One thread, a fixed schedule: it counts once and says so; the starter
 * waits for that before joining. */
static void count_once(void *context)
{
    Shared *s;

    s = (Shared *)context;
    cu_mutex_lock(s->mutex);
    s->count++;
    s->ready = 1;
    cu_cond_broadcast(s->changed);
    cu_mutex_unlock(s->mutex);
}

static void test_one_thread(void)
{
    CuArena a;
    CuThread *t;
    Shared s;

    cu_arena_init(&a);
    assert(cu_mutex_init(&s.mutex, &a));
    assert(cu_cond_init(&s.changed, &a));
    s.count = 0;
    s.ready = 0;
    assert(cu_thread_start(&t, &a, 0, count_once, &s));
    cu_mutex_lock(s.mutex);
    while (!s.ready)
        cu_cond_wait(s.changed, s.mutex);
    cu_mutex_unlock(s.mutex);
    cu_thread_join(t);
    assert(s.count == 1);
    cu_cond_destroy(s.changed);
    cu_mutex_destroy(s.mutex);
    cu_arena_release(&a);
}

/* Each worker uses a scratch arena, which the thread wrapper releases when
 * the worker returns: the leak checkers would report it otherwise. */
static void count_many(void *context)
{
    Shared *s;
    CuTemp scratch;
    char *bytes;
    int i;

    s = (Shared *)context;
    scratch = cu_scratch_begin1(NULL);
    bytes = cu_arena_push_bytes(scratch.arena, 4096);
    assert(bytes != NULL);
    memset(bytes, 1, 4096);
    for (i = 0; i < ROUNDS; i++) {
        cu_mutex_lock(s->mutex);
        s->count += bytes[i % 4096];
        cu_mutex_unlock(s->mutex);
    }
    cu_scratch_end(scratch);
}

/* Small stacks, as cgrep's workers use, and a real race for the lock. */
static void test_many_threads(void)
{
    CuArena a;
    CuThread *t[WORKERS];
    Shared s;
    int i;

    cu_arena_init(&a);
    assert(cu_mutex_init(&s.mutex, &a));
    s.count = 0;
    for (i = 0; i < WORKERS; i++)
        assert(cu_thread_start(&t[i], &a, (size_t)262144, count_many, &s));
    for (i = 0; i < WORKERS; i++)
        cu_thread_join(t[i]);
    assert(s.count == (long)WORKERS * ROUNDS);
    cu_mutex_destroy(s.mutex);
    cu_arena_release(&a);
}

typedef struct {
    CuMutex *mutex;
    CuCond *changed;
    int ready;
    int go;
} StartGate;

static void set_value(void *context)
{
    int *value;

    value = (int *)context;
    *value = 1;
}

/* Start and init can run concurrently when the fault hook is disabled.
 * Each worker owns its arena; only the start gate is shared. */
static void start_from_worker(void *context)
{
    StartGate *gate;
    CuArena a;
    CuMutex *m;
    CuCond *c;
    CuThread *t;
    int value;
    int i;

    gate = (StartGate *)context;
    cu_mutex_lock(gate->mutex);
    gate->ready++;
    cu_cond_broadcast(gate->changed);
    while (!gate->go)
        cu_cond_wait(gate->changed, gate->mutex);
    cu_mutex_unlock(gate->mutex);
    cu_arena_init(&a);
    for (i = 0; i < 8; i++) {
        assert(cu_mutex_init(&m, &a));
        assert(cu_cond_init(&c, &a));
        value = 0;
        assert(cu_thread_start(&t, &a, 0, set_value, &value));
        cu_thread_join(t);
        assert(value == 1);
        cu_cond_destroy(c);
        cu_mutex_destroy(m);
    }
    cu_arena_release(&a);
}

static void test_concurrent_start_and_init(void)
{
    CuArena a;
    CuThread *t[WORKERS];
    StartGate gate;
    int i;

    cu_arena_init(&a);
    assert(cu_mutex_init(&gate.mutex, &a));
    assert(cu_cond_init(&gate.changed, &a));
    gate.ready = 0;
    gate.go = 0;
    for (i = 0; i < WORKERS; i++)
        assert(cu_thread_start(&t[i], &a, 0, start_from_worker, &gate));
    cu_mutex_lock(gate.mutex);
    while (gate.ready != WORKERS)
        cu_cond_wait(gate.changed, gate.mutex);
    gate.go = 1;
    cu_cond_broadcast(gate.changed);
    cu_mutex_unlock(gate.mutex);
    for (i = 0; i < WORKERS; i++)
        cu_thread_join(t[i]);
    cu_cond_destroy(gate.changed);
    cu_mutex_destroy(gate.mutex);
    cu_arena_release(&a);
}

static void test_stack_too_small(void)
{
    CuArena a;
    CuThread *t;

    cu_arena_init(&a);
    errno = 0;
    assert(!cu_thread_start(&t, &a, 1, count_once, NULL));
    assert(errno == EINVAL);
    cu_arena_release(&a);
}

/* Each start and init attempt fails in turn with EAGAIN, and the code that
 * made it can unwind: what came before is still usable and is cleaned up. */
static void test_fails_at_every_attempt(void)
{
    CuArena a;
    CuThread *t;
    Shared s;
    size_t n;
    int ok;

    for (n = 0;; n++) {
        cu_arena_init(&a);
        cu_thread_fail_at(n);
        s.count = 0;
        s.ready = 0;
        ok = 0;
        errno = 0;
        if (cu_mutex_init(&s.mutex, &a)) {
            if (cu_cond_init(&s.changed, &a)) {
                if (cu_thread_start(&t, &a, 0, count_once, &s)) {
                    cu_thread_join(t);
                    ok = 1;
                }
                cu_cond_destroy(s.changed);
            }
            cu_mutex_destroy(s.mutex);
        }
        cu_arena_release(&a);
        if (ok) {
            assert(s.count == 1);
            break;
        }
        assert(errno == EAGAIN && s.count == 0);
    }
    assert(n == 3);
    cu_thread_fail_at((size_t)-1);
}

/* An arena that refuses the object's memory fails the call with ENOMEM. */
static void test_arena_refuses(void)
{
    CuArena a;
    CuMutex *m;
    CuCond *c;
    CuThread *t;

    cu_arena_init_with(&a, 1024, 1);
    cu_arena_fail_at(&a, a.attempts);
    errno = 0;
    assert(!cu_mutex_init(&m, &a) && errno == ENOMEM);
    cu_arena_fail_at(&a, a.attempts);
    errno = 0;
    assert(!cu_cond_init(&c, &a) && errno == ENOMEM);
    cu_arena_fail_at(&a, a.attempts);
    errno = 0;
    assert(!cu_thread_start(&t, &a, 0, count_once, NULL) && errno == ENOMEM);
    cu_arena_release(&a);
}

#ifdef CU_DEATH_TESTS
static void join_twice(void)
{
    CuArena a;
    CuThread *t;
    Shared s;

    cu_arena_init(&a);
    assert(cu_mutex_init(&s.mutex, &a) && cu_cond_init(&s.changed, &a));
    s.count = 0;
    s.ready = 0;
    assert(cu_thread_start(&t, &a, 0, count_once, &s));
    cu_thread_join(t);
    cu_thread_join(t);
}

static void lock_destroyed(void)
{
    CuArena a;
    CuMutex *m;

    cu_arena_init(&a);
    assert(cu_mutex_init(&m, &a));
    cu_mutex_destroy(m);
    cu_mutex_lock(m);
}

static void broadcast_destroyed(void)
{
    CuArena a;
    CuCond *c;

    cu_arena_init(&a);
    assert(cu_cond_init(&c, &a));
    cu_cond_destroy(c);
    cu_cond_broadcast(c);
}

static void start_null_fn(void)
{
    CuArena a;
    CuThread *t;
    int ok;

    cu_arena_init(&a);
    ok = cu_thread_start(&t, &a, 0, NULL, NULL);
    (void)ok;
}
#endif

int main(void)
{
    test_one_thread();
    test_many_threads();
    test_concurrent_start_and_init();
    test_stack_too_small();
    test_fails_at_every_attempt();
    test_arena_refuses();
#ifdef CU_DEATH_TESTS
    expect_abort(join_twice);
    expect_abort(lock_destroyed);
    expect_abort(broadcast_destroyed);
    expect_abort(start_null_fn);
#endif
    return 0;
}
#endif
