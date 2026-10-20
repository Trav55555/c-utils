#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

#if defined(__cerb__) || defined(__wasi__) || !(defined(__unix__) || defined(__APPLE__))
int main(void)
{
    CuArena a;
    CuProcess *p;
    CuProcess *sentinel;
    CuProcessStdio io;
    char name[] = "program";
    char *args[2];

    args[0] = name;
    args[1] = NULL;
    io.input = CU_PROCESS_INHERIT;
    io.output = CU_PROCESS_INHERIT;
    io.error = CU_PROCESS_INHERIT;
    cu_arena_init(&a);
    /* An opaque-type cast trips Cerberus' lexer; the void * conversion
     * preserves the same non-NULL output sentinel. */
    sentinel = (void *)&a;
    p = sentinel;
    errno = 0;
    assert(!cu_process_start_redirected(&p, &a, name, args, NULL, &io));
    assert(p == sentinel);
#ifdef ENOSYS
    assert(errno == ENOSYS);
#else
    assert(errno == EINVAL);
#endif
    p = sentinel;
    errno = 0;
    assert(!cu_process_start(&p, &a, name, args, NULL));
    assert(p == sentinel);
#ifdef ENOSYS
    assert(errno == ENOSYS);
#else
    assert(errno == EINVAL);
#endif
    cu_arena_release(&a);
    return 0;
}
#else
#include "death.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char helper[] = "--stdio-child";
/* These are input and comparison fixtures, not the helper's output buffers. */
static const unsigned char input_bytes[] = { 0x41, 0, 0xe3, 0x19, 0x0a, 0x7f, 0x42 };
static const unsigned char stdout_bytes[] = { 'O', 0, 'u', 't', 0xfe, '\n' };
static const unsigned char stderr_bytes[] = { 0x81, 'E', 'r', 0, '!', '\n', 'Z' };
static const unsigned char merged_bytes[] = {
    'O', 0, 'u', 't', 0xfe, '\n', 0x81, 'E', 'r', 0, '!', '\n', 'Z'
};

