/* Differential-test driver: minifies stdin with c-minify and writes the
 * result to stdout, so tools/diff_fuzz.py can compare it with the Go
 * oracle byte for byte.
 *
 *   minify_run [-type text/html|application/xml] [-flags N]
 *
 * -flags is a CU_MINIFY_* mask (decimal, octal or hex); the default is
 * CU_MINIFY_SITE. Exit 0 with the bytes on success; 3 with "status N" on
 * stderr for any other status; 2 on a usage or I/O error. Never part of
 * make test. */
#include "cu_minify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(const char *why)
{
    fprintf(stderr, "minify_run: %s\n", why);
    return 2;
}

int main(int argc, char **argv)
{
    CuArena a, scratch;
    CuMinifyOptions opt;
    CuMinifyType type = CU_MINIFY_HTML;
    CuBuilder b;
    CuStr src, out;
    CuMinifyStatus st;
    int i;

    opt.flags = CU_MINIFY_SITE;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            if (!cu_minify_type_for(cu_str_cstr(argv[++i]), &type)) return usage("unknown type");
        } else if (strcmp(argv[i], "-flags") == 0 && i + 1 < argc) {
            opt.flags = strtoul(argv[++i], NULL, 0);
        } else {
            return usage("unknown argument");
        }
    }
    cu_arena_init(&a);
    cu_arena_init(&scratch);
    if (!cu_stream_read(&a, stdin, &src)) return usage("cannot read stdin");
    cu_sb_init(&b, &a);
    st = cu_minify(&scratch, type, &opt, src, &b);
    if (st != CU_MINIFY_OK) {
        fprintf(stderr, "status %d\n", (int)st);
        return 3;
    }
    if (!cu_sb_finish(&b, &out)) return usage("out of memory");
    if (out.len > 0 && fwrite(out.ptr, 1, out.len, stdout) != out.len) return usage("cannot write stdout");
    cu_arena_release(&scratch);
    cu_arena_release(&a);
    return 0;
}
