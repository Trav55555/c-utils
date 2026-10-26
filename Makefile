DCHECK ?= $(HOME)/projects/dependable-c-agent-toolchain/bin/dcheck
PYTHON ?= python3
DEEP_SECONDS ?= 1800
WASI_SECONDS ?= 300

# make test needs only a C compiler; check needs dcheck. Deep also needs
# Python 3 and Linux cgroup v2 with a working user systemd manager. The
# language and warning flags always apply; CFLAGS adds to them.
STRICT = -std=c89 -pedantic-errors -Wall -Wextra -Werror
CFLAGS ?= -g
BUILD ?= build
TESTS := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

.PHONY: check deep test curun test-curun test-wasi-strict cuhealth test-cuhealth
check:
	$(DCHECK) gate
# DCHECK_JSON=FILE writes dcheck's JSON result there instead of its summary
# (the nightly CI run reads it; ci/findings.py).
deep:
	$(PYTHON) -I tools/verify_deep.py --seconds "$(DEEP_SECONDS)" -- "$(DCHECK)" deep $(if $(DCHECK_JSON),--json >"$(DCHECK_JSON)")
test: $(TESTS)
	@for t in $(TESTS); do echo "$$t"; "$$t" || exit 1; done

curun:
	$(MAKE) -C examples/curun BUILD="$(abspath $(BUILD))" curun
test-curun:
	$(MAKE) -C examples/curun BUILD="$(abspath $(BUILD))" test
test-wasi-strict:
	$(PYTHON) -I tools/verify_deep.py --seconds "$(WASI_SECONDS)" -- $(PYTHON) -I tests/wasi_strict.py --build "$(abspath $(BUILD))/wasi"
cuhealth:
	$(MAKE) -C examples/cuhealth BUILD="$(abspath $(BUILD))" all
test-cuhealth:
	$(MAKE) -C examples/cuhealth BUILD="$(abspath $(BUILD))" test

$(BUILD)/test_%: tests/test_%.c tests/death.h tests/align.h include/cu.h $(wildcard src/*.c) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) -Iinclude -o $@ $< $(wildcard src/*.c)

$(BUILD):
	mkdir -p $@
