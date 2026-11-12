DCHECK ?= $(HOME)/projects/dependable-c-agent-toolchain/bin/dcheck
PYTHON ?= python3
DEEP_SECONDS ?= 1800
WASI_SECONDS ?= 300
CONTROLS_SECONDS ?= 900

# make test needs only a C compiler; check needs dcheck. Deep also needs
# Python 3 and Linux cgroup v2 with a working user systemd manager. The
# language and warning flags always apply; CFLAGS adds to them.
STRICT = -std=c89 -pedantic-errors -Wall -Wextra -Werror
CFLAGS ?= -g
BUILD ?= build
FUZZ_RUNS ?= 20000
FUZZ_SECONDS ?= 10
TESTS := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

.PHONY: check deep controls test curun test-curun test-wasi-strict cuhealth test-cuhealth fuzz-smoke
check:
	$(DCHECK) gate
# DCHECK_JSON=FILE writes dcheck's JSON result there instead of its summary
# (the nightly CI run reads it; ci/findings.py).
deep:
	$(PYTHON) -I tools/verify_deep.py --seconds "$(DEEP_SECONDS)" -- "$(DCHECK)" deep $(if $(DCHECK_JSON),--json >"$(DCHECK_JSON)")
# Seeds known lifetime and failure-path faults into scratch copies and requires
# the focused tests, fuzz input and CBMC proof to catch each (docs/FAULT-CONTROLS.md).
# Needs what deep needs for fuzzing and CBMC, under the same scope runner.
controls:
	$(PYTHON) -I tools/verify_deep.py --seconds "$(CONTROLS_SECONDS)" -- $(PYTHON) -I tools/fault_controls.py --dcheck "$(DCHECK)"
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

# Builds each fuzz harness with clang and libFuzzer + ASan/UBSan, replays its
# .seeds directory, then runs a short bounded random pass (FUZZ_RUNS inputs or
# FUZZ_SECONDS, whichever ends first; new inputs go under $(BUILD)/fuzz, never
# into the seeds). Any crash fails. Needs clang; the c-mark harness is included
# when that subproject is present. Not part of test.
FUZZ_CC ?= clang
FUZZ_FLAGS = -std=c89 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -D_DEFAULT_SOURCE -D_GNU_SOURCE
FUZZ_LIMITS = -max_total_time=$(FUZZ_SECONDS) -runs=$(FUZZ_RUNS)
fuzz-smoke:
	@set -e; mkdir -p "$(BUILD)/fuzz"; \
	run() { name=$$1; seeds=$$2; echo "fuzz $$name"; \
		mkdir -p "$(BUILD)/fuzz/$$name.corpus"; \
		if [ -d "$$seeds" ]; then "$(BUILD)/fuzz/$$name" -runs=0 "$$seeds"; \
			"$(BUILD)/fuzz/$$name" $(FUZZ_LIMITS) "$(BUILD)/fuzz/$$name.corpus" "$$seeds"; \
		else "$(BUILD)/fuzz/$$name" $(FUZZ_LIMITS) "$(BUILD)/fuzz/$$name.corpus"; fi; }; \
	for f in fuzz/fuzz_*.c; do n=$$(basename $$f .c); \
		$(FUZZ_CC) $(FUZZ_FLAGS) -Iinclude -o "$(BUILD)/fuzz/$$n" $$f src/*.c; \
		run $$n fuzz/$$n.seeds; done; \
	for f in $(wildcard c-mark/fuzz/fuzz_mark.c); do \
		$(FUZZ_CC) $(FUZZ_FLAGS) -Iinclude -Ic-mark/include -Ic-mark/src -o "$(BUILD)/fuzz/fuzz_mark" $$f c-mark/src/*.c src/*.c; \
		run fuzz_mark c-mark/fuzz/fuzz_mark.seeds; done

$(BUILD)/test_%: tests/test_%.c tests/death.h tests/align.h include/cu.h $(wildcard src/*.c) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) -Iinclude -o $@ $< $(wildcard src/*.c)

$(BUILD):
	mkdir -p $@
