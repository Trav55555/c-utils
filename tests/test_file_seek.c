/* Inject a failed size query or rewind into the real file reader. These
 * stdio failures must not turn a nonempty file into a successful empty
 * result, or silently discard its contents. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

static int fail_tell;
static int fail_rewind;
static int fail_end_seek;
static int fail_close;
static int fail_growth;
static int clobber_close_errno;

static long file_tell(FILE *f)
{
    if (fail_tell) {
        errno = ERANGE;
        return -1;
    }
    return ftell(f);
}

static int file_seek(FILE *f, long offset, int whence)
{
    if (fail_end_seek && whence == SEEK_END) {
        assert(fseek(f, offset, whence) == 0);
        errno = ERANGE;
        return -1;
    }
    if (fail_rewind && whence == SEEK_SET) {
        errno = ERANGE;
        return -1;
    }
    return fseek(f, offset, whence);
}

static int file_close(FILE *f)
{
    int ok;

    ok = fclose(f);
    if (fail_close) {
        errno = ERANGE;
        return EOF;
    }
    if (clobber_close_errno)
        errno = EDOM;
    return ok;
}

static int file_size_mul(size_t a, size_t b, size_t *out)
{
    if (fail_growth)
        return 0;
    return cu_size_mul(a, b, out);
}

CU_MUST_USE int file_read_probe(CuArena *a, const char *path, CuStr *out);
CU_MUST_USE int stream_read_probe(CuArena *a, FILE *f, CuStr *out);

#define ftell file_tell
#define fseek file_seek
#define fclose file_close
#define cu_size_mul file_size_mul
#define cu_file_read file_read_probe
#define cu_stream_read stream_read_probe
#include "../src/cu_file.c"
#undef ftell
#undef fseek
#undef fclose
#undef cu_size_mul
#undef cu_file_read
#undef cu_stream_read

int main(void)
{
    const char *dir;
    char path[512];
    FILE *f;
    CuArena a;
    CuStr s;
    size_t i;

    dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0')
        dir = "/tmp";
    assert(strlen(dir) < sizeof path - 32);
    sprintf(path, "%s/cu-file-seek-test", dir);
    f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite("contents", 1, 8, f) == 8);
    assert(fclose(f) == 0);
    cu_arena_init(&a);
    fail_tell = 1;
    fail_rewind = 0;
    assert(file_read_probe(&a, path, &s));
    assert(cu_str_eq(s, CU_LIT("contents")));
    fail_tell = 0;
    fail_end_seek = 1;
    assert(file_read_probe(&a, path, &s));
    assert(cu_str_eq(s, CU_LIT("contents")));
    fail_end_seek = 0;
    clobber_close_errno = 1;
    fail_rewind = 1;
    s = CU_LIT("unset");
    errno = 0;
    assert(!file_read_probe(&a, path, &s));
    assert(s.len == 0 && s.ptr != NULL && errno == ERANGE);
    assert(!cu_arena_failed(&a));
    fail_rewind = 0;
    clobber_close_errno = 0;
    fail_close = 1;
    assert(!file_read_probe(&a, path, &s));
    assert(s.len == 0 && errno == ERANGE);
    assert(!cu_arena_failed(&a));
    fail_close = 0;
    f = fopen(path, "wb");
    assert(f != NULL);
    for (i = 0; i < 5000; i++)
        assert(fputc('x', f) != EOF);
    assert(fclose(f) == 0);
    f = fopen(path, "rb");
    assert(f != NULL);
    /* Force the checked multiply's failure without allocating a buffer
     * near SIZE_MAX. Actual arithmetic boundaries have their own tests. */
    fail_growth = 1;
    assert(!stream_read_probe(&a, f, &s));
    assert(s.len == 0 && cu_arena_failed(&a));
    assert(fclose(f) == 0);
    cu_arena_release(&a);
    assert(remove(path) == 0);
    return 0;
}
