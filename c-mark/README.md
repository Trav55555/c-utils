# c-mark

c-mark is a C89 Markdown-to-HTML renderer in the `c-utils` repository. Its
compatibility target is Goldmark v1.6.0. The public API is in
[`include/cu_mark.h`](include/cu_mark.h); the C library has no Go runtime
dependency. Go builds the reference oracle and corpus generator; table
generators also use Go or Goldmark data.

## Build and test

Run these commands from the repository root:

```sh
make -C c-mark test
make -C c-mark test-tools
```

`test` builds with strict C89 warnings and runs targeted renderer tests,
extension tests, and the committed Goldmark corpus. The corpus has 1,576
entries across 20 suite labels. It runs Goldmark's own cases under suite
options and again under Hugo 0.120.4's extensions and heading IDs, with unsafe
HTML enabled. `test-tools` checks the corpus runner's empty-selection errors
and the differential fuzzer's failure handling; it needs Python 3 but not Go.

To compare generated documents with the pinned Go reference, run:

```sh
make -C c-mark diff-fuzz SEED=1 N=2000
```

This checks 2,000 documents per option set. It needs Python 3 and Go, and exits
with failure for either an output mismatch or a Goldmark oracle error. The
oracle dependency is checksum-pinned in `oracle/go.sum`.

Applications include `c-mark/include` and the repository's `include`, then
compile `c-mark/src/*.c` with `src/*.c` using the strict C89 flags shown in
`c-mark/Makefile`. Initialize every `CuMarkOptions` field. Pass a scratch arena
separate from the arena that owns the output builder. Check for
`CU_MARK_OK`; discard the builder output for any other status.

## Supported options and bounds

The default options implement Goldmark's core Markdown behavior. Flags enable
tables, strikethrough, linkification, and task lists. Other flags enable
typographer substitutions, definition lists, footnotes, automatic heading IDs,
attributes, and Hugo's heading ID style. `CU_MARK_HUGO` combines Hugo 0.120.4's
default extensions and heading IDs. Renderer flags control unsafe HTML and
URLs, XHTML output, and hard line wraps. Table alignment can use Goldmark's
default, an `align` attribute, a style attribute, or no alignment. A callback
can replace fenced code rendering.

c-mark does not implement CJK options, emoji, custom linkify patterns,
custom footnote options or ID functions, custom typographer substitutions,
render hooks other than fenced code, or Goldmark's plugin interfaces. The renderer caps input at
16 MiB and nesting at 256 for block containers and recursive attribute values.
The API accepts byte strings, including NUL and invalid UTF-8. Resource
failure returns `CU_MARK_NOMEM`; a bound violation returns `CU_MARK_LIMIT`.
Either status may leave partial output, which callers must discard.

## Reference and qualification status

The oracle uses Goldmark v1.6.0, pinned by module checksum. Its entity and
case-fold tables supply `mark_entities.c` and `mark_casefold.c`. Go 1.22.4
generated `mark_unicode.c` from Unicode 15.0.0 tables. License notices are in
`LICENSE.goldmark` and `LICENSE.go`.

The committed corpus provides finite coverage, not proof of agreement for
every input. A fixed-seed-1 run generated 22,000 documents across 11 option
sets. It produced zero output mismatches and 21,999 successful oracle
comparisons; Goldmark panicked once in `table-default`. Goldmark v1.6.0 panics
in `text.blockReader.Peek` from `parser.linkParser.Parse` on the valid UTF-8
input `1. '\n--\n\t][]`; the oracle returns no HTML. The same input panics in
raw Hugo mode, while plain CommonMark mode succeeds. c-mark renders it, but no
reference output exists for comparison. The fuzzer reports the oracle error as
a failure and does not exclude the input.

The regions check accepts the numbered heading ID path in `mark_parser.c`.
That static result does not prove every lifetime safe or replace the native
tests and corpus comparison.
