#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdlib.h>
#include "cu.h"

#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
#define CU_TASK_NONE 1
#endif
#ifndef ENOSYS
#define ENOSYS EINVAL
#endif
#ifndef ECANCELED
#define ECANCELED EINVAL
#endif

struct CuTask {
    CuMutex *mutex;
    const CuTaskSpec *jobs;
    size_t count;
    size_t next;
    int stopped;
    int error;
    unsigned long sec;
    unsigned long nsec;
    unsigned long timeout;
};

typedef struct TaskWorker {
    CuThread *thread;
} TaskWorker;

/* Caller holds the mutex (or no workers exist yet). */
static void stop(CuTask *task, int error)
{
    if (!task->stopped) {
        task->stopped = 1;
        task->error = error;
    }
}

#ifndef CU_TASK_NONE
#include <time.h>

static int stamp(unsigned long *sec, unsigned long *nsec)
{
    struct timespec t;

    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return 0;
    *sec = (unsigned long)t.tv_sec;
    *nsec = (unsigned long)t.tv_nsec;
    return 1;
}

static int remaining(CuTask *task, unsigned long *left)
{
    unsigned long sec;
    unsigned long nsec;
    unsigned long elapsed;
    unsigned long used;

    *left = 0;
    if (!task->stopped) {
        if (!stamp(&sec, &nsec)) {
            stop(task, errno);
        } else {
            elapsed = sec - task->sec;
            if (nsec < task->nsec) {
                elapsed--;
                nsec = 1000000000UL - task->nsec + nsec;
            } else {
                nsec -= task->nsec;
            }
            if (elapsed > task->timeout / 1000) {
                stop(task, ETIMEDOUT);
            } else {
                *left = task->timeout - elapsed * 1000;
                used = nsec / 1000000UL;
                if (used >= *left)
                    stop(task, ETIMEDOUT);
                else
                    *left -= used;
            }
        }
    }
    if (task->stopped) {
        *left = 0;
        errno = task->error;
        return 0;
    }
    return 1;
}
#else
static int remaining(CuTask *task, unsigned long *left)
{
    (void)task;
    *left = 0;
    errno = ENOSYS;
    return 0;
}
#endif

int cu_task_remaining(CuTask *task, unsigned long *remaining_ms)
{
    int ok;
    int error;

    if (task == NULL || remaining_ms == NULL)
        abort();
    cu_mutex_lock(task->mutex);
    ok = remaining(task, remaining_ms);
    error = task->error;
    cu_mutex_unlock(task->mutex);
    if (!ok)
        errno = error;
    return ok;
}

int cu_task_cancelled(CuTask *task)
{
    unsigned long left;

    return !cu_task_remaining(task, &left);
}

void cu_task_cancel(CuTask *task)
{
    unsigned long left;

    if (task == NULL)
        abort();
    cu_mutex_lock(task->mutex);
    /* Observe an already expired deadline before explicit cancellation. */
    if (remaining(task, &left))
        stop(task, ECANCELED);
    cu_mutex_unlock(task->mutex);
}

#ifndef CU_TASK_NONE
static void work(void *context)
{
    CuTask *task;
    CuTaskSpec job;
    unsigned long left;
    int ok;
    int error;

    task = (CuTask *)context;
    for (;;) {
        cu_mutex_lock(task->mutex);
        if (!remaining(task, &left) || task->next == task->count) {
            cu_mutex_unlock(task->mutex);
            return;
        }
        job = task->jobs[task->next];
        task->next++;
        cu_mutex_unlock(task->mutex);
        errno = 0;
        ok = job.fn(task, job.context);
        error = errno;
        cu_mutex_lock(task->mutex);
        if (!ok)
            stop(task, error);
        ok = remaining(task, &left);
        cu_mutex_unlock(task->mutex);
        if (!ok)
            return;
    }
}
#endif

int cu_tasks_run(CuArena *control, const CuTaskSpec *jobs, size_t count,
                 size_t max_workers, unsigned long timeout_ms)
{
    size_t i;
#ifndef CU_TASK_NONE
    CuTask *task;
    CuMutex *mutex;
    CuThread *thread;
    TaskWorker *workers;
    size_t started;
    unsigned long sec;
    unsigned long nsec;
    unsigned long left;
    int ok;
    int error;
#endif

    if (control == NULL || max_workers == 0 || timeout_ms == 0 ||
        (count != 0 && jobs == NULL))
        abort();
    for (i = 0; i < count; i++) {
        if (jobs[i].fn == NULL)
            abort();
    }
    if (count == 0)
        return 1;
#ifdef CU_TASK_NONE
    errno = ENOSYS;
    return 0;
#else
    if (!stamp(&sec, &nsec))
        return 0;
    if (max_workers > count)
        max_workers = count;
    task = CU_PUSH(control, CuTask, 1);
    workers = CU_PUSH(control, TaskWorker, max_workers);
    if (task == NULL || workers == NULL) {
        errno = ENOMEM;
        return 0;
    }
    task->jobs = NULL;
    task->count = count;
    task->next = 0;
    task->stopped = 0;
    task->error = 0;
    task->sec = sec;
    task->nsec = nsec;
    task->timeout = timeout_ms;
    if (!cu_mutex_init(&mutex, control))
        return 0;
    task->mutex = mutex;
    task->jobs = jobs;
    started = 0;
    for (i = 0; i < max_workers; i++) {
        cu_mutex_lock(task->mutex);
        ok = remaining(task, &left);
        cu_mutex_unlock(task->mutex);
        if (!ok)
            break;
        if (!cu_thread_start(&thread, control, 0, work, task)) {
            error = errno;
            cu_mutex_lock(task->mutex);
            stop(task, error);
            cu_mutex_unlock(task->mutex);
            break;
        }
        workers[i].thread = thread;
        started++;
    }
    for (i = 0; i < started; i++)
        cu_thread_join(workers[i].thread);
    ok = remaining(task, &left);
    error = task->error;
    task->jobs = NULL;
    cu_mutex_destroy(task->mutex);
    task->mutex = NULL;
    for (i = 0; i < max_workers; i++)
        workers[i].thread = NULL;
    if (!ok)
        errno = error;
    return ok;
#endif
}
