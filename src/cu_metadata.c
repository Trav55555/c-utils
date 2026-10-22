/* Expose lstat in strict C89 builds without changing the native file ABI. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdlib.h>
#include "cu.h"

#if defined(__cerb__) || !(defined(__unix__) || defined(__APPLE__) || defined(__wasi__))
#define CU_METADATA_NONE 1
#endif

#ifdef CU_METADATA_NONE

#ifndef ENOSYS
#define ENOSYS EINVAL
#endif

CuInfoResult cu_path_info(const char *path, CuPathFollow follow, CuFileInfo *out)
{
    if (path == NULL || out == NULL ||
        (follow != CU_PATH_NOFOLLOW && follow != CU_PATH_FOLLOW))
        abort();
    errno = ENOSYS;
    return CU_INFO_ERROR;
}

CuInfoResult cu_fd_info(int fd, CuFileInfo *out)
{
    (void)fd;
    if (out == NULL)
        abort();
    errno = ENOSYS;
    return CU_INFO_ERROR;
}

CuInfoResult cu_dir_info(const CuDir *d, const char *name, CuPathFollow follow,
                         CuFileInfo *out)
{
    if (d == NULL || name == NULL || out == NULL || name[0] == '\0' || name[0] == '/' ||
        (follow != CU_PATH_NOFOLLOW && follow != CU_PATH_FOLLOW))
        abort();
    errno = ENOSYS;
    return CU_INFO_ERROR;
}

#else

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

/* Extract exactly eight base-256 digits before testing for a remainder.
 * Only a single digit is narrowed; off_t need not fit unsigned long or
 * size_t. Each unsigned-long shift is by 0, 8, 16 or 24 bits. */
static int offset_size(off_t value, CuU64 *out)
{
    CuU64 size;
    unsigned long byte;
    unsigned i;

    if (value < 0)
        return 0;
    size.hi = 0;
    size.lo = 0;
    for (i = 0; i < 8; i++) {
        byte = (unsigned long)(value % 256);
        value /= 256;
        if (i < 4)
            size.lo |= byte << (i * 8);
        else
            size.hi |= byte << ((i - 4) * 8);
    }
    if (value != 0)
        return 0;
    *out = size;
    return 1;
}

/* Publish only after classification and regular-file size validation.
 * Nonregular sizes are irrelevant and need not be inspected. */
static CuInfoResult stat_info(const struct stat *st, CuFileInfo *out)
{
    CuFileInfo info;

    info.size.hi = 0;
    info.size.lo = 0;
    if (S_ISREG(st->st_mode)) {
        info.kind = CU_DIR_FILE;
        if (!offset_size(st->st_size, &info.size)) {
#ifdef EOVERFLOW
            errno = EOVERFLOW;
#else
            errno = ERANGE;
#endif
            return CU_INFO_ERROR;
        }
    } else if (S_ISDIR(st->st_mode))
        info.kind = CU_DIR_DIR;
    else if (S_ISLNK(st->st_mode))
        info.kind = CU_DIR_LINK;
    else
        info.kind = CU_DIR_OTHER;
    *out = info;
    return CU_INFO_OK;
}

CuInfoResult cu_path_info(const char *path, CuPathFollow follow, CuFileInfo *out)
{
    struct stat st;
    int result;

    if (path == NULL || out == NULL ||
        (follow != CU_PATH_NOFOLLOW && follow != CU_PATH_FOLLOW))
        abort();
    if (follow == CU_PATH_FOLLOW)
        result = stat(path, &st);
    else
        result = lstat(path, &st);
    if (result != 0)
        return errno == ENOENT ? CU_INFO_MISSING : CU_INFO_ERROR;
    return stat_info(&st, out);
}

CuInfoResult cu_fd_info(int fd, CuFileInfo *out)
{
    struct stat st;

    if (out == NULL)
        abort();
    if (fstat(fd, &st) != 0)
        return CU_INFO_ERROR;
    return stat_info(&st, out);
}

CuInfoResult cu_dir_info(const CuDir *d, const char *name, CuPathFollow follow,
                         CuFileInfo *out)
{
    struct stat st;
    int fd;

    if (d == NULL || name == NULL || out == NULL || name[0] == '\0' || name[0] == '/' ||
        (follow != CU_PATH_NOFOLLOW && follow != CU_PATH_FOLLOW))
        abort();
    if (d->state != 1 || d->handle == NULL) {
        errno = EBADF;
        return CU_INFO_ERROR;
    }
    fd = dirfd((DIR *)d->handle);
    if (fd < 0)
        return CU_INFO_ERROR;
    if (fstatat(fd, name, &st, follow == CU_PATH_FOLLOW ? 0 : AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? CU_INFO_MISSING : CU_INFO_ERROR;
    return stat_info(&st, out);
}

#endif
