DCHECK ?= $(HOME)/projects/dependable-c-agent-toolchain/bin/dcheck

.PHONY: check deep
check:
	$(DCHECK) gate
deep:
	$(DCHECK) deep
