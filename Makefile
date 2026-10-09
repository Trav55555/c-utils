DCHECK ?= $(HOME)/projects/dependable-c-agent-toolchain/bin/dcheck

# make test needs only a C compiler; check and deep need dcheck. The
# language and warning flags always apply; CFLAGS adds to them.
STRICT = -std=c89 -pedantic-errors -Wall -Wextra -Werror
CFLAGS ?= -g
BUILD ?= build
TESTS := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

.PHONY: check deep test
check:
	$(DCHECK) gate
deep:
	$(DCHECK) deep
test: $(TESTS)
	@for t in $(TESTS); do echo "$$t"; "$$t" || exit 1; done

$(BUILD)/test_%: tests/test_%.c tests/death.h include/cu.h $(wildcard src/*.c) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) -Iinclude -o $@ $< $(wildcard src/*.c)

$(BUILD):
	mkdir -p $@
