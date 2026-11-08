/* Differential-test driver: renders stdin with c-mark and writes the HTML
 * to stdout, so tools/diff_fuzz.py can compare it with the Go oracle byte
 * for byte. Options mirror the oracle's command line, plus the sets its
 * -cases mode takes:
 *
 *   mark_render [-hugo] [-unsafe] [-xhtml] [-hardwraps] [-flags N]
 *               [-align default|attribute|style|none]
 *
 * -flags ORs a CU_MARK_* mask (decimal, octal or hex) into the flags.
 * Exit 0 with the HTML on success; 3 with "status N" on stderr when the
 * render refuses (CU_MARK_LIMIT) or runs out of memory; 2 on a usage or
 * I/O error. Never part of make test. */
#include "cu_mark.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(const char *why)
{
    fprintf(stderr, "mark_render: %s\n", why);
    return 2;
}

int main(int argc, char **argv)
{
    CuArena a, scratch;
    CuMarkOptions opt;
    CuBuilder b;
    CuStr src, out;
    CuMarkStatus st;
    int i;

    opt.flags = 0;
    opt.table_align = CU_MARK_ALIGN_DEFAULT;
    opt.code_hook = NULL;
    opt.code_ctx = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-hugo") == 0) {
            opt.flags |= CU_MARK_HUGO;
        } else if (strcmp(argv[i], "-unsafe") == 0) {
            opt.flags |= CU_MARK_UNSAFE;
        } else if (strcmp(argv[i], "-xhtml") == 0) {
            opt.flags |= CU_MARK_XHTML;
        } else if (strcmp(argv[i], "-hardwraps") == 0) {
            opt.flags |= CU_MARK_HARD_WRAPS;
        } else if (strcmp(argv[i], "-flags") == 0 && i + 1 < argc) {
            char *end;
            unsigned long v = strtoul(argv[++i], &end, 0);
            if (*argv[i] == '\0' || *end != '\0') return usage("-flags wants a number");
            opt.flags |= v;
        } else if (strcmp(argv[i], "-align") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "default") == 0) opt.table_align = CU_MARK_ALIGN_DEFAULT;
            else if (strcmp(v, "attribute") == 0) opt.table_align = CU_MARK_ALIGN_ATTRIBUTE;
            else if (strcmp(v, "style") == 0) opt.table_align = CU_MARK_ALIGN_STYLE;
            else if (strcmp(v, "none") == 0) opt.table_align = CU_MARK_ALIGN_NONE;
            else return usage("-align wants default, attribute, style or none");
        } else {
            return usage("unknown option");
        }
    }
    cu_arena_init(&a);
    cu_arena_init(&scratch);
    if (!cu_stream_read(&a, stdin, &src)) return usage("cannot read stdin");
    cu_sb_init(&b, &a);
    st = cu_mark_render(&scratch, src, &opt, &b);
    if (st != CU_MARK_OK) {
        fprintf(stderr, "status %d\n", (int)st);
        return 3;
    }
    if (!cu_sb_finish(&b, &out)) return usage("out of memory");
    if (fwrite(out.ptr, 1, out.len, stdout) != out.len || fflush(stdout) != 0) return usage("cannot write stdout");
    cu_arena_release(&scratch);
    cu_arena_release(&a);
    return 0;
}
