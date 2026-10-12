#include "death.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

static char path[512];
static int reads;
static int closes;
static int injected_read_error;
static int injected_read_errno;
static int injected_close_error;
static int clobber_close_errno;
static int injected_eof;

static size_t input_read(void *p, size_t size, size_t count, FILE *f)
{
    size_t n;

    n = fread(p, size, count, f);
    reads++;
    if (injected_read_error)
        errno = injected_read_errno;
    return n;
}

static int input_error(FILE *f)
{
    return (reads != 0 && injected_read_error) || ferror(f);
}

static int input_eof(FILE *f)
{
    return (reads != 0 && injected_eof) || feof(f);
}

static int input_close(FILE *f)
{
    int result;

    result = fclose(f);
    closes++;
    if (injected_close_error) {
        errno = ERANGE;
        return EOF;
    }
    if (clobber_close_errno)
        errno = ERANGE;
    return result;
}

int input_open_probe(CuInput *in, const char *name);
void input_attach_probe(CuInput *in, FILE *f);
CuInputStatus input_read_probe(CuInput *in, void *buf, size_t cap, size_t *n);
int input_close_probe(CuInput *in);
int input_errno_probe(const CuInput *in);

#define fread input_read
#define ferror input_error
#define feof input_eof
#define fclose input_close
#define cu_input_open input_open_probe
#define cu_input_attach input_attach_probe
#define cu_input_read input_read_probe
#define cu_input_close input_close_probe
#define cu_input_errno input_errno_probe
#include "../src/cu_input.c"
#undef fread
#undef ferror
#undef feof
#undef fclose
#undef cu_input_open
#undef cu_input_attach
#undef cu_input_read
#undef cu_input_close
#undef cu_input_errno

static void reset_injection(void)
{
    reads = 0;
    closes = 0;
    injected_read_error = 0;
    injected_read_errno = 0;
    injected_close_error = 0;
    clobber_close_errno = 0;
    injected_eof = 0;
}

static void write_bytes(const char *data, size_t n)
{
    FILE *f;

    f = fopen(path, "wb");
    assert(f != NULL);
    assert(n == 0 || fwrite(data, 1, n, f) == n);
    assert(fclose(f) == 0);
}

static void test_chunks(void)
{
    static const char data[] = "ab\0cd\nxyz";
    static const size_t capacities[] = {1, 3, 4, 32};
    char actual[sizeof data];
    char *buf;
    CuInput in;
    CuInputStatus status;
    size_t i;
    size_t n;
    size_t total;

    write_bytes(data, sizeof data - 1);
    for (i = 0; i < sizeof capacities / sizeof capacities[0]; i++) {
        buf = (char *)malloc(capacities[i]);
        assert(buf != NULL);
        assert(cu_input_open(&in, path));
        total = 0;
        while ((status = cu_input_read(&in, buf, capacities[i], &n)) == CU_INPUT_DATA) {
            assert(n > 0 && n <= capacities[i]);
            assert(n <= sizeof data - 1 - total);
            memcpy(actual + total, buf, n);
            total += n;
        }
        assert(status == CU_INPUT_END && n == 0);
        assert(total == sizeof data - 1 && memcmp(actual, data, total) == 0);
        n = 99;
        assert(cu_input_read(&in, buf, capacities[i], &n) == CU_INPUT_END && n == 0);
        assert(cu_input_close(&in));
        assert(cu_input_close(&in));
        free(buf);
    }
    write_bytes("", 0);
    assert(cu_input_open(&in, path));
    n = 99;
    assert(cu_input_read(&in, actual, sizeof actual, &n) == CU_INPUT_END && n == 0);
    assert(cu_input_close(&in));
}

