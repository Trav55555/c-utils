DCHECK ?= $(HOME)/projects/dependable-c-agent-toolchain/bin/dcheck
PYTHON ?= python3
BUILD ?= ../../build
STRICT = -std=c89 -pedantic-errors -Wall -Wextra -Werror
CFLAGS ?= -g
DEFINES = -D_DEFAULT_SOURCE -D_GNU_SOURCE
LIBSRC := $(wildcard ../../src/*.c)

.PHONY: curun test check deep
curun: $(BUILD)/curun

$(BUILD)/curun: main.c src/curun.c include/curun.h ../../include/cu.h $(LIBSRC) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) $(DEFINES) -Iinclude -I../../include -o $@ main.c src/curun.c $(LIBSRC)

$(BUILD)/curun-unit: tests/test_curun.c src/curun.c include/curun.h ../../include/cu.h $(LIBSRC) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) $(DEFINES) -Iinclude -I../../include -o $@ tests/test_curun.c src/curun.c $(LIBSRC)

$(BUILD)/curun-policy: tests/test_policy.c src/curun.c include/curun.h ../../include/cu.h $(LIBSRC) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) $(DEFINES) -Iinclude -I../../include -o $@ tests/test_policy.c src/curun.c $(LIBSRC)

test: $(BUILD)/curun $(BUILD)/curun-unit $(BUILD)/curun-policy
	$(BUILD)/curun-unit
	$(BUILD)/curun-policy
	$(PYTHON) -I tests/cli_test.py $(BUILD)/curun

check:
	$(DCHECK) gate

deep:
	$(PYTHON) -I ../../tools/verify_deep.py --seconds 1800 -- "$(DCHECK)" deep

$(BUILD):
	mkdir -p $@
