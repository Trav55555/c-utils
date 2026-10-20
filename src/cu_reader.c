/* Deadline enforcement belongs at the transport boundary, not the parser. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdlib.h>
#include "cu.h"

#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
#define CU_READER_NONE 1
#endif
#ifndef ENOSYS
#define ENOSYS EINVAL
#endif

static void init(CuReader *r)
{
    if (r == NULL)
        abort();
    r->state = CU_INPUT_DATA;
    r->error = 0;
    r->started_sec = 0;
    r->started_nsec = 0;
    r->timeout_ms = 0;
}

static CuInputStatus fail(CuReader *r, int error)
{
    r->state = CU_INPUT_ERROR;
    r->error = error;
    errno = error;
    return CU_INPUT_ERROR;
}

#ifndef CU_READER_NONE
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

/* The maximum duration in seconds is ULONG_MAX/1000, so elapsed-second
 * subtraction modulo unsigned long is sufficient even with wider time_t.
 * Avoid absolute-deadline addition and round the remaining wait up. */
static int budget(const CuReader *r, unsigned long *left)
{
    unsigned long sec;
    unsigned long nsec;
    unsigned long elapsed;
    unsigned long used;

    if (!stamp(&sec, &nsec))
        return 0;
    elapsed = sec - r->started_sec;
    if (nsec < r->started_nsec) {
        elapsed--;
        nsec = 1000000000UL - r->started_nsec + nsec;
    } else {
        nsec -= r->started_nsec;
    }
    if (elapsed > r->timeout_ms / 1000) {
        errno = ETIMEDOUT;
        return 0;
    }
    *left = r->timeout_ms - elapsed * 1000;
    used = nsec / 1000000UL;
    if (used >= *left) {
        errno = ETIMEDOUT;
        return 0;
    }
    *left -= used;
    return 1;
}
#endif

void cu_reader_memory(CuReader *r, CuStr bytes)
{
    if (bytes.ptr == NULL && bytes.len != 0)
        abort();
    init(r);
    r->kind = CU_READER_MEMORY;
    r->source.memory = bytes;
}

int cu_reader_socket(CuReader *r, CuSocket *s, unsigned long timeout_ms)
{
    if (s == NULL || timeout_ms == 0 || s->state == 0 || s->state == -2 ||
        s->listen || s->watched)
        abort();
    init(r);
    r->kind = CU_READER_SOCKET;
    r->source.socket = s;
    r->timeout_ms = timeout_ms;
    if (s->state < 0) {
        (void)fail(r, s->error);
        return 0;
    }
#ifdef CU_READER_NONE
    (void)fail(r, ENOSYS);
    return 0;
#else
    if (!stamp(&r->started_sec, &r->started_nsec)) {
        (void)fail(r, errno);
        return 0;
    }
    return 1;
#endif
}

CuInputStatus cu_reader_read(CuReader *r, void *buf, size_t cap, size_t *n)
{
    size_t got;
#ifndef CU_READER_NONE
    unsigned long left;
#endif

    if (r == NULL || buf == NULL || cap == 0 || n == NULL)
        abort();
    *n = 0;
    if (r->state == CU_INPUT_ERROR) {
        errno = r->error;
        return CU_INPUT_ERROR;
    }
    if (r->state == CU_INPUT_END)
        return CU_INPUT_END;
    if (r->kind == CU_READER_MEMORY) {
        got = r->source.memory.len < cap ? r->source.memory.len : cap;
        if (got == 0) {
            r->state = CU_INPUT_END;
            return CU_INPUT_END;
        }
        memmove(buf, r->source.memory.ptr, got);
        r->source.memory.ptr += got;
        r->source.memory.len -= got;
        *n = got;
        return CU_INPUT_DATA;
    }
    if (r->kind != CU_READER_SOCKET)
        abort();
#ifdef CU_READER_NONE
    return fail(r, ENOSYS);
#else
    if (!budget(r, &left))
        return fail(r, errno);
    if (!cu_socket_read_for(r->source.socket, (char *)buf, cap, &got, left))
        return fail(r, cu_socket_errno(r->source.socket));
    /* A successful system read must not publish data after the shared
     * budget elapsed while it waited, or after a scheduling pause. */
    if (!budget(r, &left))
        return fail(r, errno);
    if (got == 0) {
        r->state = CU_INPUT_END;
        return CU_INPUT_END;
    }
    *n = got;
    return CU_INPUT_DATA;
#endif
}

int cu_reader_errno(const CuReader *r)
{
    if (r == NULL)
        abort();
    return r->error;
}
