/* Runs c-minify over the corpus (tests/corpus.json, written by the oracle
 * from minify v2.20.5's own test tables, the parse lexer tests and the live
 * site) and requires the oracle's exact bytes.
 *
 *   test_corpus [CORPUS] [--report] [--suite NAME] [--show N]
 *
 * A case counts as ok when the bytes match (or the oracle recorded an
 * error and the port reports CU_MINIFY_INVALID), as unsupported when the
 * port reports CU_MINIFY_UNSUPPORTED and the oracle recorded that the
 * input reached the CSS, JS, JSON or SVG minifier, and as wrong otherwise:
 * an UNSUPPORTED for an input that reached none of those is wrong too.
 * Without --report it exits 1 when any suite has a wrong case; with it,
 * it prints every suite's counts and exits 0, for progress while porting.
 * --suite limits the run to one suite, --show prints up to N wrong cases. */
#include "cu_minify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SUITES 32

typedef struct Suite {
    CuStr name;
    unsigned long ok;
    unsigned long unsupported;
    unsigned long wrong;
} Suite;

static int flag(const CuJson *options, const char *name)
{
    const CuJson *v = cu_json_find(options, cu_str_cstr(name));
    return v != NULL && v->kind == CU_JSON_BOOL && v->truth;
}

static void options_of(const CuJson *o, CuMinifyOptions *opt)
{
    static const struct {
        const char *name;
        unsigned long bit;
    } names[] = {
        {"keepComments", CU_MINIFY_KEEP_COMMENTS},
        {"keepConditionalComments", CU_MINIFY_KEEP_CONDITIONAL_COMMENTS},
        {"keepDefaultAttrVals", CU_MINIFY_KEEP_DEFAULT_ATTR_VALS},
        {"keepDocumentTags", CU_MINIFY_KEEP_DOCUMENT_TAGS},
        {"keepEndTags", CU_MINIFY_KEEP_END_TAGS},
        {"keepQuotes", CU_MINIFY_KEEP_QUOTES},
        {"keepWhitespace", CU_MINIFY_KEEP_WHITESPACE},
    };
    size_t i;
    opt->flags = 0;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (flag(o, names[i].name)) opt->flags |= names[i].bit;
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

/* Whether the oracle recorded that the input reached a minifier this slice
 * does not port. */
static int reaches_unported(const CuJson *c)
{
    const CuJson *r = cu_json_find(c, cu_str_cstr("reaches"));
    size_t i;
    if (r == NULL || r->kind != CU_JSON_ARRAY) return 0;
    for (i = 0; i < r->n; i++) {
        const CuJson *s = &r->items[i];
        if (s->kind != CU_JSON_STRING) continue;
        if (cu_str_eq(s->text, cu_str_cstr("css")) || cu_str_eq(s->text, cu_str_cstr("js")) ||
            cu_str_eq(s->text, cu_str_cstr("json")) || cu_str_eq(s->text, cu_str_cstr("svg"))) {
            return 1;
        }
    }
    return 0;
}

static const char *status_name(CuMinifyStatus st)
{
    switch (st) {
    case CU_MINIFY_OK:
        return "OK";
    case CU_MINIFY_NOMEM:
        return "NOMEM";
    case CU_MINIFY_LIMIT:
        return "LIMIT";
    case CU_MINIFY_UNSUPPORTED:
        return "UNSUPPORTED";
    case CU_MINIFY_INVALID:
        return "INVALID";
    }
    return "?";
}

static void show(const CuJson *c, CuMinifyStatus st, CuStr got)
{
    const CuJson *no = cu_json_find(c, cu_str_cstr("no"));
    CuStr suite = field(c, "suite");
    CuStr in = field(c, "input");
    CuStr want = field(c, "expected");
    long n = 0;
    if (no == NULL || !cu_json_to_long(no, &n)) n = -1;
    printf("---- %.*s %ld (%s)\ninput:\n%.*s\nwant:\n%.*s\ngot:\n%.*s\n", (int)suite.len, suite.ptr, n,
           status_name(st), (int)in.len, in.ptr, (int)want.len, want.ptr, (int)got.len, got.ptr);
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
    unsigned long total_wrong = 0, total_unsupported = 0;
    int argi;

#if defined(__wasi__) || defined(__cerb__)
    /* The checker's WASI and Cerberus runs see no project files, as in the
     * library's tests/test_json.c; test_minify still runs there. */
    (void)argc;
    (void)argv;
    puts("SKIP c-minify corpus: no project files under this runtime");
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
    for (i = 0; i < corpus.n; i++) {
        const CuJson *c = &corpus.items[i];
        const CuJson *o = cu_json_find(c, cu_str_cstr("options"));
        const CuJson *err = cu_json_find(c, cu_str_cstr("error"));
        CuStr suite = field(c, "suite");
        CuStr type = field(c, "type");
        CuMinifyType mt;
        CuMinifyOptions opt;
        CuBuilder b;
        CuStr got;
        CuTemp t;
        CuMinifyStatus st;
        int ok, unsupported;
        if (only != NULL && !cu_str_eq(suite, cu_str_cstr(only))) continue;
        for (j = 0; j < nsuites && !cu_str_eq(suites[j].name, suite); j++) {
        }
        if (j == nsuites) {
            if (nsuites == MAX_SUITES) {
                fprintf(stderr, "too many suites\n");
                return 2;
            }
            suites[j].name = suite;
            suites[j].ok = 0;
            suites[j].unsupported = 0;
            suites[j].wrong = 0;
            nsuites++;
        }
        if (o == NULL || o->kind != CU_JSON_OBJECT) {
            fprintf(stderr, "corpus case without options\n");
            return 2;
        }
        if (!cu_minify_type_for(type, &mt)) {
            fprintf(stderr, "corpus case with unknown type %.*s\n", (int)type.len, type.ptr);
            return 2;
        }
        options_of(o, &opt);
        t = cu_temp_begin(&out_arena);
        cu_sb_init(&b, &out_arena);
        st = cu_minify(&scratch, mt, &opt, field(c, "input"), &b);
        if (!cu_sb_finish(&b, &got)) got = cu_str_empty();
        if (err != NULL && err->kind == CU_JSON_STRING) {
            ok = st == CU_MINIFY_INVALID;
        } else {
            ok = st == CU_MINIFY_OK && cu_str_eq(got, field(c, "expected"));
        }
        unsupported = !ok && st == CU_MINIFY_UNSUPPORTED && reaches_unported(c);
        if (ok) {
            suites[j].ok++;
        } else if (unsupported) {
            suites[j].unsupported++;
            total_unsupported++;
        } else {
            suites[j].wrong++;
            total_wrong++;
            if (shown < show_max) {
                show(c, st, got);
                shown++;
            }
        }
        cu_temp_end(t);
    }
    printf("%-34s %6s %12s %6s\n", "suite", "ok", "unsupported", "wrong");
    for (j = 0; j < nsuites; j++) {
        printf("%-34.*s %6lu %12lu %6lu\n", (int)suites[j].name.len, suites[j].name.ptr, suites[j].ok,
               suites[j].unsupported, suites[j].wrong);
    }
    cu_arena_release(&out_arena);
    cu_arena_release(&scratch);
    cu_arena_release(&file_arena);
    if (total_wrong != 0 && !report) {
        printf("FAIL c-minify corpus: %lu cases differ from minify v2.20.5\n", total_wrong);
        return 1;
    }
    if (total_wrong == 0) {
        printf("PASS c-minify corpus: every ported case matches minify v2.20.5 byte for byte (%lu unsupported)\n",
               total_unsupported);
    }
    return 0;
}
