/* Exercise platform failures separately from the public fail-at hook. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>

#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
int main(void)
{
    return 0;
}
#else
#include <pthread.h>

/* A private copy of the implementation, as in test_file_seek.c. */
#define cu_thread_fail_at thread_fail_at_probe
#define cu_thread_start thread_start_probe
#define cu_thread_join thread_join_probe
#define cu_mutex_init mutex_init_probe
#define cu_mutex_lock mutex_lock_probe
#define cu_mutex_unlock mutex_unlock_probe
#define cu_mutex_destroy mutex_destroy_probe
#define cu_cond_init cond_init_probe
#define cu_cond_wait cond_wait_probe
#define cu_cond_broadcast cond_broadcast_probe
#define cu_cond_destroy cond_destroy_probe
#include "cu.h"

static int fail_step;
static int attr_live;
static int destroys;

static int attr_init(pthread_attr_t *attr)
{
    int code;

    if (fail_step == 1)
        return ENOMEM;
    code = pthread_attr_init(attr);
    if (code == 0)
        attr_live = 1;
    return code;
}

static int attr_stack(pthread_attr_t *attr, size_t size)
{
    assert(attr_live);
    if (fail_step == 2)
        return EINVAL;
    return pthread_attr_setstacksize(attr, size);
}

static int attr_destroy(pthread_attr_t *attr)
{
    assert(attr_live);
    attr_live = 0;
    destroys++;
    return pthread_attr_destroy(attr);
}

static int thread_create(pthread_t *id, const pthread_attr_t *attr,
                         void *(*fn)(void *), void *context)
{
    assert(attr_live);
    if (fail_step == 3)
        return EAGAIN;
    return pthread_create(id, attr, fn, context);
}

static int mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    if (fail_step == 4)
        return EAGAIN;
    return pthread_mutex_init(m, attr);
}

static int cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
    if (fail_step == 5)
        return EAGAIN;
    return pthread_cond_init(c, attr);
}

#define pthread_attr_init attr_init
#define pthread_attr_setstacksize attr_stack
#define pthread_attr_destroy attr_destroy
#define pthread_create thread_create
#define pthread_mutex_init mutex_init
#define pthread_cond_init cond_init
#include "../src/cu_thread.c"
#undef pthread_attr_init
#undef pthread_attr_setstacksize
#undef pthread_attr_destroy
#undef pthread_create
#undef pthread_mutex_init
#undef pthread_cond_init

static void set_value(void *context)
{
    int *value;

    value = (int *)context;
    *value = 1;
}

int main(void)
{
    CuArena a;
    CuThread thread_sentinel;
    CuMutex mutex_sentinel;
    CuCond cond_sentinel;
    CuThread *t;
    CuMutex *m;
    CuCond *c;
    int value;

    cu_arena_init(&a);
    for (fail_step = 1; fail_step <= 3; fail_step++) {
        t = &thread_sentinel;
        destroys = 0;
        errno = 0;
        assert(!cu_thread_start(&t, &a, (size_t)262144, set_value, NULL));
        assert(t == &thread_sentinel);
        assert(errno == (fail_step == 1 ? ENOMEM : fail_step == 2 ? EINVAL : EAGAIN));
        assert(!attr_live && destroys == (fail_step == 1 ? 0 : 1));
    }
    fail_step = 4;
    m = &mutex_sentinel;
    errno = 0;
    assert(!cu_mutex_init(&m, &a) && errno == EAGAIN && m == &mutex_sentinel);
    fail_step = 5;
    c = &cond_sentinel;
    errno = 0;
    assert(!cu_cond_init(&c, &a) && errno == EAGAIN && c == &cond_sentinel);
    fail_step = 0;
    destroys = 0;
    value = 0;
    assert(cu_thread_start(&t, &a, (size_t)262144, set_value, &value));
    assert(!attr_live && destroys == 1);
    cu_thread_join(t);
    assert(value == 1);
    assert(cu_mutex_init(&m, &a));
    assert(cu_cond_init(&c, &a));
    cu_cond_destroy(c);
    cu_mutex_destroy(m);
    cu_arena_release(&a);
    return 0;
}
#endif