static void test_borrowed(void)
{
    FILE *f;
    CuInput in;
    char buf[8];
    size_t n;

    write_bytes("before after", 12);
    reset_injection();
    f = fopen(path, "rb");
    assert(f != NULL);
    input_attach_probe(&in, f);
    assert(input_close_probe(&in));
    assert(closes == 0);
    assert(fclose(f) == 0);
    f = fopen(path, "rb");
    assert(f != NULL && fseek(f, 7, SEEK_SET) == 0);
    cu_input_attach(&in, f);
    assert(cu_input_read(&in, buf, sizeof buf, &n) == CU_INPUT_DATA);
    assert(n == 5 && memcmp(buf, "after", 5) == 0);
    assert(cu_input_read(&in, buf, sizeof buf, &n) == CU_INPUT_END && n == 0);
    assert(cu_input_close(&in));
    assert(feof(f) && !ferror(f));
    assert(fseek(f, 0, SEEK_SET) == 0 && fgetc(f) == 'b');
    assert(fclose(f) == 0);

    f = fopen(path, "wb");
    assert(f != NULL);
    cu_input_attach(&in, f);
    assert(cu_input_read(&in, buf, sizeof buf, &n) == CU_INPUT_ERROR && n == 0);
    assert(!cu_input_close(&in));
    assert(ferror(f));
    errno = EDOM;
    cu_input_attach(&in, f);
    assert(cu_input_read(&in, buf, sizeof buf, &n) == CU_INPUT_ERROR && n == 0);
    assert(cu_input_errno(&in) == EDOM && errno == EDOM);
    assert(!cu_input_close(&in) && ferror(f));
    assert(fclose(f) == 0);
}

static void test_failed_open(void)
{
    CuInput in;
    char buf[1];
    size_t n;

    assert(remove(path) == 0);
    errno = 0;
    assert(!cu_input_open(&in, path));
    assert(cu_input_errno(&in) == ENOENT);
    n = 99;
    assert(cu_input_read(&in, buf, sizeof buf, &n) == CU_INPUT_ERROR && n == 0);
    assert(errno == ENOENT);
    assert(!cu_input_close(&in));
    assert(!cu_input_close(&in));
}

static void test_terminal_cache(void)
{
    CuInput in;
    char buf[8];
    size_t n;
    int calls;

    reset_injection();
    write_bytes("abc", 3);
    assert(input_open_probe(&in, path));
    assert(input_read_probe(&in, buf, sizeof buf, &n) == CU_INPUT_DATA && n == 3);
    calls = reads;
    assert(input_read_probe(&in, buf, sizeof buf, &n) == CU_INPUT_END && n == 0);
    assert(reads == calls);
    assert(input_close_probe(&in));
    assert(input_close_probe(&in) && closes == 1);

    reset_injection();
    injected_eof = 1;
    assert(input_open_probe(&in, path));
    assert(input_read_probe(&in, buf, 3, &n) == CU_INPUT_DATA && n == 3);
    calls = reads;
    assert(input_read_probe(&in, buf, 3, &n) == CU_INPUT_END && n == 0);
    assert(reads == calls);
    assert(input_close_probe(&in));
    reset_injection();
}

