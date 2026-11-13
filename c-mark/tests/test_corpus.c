/* Runs c-mark over the corpus (tests/corpus.json, written by the oracle from
 * goldmark v1.6.0's own test cases) and requires the oracle's exact bytes.
 *
 *   test_corpus [CORPUS] [--report] [--suite NAME] [--show N]
 *
 * Without --report it exits 1 on the first suite with a failure; with it,
 * it prints every suite's count and exits 0, for progress while porting.
 * --suite limits the run to one suite, --show prints up to N failures. */
#include "cu_mark.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SUITES 32

typedef struct Suite {
    CuStr name;
    unsigned long pass;
    unsigned long fail;
} Suite;

static int flag(const CuJson *options, const char *name)
{
    const CuJson *v = cu_json_find(options, cu_str_cstr(name));
    return v != NULL && v->kind == CU_JSON_BOOL && v->truth;
}

static void options_of(const CuJson *o, CuMarkOptions *opt)
{
    static const struct {
        const char *name;
        unsigned long bit;
    } names[] = {
        {"table", CU_MARK_TABLE},
        {"strikethrough", CU_MARK_STRIKETHROUGH},
        {"linkify", CU_MARK_LINKIFY},
        {"taskList", CU_MARK_TASKLIST},
        {"typographer", CU_MARK_TYPOGRAPHER},
        {"definitionList", CU_MARK_DEFINITION_LIST},
        {"footnote", CU_MARK_FOOTNOTE},
        {"autoHeadingID", CU_MARK_AUTO_HEADING_ID},
        {"attribute", CU_MARK_ATTRIBUTE},
        {"hugoIDs", CU_MARK_HUGO_IDS},
        {"unsafe", CU_MARK_UNSAFE},
        {"xhtml", CU_MARK_XHTML},
        {"hardWraps", CU_MARK_HARD_WRAPS},
    };
    const CuJson *align;
    size_t i;
    opt->flags = 0;
    opt->table_align = CU_MARK_ALIGN_DEFAULT;
    opt->code_hook = NULL;
    opt->code_ctx = NULL;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (flag(o, names[i].name)) opt->flags |= names[i].bit;
    }
    align = cu_json_find(o, cu_str_cstr("tableAlign"));
    if (align != NULL && align->kind == CU_JSON_STRING) {
        if (cu_str_eq(align->text, cu_str_cstr("attribute"))) opt->table_align = CU_MARK_ALIGN_ATTRIBUTE;
        else if (cu_str_eq(align->text, cu_str_cstr("style"))) opt->table_align = CU_MARK_ALIGN_STYLE;
        else if (cu_str_eq(align->text, cu_str_cstr("none"))) opt->table_align = CU_MARK_ALIGN_NONE;
    }
}

static CuStr field(const CuJson *c, const char *name)
{
    const CuJson *v = cu_json_find(c, cu_str_cstr(name));
    if (v == NULL || v->kind != CU_JSON_STRING) {
        fprintf(stderr, "corpus case without string field %s\n", name);
        exit(2);
    }
    return v->text;
}

static void show(const CuJson *c, CuStr got)
{
    const CuJson *no = cu_json_find(c, cu_str_cstr("no"));
    CuStr suite = field(c, "suite");
    CuStr md = field(c, "markdown");
    CuStr want = field(c, "html");
    long n = 0;
    if (no == NULL || !cu_json_to_long(no, &n)) n = -1;
    printf("---- %.*s %ld\nmarkdown:\n%.*s\nwant:\n%.*s\ngot:\n%.*s\n", (int)suite.len, suite.ptr, n, (int)md.len,
           md.ptr, (int)want.len, want.ptr, (int)got.len, got.ptr);
}