static void put(int fd, const void *data, size_t size)
{
    const unsigned char *p;
    ssize_t n;

    p = (const unsigned char *)data;
    while (size != 0) {
        n = write(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        assert(n > 0);
        p += (size_t)n;
        size -= (size_t)n;
    }
}

static void exact(int fd, const unsigned char *bytes, size_t size)
{
    unsigned char buffer[64];
    size_t used;
    ssize_t n;

    assert(size < sizeof(buffer));
    assert(lseek(fd, (off_t)0, SEEK_SET) == (off_t)0);
    used = 0;
    for (;;) {
        assert(used < sizeof(buffer));
        n = read(fd, buffer + used, sizeof(buffer) - used);
        if (n < 0 && errno == EINTR)
            continue;
        assert(n >= 0);
        if (n == 0)
            break;
        used += (size_t)n;
    }
    assert(used == size);
    assert(memcmp(buffer, bytes, size) == 0);
}

static int child(const char *mode)
{
    unsigned char bytes[8];
    size_t used;
    ssize_t n;
    int i;

    (void)alarm(8);
    if (strcmp(mode, "zero") == 0)
        return 23;
    if (strcmp(mode, "closed") == 0) {
        for (i = 0; i < 3; ++i) {
            errno = 0;
            if (fcntl(i, F_GETFD) != -1 || errno != EBADF)
                return 71;
        }
        return 23;
    }
    used = 0;
    for (;;) {
        if (used == sizeof(bytes))
            return 72;
        n = read(0, bytes + used, sizeof(bytes) - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return 73;
        if (n == 0)
            break;
        used += (size_t)n;
    }
    if (used != 7 || bytes[0] != 65 || bytes[1] != 0 || bytes[2] != 227 ||
        bytes[3] != 25 || bytes[4] != 10 || bytes[5] != 127 || bytes[6] != 66)
        return 74;
    /* Independent literal oracle; do not derive these from comparison arrays. */
    put(1, "O\000ut\376\n", 6);
    put(2, "\201Er\000!\nZ", 7);
    return 23;
}

static void done(CuProcess *p)
{
    CuProcessExit status;

    status.kind = CU_PROCESS_SIGNALED;
    status.code = -999;
    assert(cu_process_wait(p, 5000, &status) == CU_PROCESS_DONE);
    assert(status.kind == CU_PROCESS_EXITED && status.code == 23);
}

static CuProcess *start(CuArena *a, const char *path, CuProcessStdio *io, char *mode)
{
    CuProcess *p;
    char *args[4];

    args[0] = (char *)path;
    args[1] = helper;
    args[2] = mode;
    args[3] = NULL;
    p = NULL;
    assert(cu_process_start_redirected(&p, a, path, args, NULL, io));
    assert(p != NULL);
    return p;
}

static void seed(int fd)
{
    put(fd, input_bytes, sizeof(input_bytes));
    assert(lseek(fd, (off_t)0, SEEK_SET) == (off_t)0);
}

/* fstat outputs start with values that cannot accidentally meet the oracle
 * under MSan's uninstrumented-libc model. Offsets intentionally are not saved. */
typedef struct Snapshot {
    struct stat object;
    int descriptor_flags;
    int status_flags;
} Snapshot;

static void snapshot(int fd, Snapshot *s, int fill)
{
    memset(&s->object, fill, sizeof(s->object));
    assert(fstat(fd, &s->object) == 0);
    s->descriptor_flags = fcntl(fd, F_GETFD);
    s->status_flags = fcntl(fd, F_GETFL);
    assert(s->descriptor_flags >= 0 && s->status_flags >= 0);
}

static void unchanged(int fd, const Snapshot *before)
{
    Snapshot after;

    snapshot(fd, &after, 0x5a);
    assert(after.object.st_dev == before->object.st_dev);
    assert(after.object.st_ino == before->object.st_ino);
    assert(after.object.st_mode == before->object.st_mode);
    assert(after.descriptor_flags == before->descriptor_flags);
    assert(after.status_flags == before->status_flags);
}

static void regular(const char *path, int merged, int close_sources)
{
    FILE *files[3];
    int fd[3];
    int observer[3];
    Snapshot before[3];
    CuArena a;
    CuProcess *p;
    CuProcessStdio io;
    int i;

    for (i = 0; i < 3; ++i) {
        files[i] = tmpfile();
        assert(files[i] != NULL);
        fd[i] = fileno(files[i]);
        assert(fd[i] >= 3);
        assert(fcntl(fd[i], F_SETFD, FD_CLOEXEC) == 0);
        observer[i] = dup(fd[i]);
        assert(observer[i] >= 3);
        assert(fcntl(observer[i], F_SETFD, FD_CLOEXEC) == 0);
        snapshot(fd[i], &before[i], 0xa5);
    }
    seed(fd[0]);
    io.input = fd[0];
    io.output = fd[1];
    io.error = merged ? fd[1] : fd[2];
    cu_arena_init(&a);
    p = start(&a, path, &io, "io");
    for (i = 0; i < 3; ++i) {
        unchanged(fd[i], &before[i]);
        if (close_sources)
            assert(fclose(files[i]) == 0);
    }
    done(p);
    exact(observer[1], merged ? merged_bytes : stdout_bytes,
          merged ? sizeof(merged_bytes) : sizeof(stdout_bytes));
    exact(observer[2], stderr_bytes, merged ? 0 : sizeof(stderr_bytes));
    for (i = 0; i < 3; ++i) {
        if (!close_sources) {
            unchanged(fd[i], &before[i]);
            assert(fclose(files[i]) == 0);
        }
        assert(close(observer[i]) == 0);
    }
    cu_arena_release(&a);
}

/* All parent-global descriptor manipulation is confined to a disposable fork.
 * Cases: 0/1 swap, three-way cycle, explicit identity with CLOEXEC, inherited
 * streams, partial inheritance, initially closed streams, inherited CLOEXEC. */
static void isolated_body(const char *path, int which)
{
    FILE *files[3];
    int fd[3];
    int map[3];
    int i;
    Snapshot before[3];
    CuArena a;
    CuProcess *p;
    CuProcessStdio io;

    (void)alarm(12);
    for (i = 0; i < 3; ++i) {
        files[i] = tmpfile();
        assert(files[i] != NULL);
        fd[i] = fileno(files[i]);
        assert(fd[i] >= 3);
        assert(fcntl(fd[i], F_SETFD, FD_CLOEXEC) == 0);
        map[i] = i;
    }
    if (which == 0) { map[0] = 1; map[1] = 0; }
    if (which == 1) { map[0] = 1; map[1] = 2; map[2] = 0; }
    seed(fd[map[0]]);
    for (i = 0; i < 3; ++i) {
        assert(dup2(fd[i], i) == i);
        if (which == 2 || which == 7)
            assert(fcntl(i, F_SETFD, FD_CLOEXEC) == 0);
        if (which == 5 || which == 6)
            assert(close(i) == 0);
        else
            snapshot(i, &before[i], 0xa5);
    }
    io.input = map[0];
    io.output = map[1];
    io.error = map[2];
    if (which == 3 || which == 6 || which == 7) {
        io.input = CU_PROCESS_INHERIT;
        io.output = CU_PROCESS_INHERIT;
        io.error = CU_PROCESS_INHERIT;
    }
    if (which == 4) {
        io.input = CU_PROCESS_INHERIT;
        io.output = fd[1];
        io.error = CU_PROCESS_INHERIT;
    }
    if (which == 5) {
        io.input = fd[0];
        io.output = fd[1];
        io.error = fd[2];
    }
    cu_arena_init(&a);
    p = start(&a, path, &io, (which == 6 || which == 7) ? "closed" : "io");
    for (i = 0; i < 3; ++i) {
        if (which == 5 || which == 6) {
            errno = 0;
            assert(fcntl(i, F_GETFD) == -1 && errno == EBADF);
        } else {
            unchanged(i, &before[i]);
        }
    }
    done(p);
    if (which != 6 && which != 7) {
        exact(fd[map[1]], stdout_bytes, sizeof(stdout_bytes));
        exact(fd[map[2]], stderr_bytes, sizeof(stderr_bytes));
    }
    cu_arena_release(&a);
    for (i = 0; i < 3; ++i)
        assert(fclose(files[i]) == 0);
}

static void isolated(const char *path, int which)
{
    pid_t pid;
    pid_t got;
    int status;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        isolated_body(path, which);
        _exit(0);
    }
    status = -999;
    do {
        got = waitpid(pid, &status, 0);
    } while (got < 0 && errno == EINTR);
    assert(got == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void errors(const char *path)
{
    CuArena a;
    CuProcess *p;
    CuProcess *sentinel;
    CuProcessStdio io;
    char *args[2];
    int fd;
    int i;
    int j;
    int bad[2];

    fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    assert(close(fd) == 0);
    bad[0] = fd;
    bad[1] = -2;
    args[0] = (char *)path;
    args[1] = NULL;
    cu_arena_init(&a);
    sentinel = (void *)&a;
    for (i = 0; i < 2; ++i) {
        for (j = 0; j < 3; ++j) {
            io.input = CU_PROCESS_INHERIT;
            io.output = CU_PROCESS_INHERIT;
            io.error = CU_PROCESS_INHERIT;
            if (j == 0) io.input = bad[i];
            if (j == 1) io.output = bad[i];
            if (j == 2) io.error = bad[i];
            p = sentinel;
            errno = 0;
            assert(!cu_process_start_redirected(&p, &a, path, args, NULL, &io));
            assert(p == sentinel && errno == EBADF);
        }
    }
    cu_arena_release(&a);
}

/* A temporary copy must not resurrect a later, originally closed FD. */
static void closed_later(const char *path)
{
    FILE *file;
    CuArena a;
    CuProcess *p;
    CuProcessStdio io;
    char *args[4];
    int missing;
    int ok;
    int saved;
    int field;

    file = tmpfile();
    assert(file != NULL);
    missing = dup(fileno(file));
    assert(missing >= 3 && close(missing) == 0);
    args[0] = (char *)path;
    args[1] = helper;
    args[2] = "zero";
    args[3] = NULL;
    cu_arena_init(&a);
    for (field = 1; field <= 2; field++) {
        io.input = fileno(file);
        io.output = field == 1 ? missing : fileno(file);
        io.error = field == 2 ? missing : CU_PROCESS_INHERIT;
        p = NULL;
        ok = cu_process_start_redirected(&p, &a, path, args, NULL, &io);
        saved = errno;
        if (ok)
            done(p);
        assert(!ok && p == NULL && saved == EBADF);
    }
    assert(fclose(file) == 0);
    cu_arena_release(&a);
}

typedef struct RedirectJob {
    const char *path;
    int complete;
} RedirectJob;

static void redirect_job(void *context)
{
    RedirectJob *job;

    job = (RedirectJob *)context;
    regular(job->path, 0, 1);
    job->complete = 1;
}

static void concurrent(const char *path)
{
    CuArena arena;
    RedirectJob *jobs;
    CuThread *threads[3];
    size_t i;

    cu_arena_init(&arena);
    jobs = CU_PUSH(&arena, RedirectJob, 3);
    assert(jobs != NULL);
    for (i = 0; i < 3; i++) {
        jobs[i].path = path;
        jobs[i].complete = 0;
        assert(cu_thread_start(&threads[i], &arena, 0, redirect_job, &jobs[i]));
    }
    for (i = 0; i < 3; i++) {
        cu_thread_join(threads[i]);
        assert(jobs[i].complete);
    }
    cu_arena_release(&arena);
}

#ifdef CU_DEATH_TESTS
static int invalid_case;
static void invalid(void)
{
    int result;
    CuArena a;
    CuProcess *p;
    CuProcessStdio io;
    char name[] = "program";
    char *args[2];

    cu_arena_init(&a);
    p = NULL;
    args[0] = name;
    args[1] = NULL;
    io.input = CU_PROCESS_INHERIT;
    io.output = CU_PROCESS_INHERIT;
    io.error = CU_PROCESS_INHERIT;
    result = cu_process_start_redirected(invalid_case == 0 ? NULL : &p,
        invalid_case == 1 ? NULL : &a, invalid_case == 2 ? NULL : name,
        invalid_case == 3 ? NULL : args, NULL, invalid_case == 4 ? NULL : &io);
    (void)result;
}
#endif

int main(int argc, char **argv)
{
    int i;

    if (argc == 3 && strcmp(argv[1], helper) == 0)
        return child(argv[2]);
    assert(argc > 0 && argv[0] != NULL);
    (void)alarm(60);
    regular(argv[0], 0, 0);
    regular(argv[0], 0, 1);
    regular(argv[0], 1, 1);
    for (i = 0; i < 8; ++i)
        isolated(argv[0], i);
    errors(argv[0]);
    closed_later(argv[0]);
    concurrent(argv[0]);
#ifdef CU_DEATH_TESTS
    for (invalid_case = 0; invalid_case < 5; ++invalid_case)
        expect_abort(invalid);
#endif
    (void)alarm(0);
    return 0;
}
#endif
