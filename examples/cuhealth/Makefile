DCHECK ?= $(HOME)/projects/dependable-c-agent-toolchain/bin/dcheck
PYTHON ?= python3
BUILD ?= build/default
STRICT = -std=c89 -pedantic-errors -Wall -Wextra -Werror
CFLAGS ?= -g
DEFINES = -D_DEFAULT_SOURCE -D_GNU_SOURCE
LIBSRC := $(wildcard ../../src/*.c)
.PHONY: all test check deep
all: $(BUILD)/cuhealth
$(BUILD)/cuhealth: main.c src/cuhealth.c include/cuhealth.h ../../include/cu.h $(LIBSRC) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) $(DEFINES) -Iinclude -I../../include -o $@ main.c src/cuhealth.c $(LIBSRC)
$(BUILD)/unit: tests/test_health.c src/cuhealth.c include/cuhealth.h ../../include/cu.h $(LIBSRC) | $(BUILD)
	$(CC) $(STRICT) $(CFLAGS) $(DEFINES) -Iinclude -I../../include -o $@ tests/test_health.c src/cuhealth.c $(LIBSRC)
test: $(BUILD)/cuhealth $(BUILD)/unit
	$(BUILD)/unit
	$(PYTHON) -I tests/socket_test.py $(BUILD)/cuhealth
check:
	$(PYTHON) -I ../../tools/verify_deep.py --seconds 600 -- "$(DCHECK)" gate
deep:
	$(PYTHON) -I ../../tools/verify_deep.py --seconds 900 -- "$(DCHECK)" deep
$(BUILD):
	mkdir -p $@