int main(int argc, char **argv)
{
    const char *path = "tests/corpus.json";
    const char *only = NULL;
    int report = 0;
    long show_max = 0, shown = 0;
    CuArena file_arena, scratch, out_arena;
    CuStr text;
    CuJson corpus;
    CuJsonError jerr;
    Suite suites[MAX_SUITES];
    size_t nsuites = 0, i, j;
    unsigned long total_fail = 0;
    int argi;

#if defined(__wasi__) || defined(__cerb__)
    /* The checker's WASI and Cerberus runs see no project files, as in the
     * library's tests/test_json.c; test_mark and test_ext still run there. */
    (void)argc;
    (void)argv;
    puts("SKIP c-mark corpus: no project files under this runtime");
    return 0;
#endif
    for (argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--report") == 0) {
            report = 1;
        } else if (strcmp(argv[argi], "--suite") == 0 && argi + 1 < argc) {
            only = argv[++argi];
        } else if (strcmp(argv[argi], "--show") == 0 && argi + 1 < argc) {
            show_max = atol(argv[++argi]);
        } else {
            path = argv[argi];
        }
    }
    cu_arena_init(&file_arena);
    cu_arena_init(&scratch);
    cu_arena_init(&out_arena);
    if (!cu_file_read(&file_arena, path, &text)) {
        fprintf(stderr, "cannot read %s\n", path);
        return 2;
    }
    if (!cu_json_parse(&file_arena, text, &corpus, &jerr) || corpus.kind != CU_JSON_ARRAY) {
        fprintf(stderr, "%s is not a JSON array\n", path);
        return 2;
    }
    if (corpus.n == 0) {
        fprintf(stderr, "%s contains no corpus cases\n", path);
        return 2;
    }
    for (i = 0; i < corpus.n; i++) {
        const CuJson *c = &corpus.items[i];
        const CuJson *o = cu_json_find(c, cu_str_cstr("options"));
        CuStr suite = field(c, "suite");
        CuMarkOptions opt;
        CuBuilder b;
        CuStr got;
        CuTemp t;
        CuMarkStatus st;
        int ok;
        if (only != NULL && !cu_str_eq(suite, cu_str_cstr(only))) continue;
        for (j = 0; j < nsuites && !cu_str_eq(suites[j].name, suite); j++) {
        }
        if (j == nsuites) {
            if (nsuites == MAX_SUITES) {
                fprintf(stderr, "too many suites\n");
                return 2;
            }
            suites[j].name = suite;
            suites[j].pass = 0;
            suites[j].fail = 0;
            nsuites++;
        }
        if (o == NULL || o->kind != CU_JSON_OBJECT) {
            fprintf(stderr, "corpus case without options\n");
            return 2;
        }
        options_of(o, &opt);
        t = cu_temp_begin(&out_arena);
        cu_sb_init(&b, &out_arena);
        st = cu_mark_render(&scratch, field(c, "markdown"), &opt, &b);
        ok = st == CU_MARK_OK && cu_sb_finish(&b, &got) && cu_str_eq(got, field(c, "html"));
        if (ok) {
            suites[j].pass++;
        } else {
            suites[j].fail++;
            total_fail++;
            if (shown < show_max) {
                if (st != CU_MARK_OK) got = cu_str_cstr("(render failed)");
                show(c, got);
                shown++;
            }
        }
        cu_temp_end(t);
    }
    if (nsuites == 0) {
        cu_arena_release(&out_arena);
        cu_arena_release(&scratch);
        cu_arena_release(&file_arena);
        fprintf(stderr, "corpus selection matched no cases\n");
        return 2;
    }
    for (j = 0; j < nsuites; j++) {
        printf("%-24.*s %5lu/%lu\n", (int)suites[j].name.len, suites[j].name.ptr, suites[j].pass,
               suites[j].pass + suites[j].fail);
    }
    cu_arena_release(&out_arena);
    cu_arena_release(&scratch);
    cu_arena_release(&file_arena);
    if (total_fail != 0 && !report) {
        printf("FAIL c-mark corpus: %lu cases differ from goldmark v1.6.0\n", total_fail);
        return 1;
    }
    if (total_fail == 0) printf("PASS c-mark corpus: every case matches goldmark v1.6.0 byte for byte\n");
    return 0;
}
