#include "death.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include "cu.h"

static int once(CuTask *task, void *context)
{
    int *n;
    unsigned long left;

    n = (int *)context;
    assert(cu_task_remaining(task, &left) && left > 0);
    (*n)++;
    return 1;
}

#if !defined(__cerb__) && !defined(__wasi__) && (defined(__unix__) || defined(__APPLE__))
#include <time.h>

static void pause_ms(unsigned long ms)
{
    struct timespec t;

    t.tv_sec = (time_t)(ms / 1000);
    t.tv_nsec = (long)((ms % 1000) * 1000000UL);
    while (nanosleep(&t, &t) != 0)
        assert(errno == EINTR);
}

static int fail(CuTask *task, void *context)
{
    (void)task;
    assert(errno == 0);
    errno = *(int *)context;
    return 0;
}

static int cancel(CuTask *task, void *context)
{
    int *n;
    unsigned long left;

    n = (int *)context;
    (*n)++;
    cu_task_cancel(task);
    assert(cu_task_cancelled(task));
    assert(!cu_task_remaining(task, &left) && left == 0 && errno == ECANCELED);
    return 1;
}

static int delay(CuTask *task, void *context)
{
    int *n;

    (void)task;
    n = (int *)context;
    (*n)++;
    pause_ms(70);
    return 1;
}

typedef struct Shared {
    CuMutex *mutex;
    int active;
    int peak;
    int entered;
    int exited;
    int overlap;
    int wait_cancel;
    int cancelled;
} Shared;

/* No unbounded rendezvous: cancellation/deadline always releases a waiter. */
static int together(CuTask *task, void *context)
{
    Shared *s;
    int ready;

    s = (Shared *)context;
    cu_mutex_lock(s->mutex);
    s->active++;
    s->entered++;
    if (s->active > s->peak)
        s->peak = s->active;
    if (s->active > 1)
        s->overlap = 1;
    cu_mutex_unlock(s->mutex);
    do {
        cu_mutex_lock(s->mutex);
        ready = s->overlap && !s->wait_cancel;
        cu_mutex_unlock(s->mutex);
        if (ready || cu_task_cancelled(task))
            break;
        pause_ms(1);
    } while (1);
    cu_mutex_lock(s->mutex);
    s->active--;
    s->exited++;
    if (!ready)
        s->cancelled++;
    cu_mutex_unlock(s->mutex);
    return 1;
}

static void test_counts(void)
{
    CuArena a;
    CuTaskSpec jobs[12];
    int counts[12];
    size_t i;

    cu_arena_init(&a);
    for (i = 0; i < 12; i++) {
        counts[i] = 0;
        jobs[i].fn = once;
        jobs[i].context = &counts[i];
    }
    assert(cu_tasks_run(&a, jobs, 12, 3, 3000));
    for (i = 0; i < 12; i++)
        assert(counts[i] == 1);
    assert(cu_tasks_run(&a, jobs, 1, 9, ULONG_MAX));
    assert(counts[0] == 2);
    cu_arena_release(&a);
}

static void test_stops(void)
{
    CuArena a;
    CuTaskSpec jobs[4];
    int n;
    int error;
    int i;

    cu_arena_init(&a);
    n = 0;
    for (i = 0; i < 4; i++) {
        jobs[i].fn = once;
        jobs[i].context = &n;
    }
    jobs[0].fn = fail;
    jobs[0].context = &error;
    error = EDOM;
    assert(!cu_tasks_run(&a, jobs, 4, 1, 3000) && errno == EDOM);
    assert(n == 0);
    error = 0;
    errno = EINVAL;
    assert(!cu_tasks_run(&a, jobs, 4, 1, 3000) && errno == 0);
    jobs[0].fn = cancel;
    jobs[0].context = &n;
    assert(!cu_tasks_run(&a, jobs, 4, 1, 3000) && errno == ECANCELED);
    assert(n == 1);
    n = 0;
    for (i = 0; i < 4; i++)
        jobs[i].fn = delay;
    assert(!cu_tasks_run(&a, jobs, 4, 1, 110) && errno == ETIMEDOUT);
    assert(n <= 2);
    n = 0;
    assert(!cu_tasks_run(&a, jobs, 1, 1, 20) && errno == ETIMEDOUT);
    assert(n <= 1);
    cu_arena_release(&a);
}

typedef struct Errors {
    CuMutex *mutex;
    int second_entered;
    int exited;
} Errors;

static int first_zero(CuTask *task, void *context)
{
    Errors *e;
    int ready;

    e = (Errors *)context;
    do {
        cu_mutex_lock(e->mutex);
        ready = e->second_entered;
        cu_mutex_unlock(e->mutex);
        if (ready || cu_task_cancelled(task)) break;
        pause_ms(1);
    } while (1);
    cu_mutex_lock(e->mutex);
    e->exited++;
    cu_mutex_unlock(e->mutex);
    errno = 0;
    return 0;
}