static void test_errors(void)
{
    static const size_t capacities[] = {1, 8};
    CuInput in;
    char buf[8];
    size_t n;
    size_t i;
    int calls;
    int zero_errno;

    write_bytes("abc", 3);
    for (zero_errno = 0; zero_errno <= 1; zero_errno++) {
        for (i = 0; i < sizeof capacities / sizeof capacities[0]; i++) {
            reset_injection();
            injected_read_error = 1;
            injected_read_errno = zero_errno ? 0 : EDOM;
            injected_eof = 1;
            assert(input_open_probe(&in, path));
            n = 99;
            assert(input_read_probe(&in, buf, capacities[i], &n) == CU_INPUT_ERROR && n == 0);
            assert(input_errno_probe(&in) == injected_read_errno);
            calls = reads;
            assert(input_read_probe(&in, buf, capacities[i], &n) == CU_INPUT_ERROR && n == 0);
            assert(reads == calls);
            clobber_close_errno = 1;
            assert(!input_close_probe(&in));
            assert(input_errno_probe(&in) == injected_read_errno && errno == injected_read_errno);
            assert(!input_close_probe(&in) && closes == 1);
        }
    }
    reset_injection();
    injected_read_error = 1;
    injected_read_errno = EDOM;
    injected_close_error = 1;
    assert(input_open_probe(&in, path));
    assert(input_read_probe(&in, buf, sizeof buf, &n) == CU_INPUT_ERROR);
    assert(!input_close_probe(&in));
    assert(input_errno_probe(&in) == EDOM && errno == EDOM);

    reset_injection();
    injected_read_error = 1;
    injected_read_errno = 0;
    injected_close_error = 1;
    assert(input_open_probe(&in, path));
    assert(input_read_probe(&in, buf, sizeof buf, &n) == CU_INPUT_ERROR);
    assert(!input_close_probe(&in));
    assert(input_errno_probe(&in) == 0 && errno == 0);

    reset_injection();
    injected_close_error = 1;
    assert(input_open_probe(&in, path));
    assert(!input_close_probe(&in));
    assert(input_errno_probe(&in) == ERANGE && errno == ERANGE);
    assert(!input_close_probe(&in) && closes == 1);
    reset_injection();
}

#ifdef CU_DEATH_TESTS
static void open_null_input(void)
{
    int ok;

    ok = cu_input_open(NULL, path);
    (void)ok;
}

static void open_null_path(void)
{
    CuInput in;
    int ok;

    ok = cu_input_open(&in, NULL);
    (void)ok;
}

static void attach_null_input(void)
{
    cu_input_attach(NULL, stdin);
}

static void attach_null_stream(void)
{
    CuInput in;

    cu_input_attach(&in, NULL);
}

static void close_null_input(void)
{
    int ok;

    ok = cu_input_close(NULL);
    (void)ok;
}

static void errno_null_input(void)
{
    int error;

    error = cu_input_errno(NULL);
    (void)error;
}

static void read_null_input(void)
{
    char buf[1];
    size_t n;
    CuInputStatus status;

    status = cu_input_read(NULL, buf, sizeof buf, &n);
    (void)status;
}

static void read_closed(void)
{
    CuInput in;
    CuInputStatus status;
    char buf[1];
    size_t n;

    assert(cu_input_open(&in, path));
    assert(cu_input_close(&in));
    status = cu_input_read(&in, buf, sizeof buf, &n);
    (void)status;
}

static void read_zero(void)
{
    CuInput in;
    CuInputStatus status;
    char buf[1];
    size_t n;

    assert(cu_input_open(&in, path));
    status = cu_input_read(&in, buf, 0, &n);
    (void)status;
}

static void read_null_buffer(void)
{
    CuInput in;
    CuInputStatus status;
    size_t n;

    assert(cu_input_open(&in, path));
    status = cu_input_read(&in, NULL, 1, &n);
    (void)status;
}

static void read_null_count(void)
{
    CuInput in;
    CuInputStatus status;
    char buf[1];

    assert(cu_input_open(&in, path));
    status = cu_input_read(&in, buf, sizeof buf, NULL);
    (void)status;
}
#endif

int main(void)
{
    const char *dir;

    dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0')
        dir = "/tmp";
    assert(strlen(dir) < sizeof path - 32);
    sprintf(path, "%s/cu-input-test", dir);
    test_chunks();
    test_borrowed();
    test_failed_open();
    test_terminal_cache();
    test_errors();
#ifdef CU_DEATH_TESTS
    expect_abort(open_null_input);
    expect_abort(open_null_path);
    expect_abort(attach_null_input);
    expect_abort(attach_null_stream);
    expect_abort(close_null_input);
    expect_abort(errno_null_input);
    expect_abort(read_null_input);
    expect_abort(read_closed);
    expect_abort(read_zero);
    expect_abort(read_null_buffer);
    expect_abort(read_null_count);
#endif
    assert(remove(path) == 0);
    return 0;
}
