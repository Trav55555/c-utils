/* pthread_attr_setstacksize needs POSIX; these serve builds where this
 * file comes first, and dcheck.conf's PROJECT_DEFINES serve dcheck's. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdlib.h>
#include "cu.h"

/* Decided at compile time: Cerberus and wasm32-wasi have no threads. */
#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
#define CU_NO_THREADS 1
#endif

static size_t fail_at = (size_t)-1;
static size_t attempts;

void cu_thread_fail_at(size_t n)
{
    fail_at = n;
    attempts = 0;
}

#ifdef CU_NO_THREADS

struct CuThread {
    int unused;
};
struct CuMutex {
    int unused;
};
struct CuCond {
    int unused;
};

/* No object can exist here, so every other call is misuse. */
static int no_threads(void)
{
#ifdef ENOSYS
    errno = ENOSYS;
#else
    errno = 0;
#endif
    return 0;
}

int cu_thread_start(CuThread **t, CuArena *a, size_t stack_size, void (*fn)(void *), void *context)
{
    (void)stack_size;
    (void)context;
    if (t == NULL || a == NULL || fn == NULL)
        abort();
    return no_threads();
}

void cu_thread_join(CuThread *t)
{
    (void)t;
    abort();
}

int cu_mutex_init(CuMutex **m, CuArena *a)
{
    if (m == NULL || a == NULL)
        abort();
    return no_threads();
}

void cu_mutex_lock(CuMutex *m)
{
    (void)m;
    abort();
}

void cu_mutex_unlock(CuMutex *m)
{
    (void)m;
    abort();
}

void cu_mutex_destroy(CuMutex *m)
{
    (void)m;
    abort();
}

int cu_cond_init(CuCond **c, CuArena *a)
{
    if (c == NULL || a == NULL)
        abort();
    return no_threads();
}

void cu_cond_wait(CuCond *c, CuMutex *m)
{
    (void)c;
    (void)m;
    abort();
}

void cu_cond_broadcast(CuCond *c)
{
    (void)c;
    abort();
}

void cu_cond_destroy(CuCond *c)
{
    (void)c;
    abort();
}

#else

#include <pthread.h>

/* Disabled injection only reads shared state. Enabled injection requires
 * start and init attempts from one thread, as the public hook specifies. */
static int injected_failure(void)
{
    return fail_at != (size_t)-1 && attempts++ == fail_at;
}

struct CuThread {
    pthread_t id;
    void (*fn)(void *);
    void *context;
    int joined;
};

struct CuMutex {
    pthread_mutex_t m;
    int live;
};

struct CuCond {
    pthread_cond_t c;
    int live;
};

static void *trampoline(void *record)
{
    CuThread *t;

    t = (CuThread *)record;
    t->fn(t->context);
    cu_scratch_release();
    return NULL;
}

int cu_thread_start(CuThread **t, CuArena *a, size_t stack_size, void (*fn)(void *), void *context)
{
    pthread_attr_t attr;
    CuThread *record;
    int code;

    if (t == NULL || a == NULL || fn == NULL)
        abort();
    if (injected_failure()) {
        errno = EAGAIN;
        return 0;
    }
    record = CU_PUSH(a, CuThread, 1);
    if (record == NULL) {
        errno = ENOMEM;
        return 0;
    }
    record->fn = fn;
    record->context = context;
    record->joined = 0;
    code = pthread_attr_init(&attr);
    if (code != 0) {
        errno = code;
        return 0;
    }
    if (stack_size != 0)
        code = pthread_attr_setstacksize(&attr, stack_size);
    if (code == 0)
        code = pthread_create(&record->id, &attr, trampoline, record);
    (void)pthread_attr_destroy(&attr);
    if (code != 0) {
        errno = code;
        return 0;
    }
    *t = record;
    return 1;
}

void cu_thread_join(CuThread *t)
{
    if (t == NULL || t->joined)
        abort();
    if (pthread_join(t->id, NULL) != 0)
        abort();
    t->joined = 1;
}

int cu_mutex_init(CuMutex **m, CuArena *a)
{
    CuMutex *mutex;
    int code;

    if (m == NULL || a == NULL)
        abort();
    if (injected_failure()) {
        errno = EAGAIN;
        return 0;
    }
    mutex = CU_PUSH(a, CuMutex, 1);
    if (mutex == NULL) {
        errno = ENOMEM;
        return 0;
    }
    code = pthread_mutex_init(&mutex->m, NULL);
    if (code != 0) {
        errno = code;
        return 0;
    }
    mutex->live = 1;
    *m = mutex;
    return 1;
}

static void check_mutex(const CuMutex *m)
{
    if (m == NULL || !m->live)
        abort();
}

void cu_mutex_lock(CuMutex *m)
{
    check_mutex(m);
    if (pthread_mutex_lock(&m->m) != 0)
        abort();
}

void cu_mutex_unlock(CuMutex *m)
{
    check_mutex(m);
    if (pthread_mutex_unlock(&m->m) != 0)
        abort();
}

void cu_mutex_destroy(CuMutex *m)
{
    check_mutex(m);
    if (pthread_mutex_destroy(&m->m) != 0)
        abort();
    m->live = 0;
}

int cu_cond_init(CuCond **c, CuArena *a)
{
    CuCond *cond;
    int code;

    if (c == NULL || a == NULL)
        abort();
    if (injected_failure()) {
        errno = EAGAIN;
        return 0;
    }
    cond = CU_PUSH(a, CuCond, 1);
    if (cond == NULL) {
        errno = ENOMEM;
        return 0;
    }
    code = pthread_cond_init(&cond->c, NULL);
    if (code != 0) {
        errno = code;
        return 0;
    }
    cond->live = 1;
    *c = cond;
    return 1;
}

static void check_cond(const CuCond *c)
{
    if (c == NULL || !c->live)
        abort();
}

void cu_cond_wait(CuCond *c, CuMutex *m)
{
    check_cond(c);
    check_mutex(m);
    if (pthread_cond_wait(&c->c, &m->m) != 0)
        abort();
}

void cu_cond_broadcast(CuCond *c)
{
    check_cond(c);
    if (pthread_cond_broadcast(&c->c) != 0)
        abort();
}

void cu_cond_destroy(CuCond *c)
{
    check_cond(c);
    if (pthread_cond_destroy(&c->c) != 0)
        abort();
    c->live = 0;
}

#endif
