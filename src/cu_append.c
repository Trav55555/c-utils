/* fsync, ftruncate and O_DIRECTORY need POSIX 2008, and flock the default
 * (BSD) names. As in cu_dir.c, dcheck force-includes a header first, so
 * dcheck.conf sets _DEFAULT_SOURCE through PROJECT_DEFINES. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "cu.h"

/* Cerberus models no file descriptors. */
#if defined(__cerb__) || !(defined(__unix__) || defined(__APPLE__) || defined(__wasi__))
#define CU_APPEND_NONE 1
#endif

#ifndef ENOSYS
#define ENOSYS EINVAL
#endif

/* Entry checks shared by every operation but close: NULL and use after
 * close (state 0, or -2 after a failure) abort; a failed CuAppend reports
 * its saved error. */
static int usable(CuAppend *f)
{
    if (f == NULL || f->state == 0 || f->state == -2)
        abort();
    if (f->state < 0) {
        errno = f->error;
        return 0;
    }
    return 1;
}

/* Records the current errno as f's sticky error. */
static int fail(CuAppend *f)
{
    f->state = -1;
    f->error = errno;
    return 0;
}

int cu_append_errno(const CuAppend *f)
{
    if (f == NULL)
        abort();
    return f->error;
}

static void check_path(const char *path)
{
    if (path == NULL)
        abort();
}

#ifdef CU_APPEND_NONE

int cu_append_open(CuAppend *f, const char *path, int flags)
{
    if (f == NULL)
        abort();
    check_path(path);
    (void)flags;
    f->fd = -1;
    f->state = -1;
    f->error = ENOSYS;
    errno = ENOSYS;
    return 0;
}

/* Every CuAppend here failed to open, so usable() always refuses it. */
int cu_append_write(CuAppend *f, CuStr bytes)
{
    (void)bytes;
    return usable(f);
}

int cu_append_sync(CuAppend *f)
{
    return usable(f);
}

int cu_append_truncate(CuAppend *f, size_t len)
{
    (void)len;
    return usable(f);
}

int cu_append_lock(CuAppend *f)
{
    return usable(f);
}

int cu_append_close(CuAppend *f)
{
    if (f == NULL)
        abort();
    if (f->state != 0)
        f->state = f->state < 0 ? -2 : 0;
    return 0;
}

static int no_files(const char *path)
{
    check_path(path);
    errno = ENOSYS;
    return 0;
}

int cu_path_rename(const char *from, const char *to)
{
    check_path(from);
    return no_files(to);
}

int cu_path_remove(const char *path)
{
    return no_files(path);
}

int cu_dir_make(const char *path)
{
    return no_files(path);
}

int cu_dir_sync(const char *path)
{
    return no_files(path);
}

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef __wasi__
#include <sys/file.h>
#endif

/* The most one write() call is asked for, so its result fits ssize_t on
 * 32-bit systems. */
#define MAX_CHUNK ((size_t)1 << 30)

int cu_append_open(CuAppend *f, const char *path, int flags)
{
    int oflags;

    if (f == NULL)
        abort();
    check_path(path);
    oflags = O_WRONLY | O_APPEND;
#ifdef O_CLOEXEC
    oflags |= O_CLOEXEC;
#endif
    if (flags & CU_APPEND_CREATE)
        oflags |= O_CREAT;
    if ((flags & CU_APPEND_CREATE) && (flags & CU_APPEND_EXCLUSIVE))
        oflags |= O_EXCL;
    if (flags & CU_APPEND_TRUNCATE)
        oflags |= O_TRUNC;
    f->error = 0;
    f->fd = open(path, oflags, 0666);
    if (f->fd < 0)
        return fail(f);
#ifndef O_CLOEXEC
    if (fcntl(f->fd, F_SETFD, FD_CLOEXEC) != 0) {
        int saved;

        saved = errno;
        (void)close(f->fd);
        f->fd = -1;
        errno = saved;
        return fail(f);
    }
#endif
    f->state = 1;
    return 1;
}

int cu_append_write(CuAppend *f, CuStr bytes)
{
    const char *p;
    size_t left;
    size_t chunk;
    ssize_t got;

    if (!usable(f))
        return 0;
    p = bytes.ptr;
    left = bytes.len;
    while (left > 0) {
        chunk = left < MAX_CHUNK ? left : MAX_CHUNK;
        got = write(f->fd, p, chunk);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return fail(f);
        }
        /* No progress for a nonzero count: the device took nothing. */
        if (got == 0) {
            errno = EIO;
            return fail(f);
        }
        p += got;
        left -= (size_t)got;
    }
    return 1;
}

int cu_append_sync(CuAppend *f)
{
    if (!usable(f))
        return 0;
#ifdef F_FULLFSYNC
    /* Apple's fsync leaves data in the drive's cache. */
    if (fcntl(f->fd, F_FULLFSYNC) != 0)
        return fail(f);
#else
    if (fsync(f->fd) != 0)
        return fail(f);
#endif
    return 1;
}

int cu_append_truncate(CuAppend *f, size_t len)
{
    off_t off;

    if (!usable(f))
        return 0;
    off = (off_t)len;
    if (off < 0 || (size_t)off != len) {
        errno = EFBIG;
        return fail(f);
    }
    if (ftruncate(f->fd, off) != 0)
        return fail(f);
    return 1;
}

int cu_append_lock(CuAppend *f)
{
    if (!usable(f))
        return 0;
#ifdef __wasi__
    errno = ENOSYS;
    return 0;
#else
    /* Not an error of f: errno says why, and f stays usable. */
    return flock(f->fd, LOCK_EX | LOCK_NB) == 0;
#endif
}

/* state -2 marks a CuAppend closed after a failure, so repeats keep
 * returning 0. */
int cu_append_close(CuAppend *f)
{
    if (f == NULL)
        abort();
    if (f->state == 0)
        return 1;
    if (f->state == -2)
        return 0;
    if (f->fd >= 0) {
        if (close(f->fd) != 0 && f->state > 0) {
            f->state = -1;
            f->error = errno;
        }
        f->fd = -1;
    }
    if (f->state < 0) {
        f->state = -2;
        errno = f->error;
        return 0;
    }
    f->state = 0;
    return 1;
}

int cu_path_rename(const char *from, const char *to)
{
    check_path(from);
    check_path(to);
    return rename(from, to) == 0;
}

int cu_path_remove(const char *path)
{
    check_path(path);
    return unlink(path) == 0;
}

int cu_dir_make(const char *path)
{
    check_path(path);
    return mkdir(path, 0777) == 0;
}

int cu_dir_sync(const char *path)
{
    int fd;
    int ok;
    int saved;
    int flags;

    check_path(path);
#ifdef __wasi__
    /* WASI grants no sync right on directory descriptors (EBADF). */
    errno = ENOSYS;
    return 0;
#else
    flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    fd = open(path, flags);
    if (fd < 0)
        return 0;
#ifndef O_CLOEXEC
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        saved = errno;
        (void)close(fd);
        errno = saved;
        return 0;
    }
#endif
    ok = fsync(fd) == 0;
    saved = errno;
    if (close(fd) != 0 && ok) {
        ok = 0;
        saved = errno;
    }
    errno = saved;
    return ok;
#endif
}

#endif
