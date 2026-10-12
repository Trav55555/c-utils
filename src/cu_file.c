#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "cu.h"

/* Reads f to its end into a buffer of cap bytes, growing it by doubling
 * when it fills; each growth abandons the old buffer in the arena, so cap
 * should be a good guess. 0 on a read error or when memory runs out. */
static int read_all(CuArena *a, FILE *f, size_t cap, CuStr *out)
{
    char *buf;
    char *bigger;
    size_t len;
    size_t got;

    buf = cu_arena_push_bytes(a, cap);
    if (buf == NULL)
        return 0;
    len = 0;
    for (;;) {
        got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (len < cap) {
            if (ferror(f))
                return 0;
            break;
        }
        if (!cu_size_mul(cap, 2, &cap)) {
            a->failed = 1;
            return 0;
        }
        bigger = cu_arena_push_bytes(a, cap);
        if (bigger == NULL)
            return 0;
        memcpy(bigger, buf, len);
        buf = bigger;
    }
    *out = cu_str_n(buf, len);
    return 1;
}

int cu_file_read(CuArena *a, const char *path, CuStr *out)
{
    FILE *f;
    long size;
    size_t cap;
    int ok;
    int c;
    int saved_errno;

    if (a == NULL || path == NULL || out == NULL)
        abort();
    *out = cu_str_empty();
    f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    /* The size, when the stream can say it, is the buffer's size: one more
     * byte lets the read see the end without growing. */
    cap = 4096;
    ok = 1;
    if (fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
        /* Even when ftell fails, the seek moved us to the end. A failed
         * rewind is an I/O failure, not a successful empty read. */
        if (fseek(f, 0, SEEK_SET) != 0)
            ok = 0;
        else if (size >= 0 && (unsigned long)(size_t)size == (unsigned long)size) {
            if (!cu_size_add((size_t)size, 1, &cap))
                cap = 4096;
        }
    } else {
        /* A failed size seek may have moved the stream. Rewind when
         * possible; on a fresh nonseekable stream it has consumed no data. */
        rewind(f);
    }
    /* Read one byte before trusting that size: a directory opens on some
     * systems, reports a huge size on others (ext4), and fails to read. */
    if (ok) {
        c = fgetc(f);
        if (c == EOF)
            ok = !ferror(f);
        else
            ok = ungetc(c, f) != EOF && read_all(a, f, cap, out);
    }
    saved_errno = errno;
    if (fclose(f) != 0) {
        if (ok)
            saved_errno = errno;
        ok = 0;
    }
    if (!ok) {
        errno = saved_errno;
        *out = cu_str_empty();
    }
    return ok;
}

int cu_stream_read(CuArena *a, FILE *f, CuStr *out)
{
    if (a == NULL || f == NULL || out == NULL)
        abort();
    *out = cu_str_empty();
    if (!read_all(a, f, 4096, out)) {
        *out = cu_str_empty();
        return 0;
    }
    return 1;
}
