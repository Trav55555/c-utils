#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

#if defined(__cerb__) || !(defined(__unix__) || defined(__APPLE__) || defined(__wasi__))
int main(void)
{
    CuFileInfo info;

    info.kind = CU_DIR_LINK;
    info.size = cu_u64(1, 9);
    errno = 0;
    assert(cu_path_info("x", CU_PATH_NOFOLLOW, &info) == CU_INFO_ERROR);
#ifdef ENOSYS
    assert(errno == ENOSYS);
#else
    assert(errno == EINVAL);
#endif
    assert(info.kind == CU_DIR_LINK && info.size.hi == 1 && info.size.lo == 9);
    assert(cu_fd_info(-1, &info) == CU_INFO_ERROR);
    assert(info.kind == CU_DIR_LINK && info.size.hi == 1 && info.size.lo == 9);
    return 0;
}
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char root[512];
static const char original[] = {'a', 'b', '\0', 'c', '\xff', '\n'};

static void path_of(char *out, const char *name)
{
    assert(strlen(root) + strlen(name) + 2 < (size_t)600);
    assert(sprintf(out, "%s/%s", root, name) > 0);
}

static void write_file(const char *path, const char *bytes, size_t length)
{
    FILE *file;

    file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(bytes, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static void expect(const char *path, CuPathFollow follow, CuDirKind kind, unsigned long size)
{
    CuFileInfo info;

    info.kind = CU_DIR_OTHER;
    info.size = cu_u64(1, 9);
    assert(cu_path_info(path, follow, &info) == CU_INFO_OK);
    assert(info.kind == kind && info.size.hi == 0 && info.size.lo == size);
}

static void unchanged(const CuFileInfo *info)
{
    assert(info->kind == CU_DIR_LINK && info->size.hi == 123 && info->size.lo == 456);
}

static void test_paths(void)
{
    char file[600];
    char empty[600];
    char directory[600];
    char nested[600];
    char link[600];
    char alias[600];
    char alias_nested[600];
    char dangling[600];
    char loop[600];
    char invalid[600];
    char high[600];
    CuFileInfo info;

    path_of(file, "file"); path_of(empty, "empty"); path_of(directory, "dir");
    path_of(nested, "dir/nested"); path_of(link, "link"); path_of(alias, "alias");
    path_of(alias_nested, "alias/nested"); path_of(dangling, "dangling");
    path_of(loop, "loop"); path_of(invalid, "file/child"); path_of(high, "file-\xc3\xa9");
    write_file(file, original, sizeof original);
    write_file(empty, "", 0);
    write_file(high, "xyz", 3);
    assert(mkdir(directory, 0700) == 0);
    write_file(nested, "xyz", 3);
    assert(symlink("file", link) == 0);
    assert(symlink("dir", alias) == 0);
    assert(symlink("missing-target", dangling) == 0);
    assert(symlink("loop", loop) == 0);
    expect(file, CU_PATH_NOFOLLOW, CU_DIR_FILE, 6);
    expect(file, CU_PATH_FOLLOW, CU_DIR_FILE, 6);
    expect(empty, CU_PATH_FOLLOW, CU_DIR_FILE, 0);
    expect(high, CU_PATH_NOFOLLOW, CU_DIR_FILE, 3);
    expect(directory, CU_PATH_NOFOLLOW, CU_DIR_DIR, 0);
    expect(link, CU_PATH_NOFOLLOW, CU_DIR_LINK, 0);
    expect(link, CU_PATH_FOLLOW, CU_DIR_FILE, 6);
    expect(alias, CU_PATH_NOFOLLOW, CU_DIR_LINK, 0);
    expect(alias, CU_PATH_FOLLOW, CU_DIR_DIR, 0);
    expect(alias_nested, CU_PATH_NOFOLLOW, CU_DIR_FILE, 3);
    path_of(invalid, "alias/");
    expect(invalid, CU_PATH_NOFOLLOW, CU_DIR_DIR, 0);
    path_of(invalid, "file/child");
    expect(dangling, CU_PATH_NOFOLLOW, CU_DIR_LINK, 0);
    expect(loop, CU_PATH_NOFOLLOW, CU_DIR_LINK, 0);
    info.kind = CU_DIR_LINK; info.size = cu_u64(123, 456);
    errno = 0;
    assert(cu_path_info(dangling, CU_PATH_FOLLOW, &info) == CU_INFO_MISSING);
    assert(errno == ENOENT); unchanged(&info);
    assert(cu_path_info(invalid, CU_PATH_FOLLOW, &info) == CU_INFO_ERROR);
    assert(errno == ENOTDIR); unchanged(&info);
    assert(cu_path_info(loop, CU_PATH_FOLLOW, &info) == CU_INFO_ERROR);
    assert(errno == ELOOP); unchanged(&info);
    path_of(invalid, "absent/child");
    assert(cu_path_info(invalid, CU_PATH_NOFOLLOW, &info) == CU_INFO_MISSING);
    assert(errno == ENOENT); unchanged(&info);
    assert(cu_path_info("", CU_PATH_NOFOLLOW, &info) == CU_INFO_MISSING);
    assert(errno == ENOENT); unchanged(&info);
    assert(unlink(high) == 0 && unlink(loop) == 0 && unlink(dangling) == 0);
    assert(unlink(alias) == 0 && unlink(link) == 0 && unlink(nested) == 0);
    assert(rmdir(directory) == 0 && unlink(empty) == 0 && unlink(file) == 0);
}

static void test_open_object(void)
{
    char file[600];
    char replacement[600];
    CuFileInfo info;
    int fd;
    int flags;
    char byte;

    path_of(file, "opened"); path_of(replacement, "replacement");
    write_file(file, original, sizeof original);
    write_file(replacement, "replacement-body", 16);
    fd = open(file, O_RDONLY | O_NONBLOCK);
    assert(fd >= 0);
    flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && lseek(fd, (off_t)2, SEEK_SET) == (off_t)2);
    assert(rename(replacement, file) == 0);
    assert(cu_fd_info(fd, &info) == CU_INFO_OK);
    assert(info.kind == CU_DIR_FILE && info.size.hi == 0 && info.size.lo == 6);
    assert(fcntl(fd, F_GETFL) == flags);
    assert(read(fd, &byte, 1) == 1 && byte == '\0');
    expect(file, CU_PATH_FOLLOW, CU_DIR_FILE, 16);
    assert(unlink(file) == 0);
    assert(cu_fd_info(fd, &info) == CU_INFO_OK && info.size.lo == 6);
    assert(close(fd) == 0);
    info.kind = CU_DIR_LINK; info.size = cu_u64(123, 456);
    errno = 0;
    assert(cu_fd_info(fd, &info) == CU_INFO_ERROR && errno == EBADF);
    unchanged(&info);
    assert(cu_fd_info(-1, &info) == CU_INFO_ERROR && errno == EBADF);
    unchanged(&info);
}

static void test_size(void)
{
    char file[600];
    CuFileInfo info;
    off_t large;
    unsigned i;
    int fd;

    path_of(file, "large");
    fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(cu_fd_info(fd, &info) == CU_INFO_OK && info.size.hi == 0 && info.size.lo == 0);
    if (sizeof(off_t) >= (size_t)8) {
        large = 1;
        for (i = 0; i < 32; i++) large *= 2;
        large += 123;
        assert(ftruncate(fd, large) == 0);
        assert(cu_fd_info(fd, &info) == CU_INFO_OK && info.kind == CU_DIR_FILE);
        assert(info.size.hi == 1 && info.size.lo == 123);
        assert(cu_path_info(file, CU_PATH_NOFOLLOW, &info) == CU_INFO_OK);
        assert(info.size.hi == 1 && info.size.lo == 123);
    }
    assert(close(fd) == 0 && unlink(file) == 0);
}

#ifndef __wasi__
static void test_other(void)
{
    char fifo[600];
    CuFileInfo info;
    int descriptors[2];

    path_of(fifo, "fifo");
    assert(mkfifo(fifo, 0600) == 0);
    expect(fifo, CU_PATH_NOFOLLOW, CU_DIR_OTHER, 0);
    assert(pipe(descriptors) == 0);
    assert(cu_fd_info(descriptors[0], &info) == CU_INFO_OK);
    assert(info.kind == CU_DIR_OTHER && info.size.hi == 0 && info.size.lo == 0);
    assert(close(descriptors[0]) == 0 && close(descriptors[1]) == 0);
    assert(unlink(fifo) == 0);
}
#endif

int main(void)
{
    const char *tmp;
    unsigned i;

    tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0') tmp = "/tmp";
    assert(strlen(tmp) < sizeof(root) - 40);
    for (i = 0; i < 1000; i++) {
        assert(sprintf(root, "%s/cu-info-test-%u", tmp, i) > 0);
        if (mkdir(root, 0700) == 0) break;
        assert(errno == EEXIST);
    }
    assert(i < 1000);
    test_paths();
    test_open_object();
    test_size();
#ifndef __wasi__
    test_other();
#endif
    assert(rmdir(root) == 0);
    return 0;
}
#endif
