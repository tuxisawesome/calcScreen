# calcScreen top-level makefile. Each part has its own makefile; this just
# drives them. See README.md.

.PHONY: all test host calc tools sim e2e clean help

help:
	@echo "calcScreen targets:"
	@echo "  make test   - build core and run the unit tests (host, ASan/UBSan)"
	@echo "  make host   - build the Pi daemon (dockd) and the simulator"
	@echo "  make calc   - build the calculator programs (needs the CE C toolchain)"
	@echo "  make tools  - build bavconv (the Bad Apple converter)"
	@echo "  make e2e    - build host + run the end-to-end simulation"
	@echo "  make all    - test + host + tools (everything that builds without CEDEV)"
	@echo "  make clean  - remove all build output"

# Everything that builds on a plain Linux box (no CE toolchain needed).
all: test host tools

test:
	$(MAKE) -C tests check

host:
	$(MAKE) -C host

tools:
	$(MAKE) -C tools

e2e:
	$(MAKE) -C host e2e

# The calculator programs. Needs the CE C toolchain on PATH (cedev-config);
# set CEDEV if it is installed elsewhere.
calc:
	$(MAKE) -C calc/dock
	$(MAKE) -C calc/badapple

clean:
	$(MAKE) -C tests clean
	$(MAKE) -C host clean
	$(MAKE) -C tools clean
	-$(MAKE) -C calc/dock clean
	-$(MAKE) -C calc/badapple clean
	rm -rf build
