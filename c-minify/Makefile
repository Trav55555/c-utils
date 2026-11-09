DCHECK ?= $(HOME)/projects/dependable-c-agent-toolchain/bin/dcheck
BUILD ?= ../build/c-minify
STRICT = -std=c89 -pedantic-errors -Wall -Wextra -Werror
CFLAGS ?= -O2 -g
DEFINES = -D_DEFAULT_SOURCE -D_GNU_SOURCE
LIBSRC := $(wildcard ../src/*.c)
MINSRC := $(wildcard src/*.c)
MINHDR := $(wildcard include/*.h) $(wildcard src/*.h)
ORACLE := $(BUILD)/oracle
DRIVER := $(BUILD)/minify_run
# The Go module cache holds the pinned minify and parse sources; the
# corpus also takes the live site's parity baseline and the exporter's
# unminified pages, which live outside this repository.
MINIFY_MOD = $$(cd oracle && GOTOOLCHAIN=local go list -m -f '{{.Dir}}' github.com/tdewolff/minify/v2)
PARSE_MOD = $$(cd oracle && GOTOOLCHAIN=local go list -m -f '{{.Dir}}' github.com/tdewolff/parse/v2)
LIVE ?= $(HOME)/projects/cuweb/examples/site/parity/baseline/2026-11-07
PAGES ?= $(HOME)/projects/cuweb/build/default/site-live/posts
# diff-fuzz: the seed and the number of documents per set.
SEED ?= 1
N ?= 2000

.PHONY: test report check corpus tables diff-fuzz
test: $(BUILD)/test_minify $(BUILD)/test_corpus
	$(BUILD)/test_minify
	$(BUILD)/test_corpus tests/corpus.json

# Progress while porting: every suite's counts, exit 0.
report: $(BUILD)/test_corpus
	$(BUILD)/test_corpus tests/corpus.json --report

$(BUILD)/test_%: tests/test_%.c $(MINSRC) $(MINHDR) ../include/cu.h $(LIBSRC) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) $(DEFINES) -Iinclude -Isrc -I../include -o $@ $< $(MINSRC) $(LIBSRC)

# The generated tables (src/minify_html_hash.h, src/minify_html_table.c)
# from minify's html/hash.go and html/table.go.
tables:
	python3 -I tools/gen_tables.py "$(MINIFY_MOD)" src

# The reference minifier (Go, minify v2.20.5 pinned by go.sum) and the
# corpus it writes. Needs Go and the modules; never part of the C build or
# its tests, which read the committed tests/corpus.json.
$(ORACLE): oracle/*.go oracle/go.mod oracle/go.sum | $(BUILD)
	cd oracle && GOTOOLCHAIN=local go build -o $(abspath $@) .
corpus: $(ORACLE)
	$(ORACLE) -corpus "$(MINIFY_MOD)" -parse "$(PARSE_MOD)" -live "$(LIVE)" -pages "$(PAGES)" > tests/corpus.json

# Differential fuzzing against the oracle (tools/diff_fuzz.py): random
# documents from SEED, N per set, every byte compared; mismatches are
# minimised and saved under tests/diff_fuzz/. Needs Go; never part of
# make test.
$(DRIVER): tools/minify_run.c $(MINSRC) $(MINHDR) ../include/cu.h $(LIBSRC) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) $(DEFINES) -Iinclude -I../include -o $@ $< $(MINSRC) $(LIBSRC)
diff-fuzz: $(DRIVER) $(ORACLE)
	python3 -I tools/diff_fuzz.py --oracle $(ORACLE) --driver $(DRIVER) --seed $(SEED) --count $(N) --save tests/diff_fuzz

check:
	$(DCHECK) gate

$(BUILD):
	mkdir -p $@
