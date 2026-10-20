DCHECK ?= $(HOME)/projects/dependable-c-agent-toolchain/bin/dcheck
PYTHON ?= python3
DEEP_SECONDS ?= 1800

# make test needs only a C compiler; check needs dcheck. Deep also needs
# Python 3 and Linux cgroup v2 with a working user systemd manager. The
# language and warning flags always apply; CFLAGS adds to them.
STRICT = -std=c89 -pedantic-errors -Wall -Wextra -Werror
CFLAGS ?= -g
BUILD ?= build
TESTS := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

.PHONY: check deep test curun test-curun cuhealth test-cuhealth
check:
	$(DCHECK) gate
deep:
	$(PYTHON) -I tools/verify_deep.py --seconds "$(DEEP_SECONDS)" -- "$(DCHECK)" deep
test: $(TESTS)
	@for t in $(TESTS); do echo "$$t"; "$$t" || exit 1; done

curun:
	$(MAKE) -C examples/curun BUILD="$(abspath $(BUILD))" curun
test-curun:
	$(MAKE) -C examples/curun BUILD="$(abspath $(BUILD))" test
cuhealth:
	$(MAKE) -C examples/cuhealth BUILD="$(abspath $(BUILD))" all
test-cuhealth:
	$(MAKE) -C examples/cuhealth BUILD="$(abspath $(BUILD))" test

$(BUILD)/test_%: tests/test_%.c tests/death.h tests/align.h include/cu.h $(wildcard src/*.c) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) -Iinclude -o $@ $< $(wildcard src/*.c)

$(BUILD):
	mkdir -p $@
