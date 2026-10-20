#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include "cu.h"

#if !defined(__cerb__) && !defined(__wasi__) && (defined(__unix__) || defined(__APPLE__))
#include <time.h>

static CuMutex *observer;
static int starts;
static int entered;
static int exited;
static int joining;

static void tick(void)
{
    struct timespec t;

    t.tv_sec = 0;
    t.tv_nsec = 1000000L;
    while (nanosleep(&t, &t) != 0) assert(errno == EINTR);
}

static int test_start(CuThread **out, CuArena *arena, size_t stack,
                       void (*fn)(void *), void *context)
{
    int ready;
    int i;

    starts++;
    if (starts == 2) {
        ready = 0;
        for (i = 0; i < 1000; i++) {
            cu_mutex_lock(observer);
            ready = entered;
            cu_mutex_unlock(observer);
            if (ready) break;
            tick();
        }
        assert(ready == 1);
        errno = EAGAIN;
        return 0;
    }
    return cu_thread_start(out, arena, stack, fn, context);
}

static void test_join(CuThread *thread)
{
    cu_mutex_lock(observer);
    joining = 1;
    cu_mutex_unlock(observer);
    cu_thread_join(thread);
}

int tasks_run_probe(CuArena *control, const CuTaskSpec *jobs, size_t count,
                    size_t max_workers, unsigned long timeout_ms);
int task_cancelled_probe(CuTask *task);
int task_remaining_probe(CuTask *task, unsigned long *remaining_ms);
void task_cancel_probe(CuTask *task);
#define cu_thread_start test_start
#define cu_thread_join test_join
#define cu_tasks_run tasks_run_probe
#define cu_task_cancelled task_cancelled_probe
#define cu_task_remaining task_remaining_probe
#define cu_task_cancel task_cancel_probe
#ifndef CU_TASK_SOURCE
#define CU_TASK_SOURCE "../src/cu_task.c"
#endif
#include CU_TASK_SOURCE
#undef cu_thread_start
#undef cu_thread_join
#undef cu_tasks_run
#undef cu_task_cancelled
#undef cu_task_remaining
#undef cu_task_cancel

static int waiting(CuTask *task, void *context)
{
    int ready;
    int i;

    (void)context;
    cu_mutex_lock(observer);
    entered++;
    cu_mutex_unlock(observer);
    while (!task_cancelled_probe(task)) tick();
    ready = 0;
    for (i = 0; i < 1000; i++) {
        cu_mutex_lock(observer);
        ready = joining;
        cu_mutex_unlock(observer);
        if (ready) break;
        tick();
    }
    assert(ready);
    cu_mutex_lock(observer);
    exited++;
    cu_mutex_unlock(observer);
    return 1;
}
#endif

int main(void)
{
#if !defined(__cerb__) && !defined(__wasi__) && (defined(__unix__) || defined(__APPLE__))
    CuArena control;
    CuArena sync;
    CuTaskSpec jobs[2];

    cu_arena_init(&control);
    cu_arena_init(&sync);
    assert(cu_mutex_init(&observer, &sync));
    starts = entered = exited = joining = 0;
    jobs[0].fn = waiting;
    jobs[0].context = NULL;
    jobs[1] = jobs[0];
    assert(!tasks_run_probe(&control, jobs, 2, 2, 3000) && errno == EAGAIN);
    assert(entered == 1 && exited == 1 && joining == 1);
    cu_mutex_destroy(observer);
    cu_arena_release(&sync);
    cu_arena_release(&control);
#endif
    return 0;
}