static int later_error(CuTask *task, void *context)
{
    Errors *e;

    e = (Errors *)context;
    cu_mutex_lock(e->mutex);
    e->second_entered = 1;
    cu_mutex_unlock(e->mutex);
    while (!cu_task_cancelled(task)) pause_ms(1);
    cu_task_cancel(task);
    cu_mutex_lock(e->mutex);
    e->exited++;
    cu_mutex_unlock(e->mutex);
    errno = EDOM;
    return 0;
}

static void test_first_error(void)
{
    CuArena control;
    CuArena sync;
    CuTaskSpec jobs[2];
    Errors errors;

    cu_arena_init(&control);
    cu_arena_init(&sync);
    assert(cu_mutex_init(&errors.mutex, &sync));
    errors.second_entered = 0;
    errors.exited = 0;
    jobs[0].fn = first_zero;
    jobs[0].context = &errors;
    jobs[1].fn = later_error;
    jobs[1].context = &errors;
    assert(!cu_tasks_run(&control, jobs, 2, 2, 3000) && errno == 0);
    assert(errors.second_entered && errors.exited == 2);
    cu_mutex_destroy(errors.mutex);
    cu_arena_release(&sync);
    cu_arena_release(&control);
}

static void test_workers(void)
{
    CuArena a;
    CuArena sync;
    Shared s;
    CuTaskSpec jobs[8];
    int hook;
    int i;
    int before;

    cu_arena_init(&sync);
    assert(cu_mutex_init(&s.mutex, &sync));
    for (i = 0; i < 8; i++) {
        jobs[i].fn = together;
        jobs[i].context = &s;
    }
    for (hook = -1; hook <= 3; hook++) {
        cu_arena_init(&a);
        s.active = 0;
        s.peak = 0;
        s.entered = 0;
        s.exited = 0;
        s.overlap = 0;
        s.wait_cancel = hook >= 0;
        s.cancelled = 0;
        cu_thread_fail_at(hook < 0 ? (size_t)-1 : (size_t)hook);
        if (hook < 0) {
            assert(cu_tasks_run(&a, jobs, 8, 3, 3000));
            assert(s.peak > 1 && s.peak <= 3 && s.entered == 8);
        } else {
            assert(!cu_tasks_run(&a, jobs, 8, 3, 3000) && errno == EAGAIN);
            assert(s.cancelled == s.entered);
        }
        cu_thread_fail_at((size_t)-1);
        assert(s.active == 0 && s.entered == s.exited);
        before = s.exited;
        pause_ms(2);
        assert(s.exited == before);
        cu_arena_release(&a);
    }
    cu_mutex_destroy(s.mutex);
    cu_arena_release(&sync);
}

static void test_allocations(void)
{
    CuArena a;
    CuTaskSpec job;
    size_t index;
    size_t attempts;
    int n;
    int ok;

    job.fn = once;
    job.context = &n;
    index = 0;
    do {
        cu_arena_init_with(&a, 64, 1);
        cu_arena_fail_at(&a, index);
        n = 0;
        ok = cu_tasks_run(&a, &job, 1, 1, 3000);
        attempts = a.attempts;
        if (index < attempts)
            assert(!ok && errno == ENOMEM);
        else
            assert(ok && n == 1);
        cu_arena_release(&a);
        index++;
    } while (index <= attempts);
    cu_arena_init(&a);
    cu_arena_set_limit(&a, 0);
    assert(!cu_tasks_run(&a, &job, 1, 1, 3000) && errno == ENOMEM);
    cu_arena_release(&a);
}
#endif

#ifdef CU_DEATH_TESTS
static int bad_kind;
static void bad(void)
{
    CuArena a;
    CuTaskSpec job;
    int ok;
    int n;

    cu_arena_init(&a);
    job.fn = bad_kind == 4 ? NULL : once;
    job.context = &n;
    ok = cu_tasks_run(bad_kind == 0 ? NULL : &a,
                      bad_kind == 1 ? NULL : &job, 1,
                      bad_kind == 2 ? 0 : 1, bad_kind == 3 ? 0 : 100);
    (void)ok;
}
#endif

int main(void)
{
    CuArena a;
#if defined(CU_DEATH_TESTS)
    int i;
#endif

    cu_arena_init(&a);
    cu_arena_set_limit(&a, 0);
    assert(cu_tasks_run(&a, NULL, 0, 1, 1));
    assert(a.attempts == 0);
    cu_arena_release(&a);
#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
    {
        CuTaskSpec job;
        int n;

        n = 0;
        job.fn = once;
        job.context = &n;
        assert(!cu_tasks_run(&a, &job, 1, 1, 100) && errno == ENOSYS);
        assert(n == 0 && a.attempts == 0);
    }
#else
    test_counts();
    test_stops();
    test_first_error();
    test_workers();
    test_allocations();
#endif
#ifdef CU_DEATH_TESTS
    for (i = 0; i < 5; i++) {
        bad_kind = i;
        expect_abort(bad);
    }
#endif
    return 0;
}
