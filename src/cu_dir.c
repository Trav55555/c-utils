/* dirfd, fstatat and the DT_* kinds need POSIX 2008 and, for DT_*, the
 * default (BSD) names. These defines serve builds where this file comes
 * first; dcheck force-includes a header before it, so dcheck.conf sets
 * _DEFAULT_SOURCE through PROJECT_DEFINES. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

/* Cerberus models no directory kinds or links. */
#if defined(__cerb__) || !(defined(__unix__) || defined(__APPLE__) || defined(__wasi__))
#define CU_DIR_NONE 1
#endif

#ifdef CU_DIR_NONE

int cu_dir_open(CuDir *d, CuArena *a, const char *path)
{
    if (d == NULL || a == NULL || path == NULL)
        abort();
    d->handle = NULL;
    d->arena = a;
    d->state = -1;
#ifdef ENOSYS
    d->error = ENOSYS;
#else
    d->error = 0;
#endif
    return 0;
}

int cu_dir_next(CuDir *d, CuDirEntry *e)
{
    if (d == NULL || e == NULL || d->state == 0)
        abort();
    return -1;
}

void cu_dir_close(CuDir *d)
{
    if (d == NULL)
        abort();
    d->state = 0;
}

#else

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

/* The kind recorded in the entry when the file system keeps one, else the
 * entry's own status (a link is not followed). 0 with errno set when that
 * status cannot be read. */
static int entry_kind(DIR *dir, const struct dirent *ent, CuDirKind *kind)
{
    struct stat st;

#ifdef DT_UNKNOWN
    if (ent->d_type == DT_REG) {
        *kind = CU_DIR_FILE;
        return 1;
    }
    if (ent->d_type == DT_DIR) {
        *kind = CU_DIR_DIR;
        return 1;
    }
    if (ent->d_type == DT_LNK) {
        *kind = CU_DIR_LINK;
        return 1;
    }
    if (ent->d_type != DT_UNKNOWN) {
        *kind = CU_DIR_OTHER;
        return 1;
    }
#endif
    if (fstatat(dirfd(dir), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return 0;
    if (S_ISREG(st.st_mode))
        *kind = CU_DIR_FILE;
    else if (S_ISDIR(st.st_mode))
        *kind = CU_DIR_DIR;
    else if (S_ISLNK(st.st_mode))
        *kind = CU_DIR_LINK;
    else
        *kind = CU_DIR_OTHER;
    return 1;
}

static int fail(CuDir *d, int error)
{
    d->state = -1;
    d->error = error;
    return -1;
}

int cu_dir_open(CuDir *d, CuArena *a, const char *path)
{
    if (d == NULL || a == NULL || path == NULL)
        abort();
    d->arena = a;
    d->error = 0;
    errno = 0;
    d->handle = opendir(path);
    if (d->handle == NULL) {
        d->state = -1;
        d->error = errno;
        return 0;
    }
    d->state = 1;
    return 1;
}

int cu_dir_next(CuDir *d, CuDirEntry *e)
{
    struct dirent *ent;
    CuDirKind kind;
    const char *name;
    size_t len;

    if (d == NULL || e == NULL || d->state == 0)
        abort();
    if (d->state == -1)
        return -1;
    do {
        errno = 0;
        ent = readdir((DIR *)d->handle);
        if (ent == NULL)
            return errno != 0 ? fail(d, errno) : 0;
    } while (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0);
    if (!entry_kind((DIR *)d->handle, ent, &kind))
        return fail(d, errno);
    len = strlen(ent->d_name);
    name = cu_str_cstr_copy(d->arena, cu_str_n(ent->d_name, len));
    if (name == NULL)
        return fail(d, 0);
    e->name = cu_str_n(name, len);
    e->kind = kind;
    return 1;
}

void cu_dir_close(CuDir *d)
{
    if (d == NULL)
        abort();
    if (d->handle != NULL)
        (void)closedir((DIR *)d->handle);
    d->handle = NULL;
    d->state = 0;
}

#endif

int cu_dir_errno(const CuDir *d)
{
    if (d == NULL)
        abort();
    return d->error;
}
