DCHECK ?= $(HOME)/projects/dependable-c-agent-toolchain/bin/dcheck
BUILD ?= ../build/c-mark
STRICT = -std=c89 -pedantic-errors -Wall -Wextra -Werror
CFLAGS ?= -O2 -g
DEFINES = -D_DEFAULT_SOURCE -D_GNU_SOURCE
LIBSRC := $(wildcard ../src/*.c)
MARKSRC := $(wildcard src/*.c)
MARKHDR := $(wildcard include/*.h) $(wildcard src/*.h)
ORACLE := $(BUILD)/oracle

.PHONY: test report check corpus
test: $(BUILD)/test_corpus
	$(BUILD)/test_corpus tests/corpus.json

# Progress while porting: every suite's count, exit 0.
report: $(BUILD)/test_corpus
	$(BUILD)/test_corpus tests/corpus.json --report

$(BUILD)/test_%: tests/test_%.c $(MARKSRC) $(MARKHDR) ../include/cu.h $(LIBSRC) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) $(DEFINES) -Iinclude -Isrc -I../include -o $@ $< $(MARKSRC) $(LIBSRC)

# The reference renderer (Go, goldmark v1.6.0 pinned by go.sum) and the
# corpus it writes. Needs Go and the goldmark module; never part of the C
# build or its tests, which read the committed tests/corpus.json.
$(ORACLE): oracle/*.go oracle/go.mod oracle/go.sum | $(BUILD)
	cd oracle && GOTOOLCHAIN=local go build -o $(abspath $@) .
corpus: $(ORACLE)
	$(ORACLE) -corpus "$$(cd oracle && GOTOOLCHAIN=local go list -m -f '{{.Dir}}' github.com/yuin/goldmark)" > tests/corpus.json

check:
	$(DCHECK) gate

$(BUILD):
	mkdir -p $@
