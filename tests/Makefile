# tests/Makefile - host unit tests for core/ plus portability checks.
#
#   make -C tests              build and run every test (ASan + UBSan)
#   make -C tests portability  core/ with -m32 (gcc and/or clang), with the
#                              CE toolchain's ez80-clang when installed, and a
#                              grep for disallowed headers / heap / floats
#   make -C tests check        both
#   make -C tests clean
#
# To hook into a top-level Makefile:
#   test:
#   	$(MAKE) -C tests check

CC      ?= cc
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -pedantic -g -O1
# core/ is held to a stricter standard than the tests themselves.
CORE_CFLAGS = $(CFLAGS) -Wconversion -Wno-sign-conversion
LDFLAGS ?=

CORE_DIR = ../core
DOCKD_DIR = ../host/dockd
BUILD    = build
OUT      = out

CORE_SRCS = $(wildcard $(CORE_DIR)/*.c)
CORE_HDRS = $(wildcard $(CORE_DIR)/*.h)
TEST_SRCS = test_main.c test_proto.c test_screen.c test_devhost.c \
            test_sha256.c test_settings.c test_keymap.c test_dusb.c test_ui.c \
            test_update.c

CORE_OBJS = $(patsubst $(CORE_DIR)/%.c,$(BUILD)/core_%.o,$(CORE_SRCS))
TEST_OBJS = $(patsubst %.c,$(BUILD)/%.o,$(TEST_SRCS))
# Host objects exercised by the tests (dependency-light host code only).
HOST_OBJS = $(BUILD)/host_update.o
BIN       = $(BUILD)/run_tests

.PHONY: all test check portability clean

all: test

test: $(BIN) | $(OUT)
	./$(BIN)

check: test portability

$(BIN): $(CORE_OBJS) $(HOST_OBJS) $(TEST_OBJS)
	$(CC) $(CFLAGS) $(SAN) -o $@ $^ $(LDFLAGS)

$(BUILD)/core_%.o: $(CORE_DIR)/%.c $(CORE_HDRS) | $(BUILD)
	$(CC) $(CORE_CFLAGS) $(SAN) -c -o $@ $<

$(BUILD)/host_%.o: $(DOCKD_DIR)/%.c $(DOCKD_DIR)/%.h $(CORE_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -I$(CORE_DIR) -c -o $@ $<

$(BUILD)/%.o: %.c test.h $(CORE_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -c -o $@ $<

$(BUILD) $(OUT):
	mkdir -p $@

# ---------------------------------------------------------------- portability
#
# The host usually has no 32-bit libc headers, so the -m32 build is
# freestanding: the compiler's own <stdint.h>/<stddef.h>/<stdbool.h> plus a
# 5-line <string.h> shim (shim32/). That is also closer to the eZ80 target.

PORT_FLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -Wconversion -Wno-sign-conversion
CEDEV     ?= /opt/CEdev
EZ80_CC    = $(CEDEV)/bin/ez80-clang

portability: | $(BUILD)
	@echo "== include / heap / float / mutable-static check"
	@bad=$$(grep -n '^[[:space:]]*#[[:space:]]*include' $(CORE_DIR)/*.c $(CORE_DIR)/*.h | \
	    grep -v -E '#[[:space:]]*include[[:space:]]+("[A-Za-z0-9_]+\.h"|<(stdint|stddef|stdbool|string)\.h>)'); \
	  if [ -n "$$bad" ]; then echo "disallowed includes:"; echo "$$bad"; exit 1; fi
	@bad=$$(grep -n -w -E 'malloc|calloc|realloc|free|float|double|printf|FILE' $(CORE_DIR)/*.c $(CORE_DIR)/*.h | \
	    grep -v -E '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)'); \
	  if [ -n "$$bad" ]; then echo "disallowed identifiers:"; echo "$$bad"; exit 1; fi
	@bad=$$(grep -n -P '^\s*static\s+(?!const\b|inline\b)[^()]*(=|;)\s*$$' $(CORE_DIR)/*.c $(CORE_DIR)/*.h); \
	  if [ -n "$$bad" ]; then echo "mutable static state:"; echo "$$bad"; exit 1; fi
	@echo "   ok"
	@done=0; \
	if command -v gcc >/dev/null 2>&1; then \
	  echo "== gcc -m32 (freestanding)"; \
	  inc=$$(gcc -print-file-name=include); \
	  for f in $(CORE_SRCS); do \
	    gcc -m32 -ffreestanding -nostdinc -isystem $$inc -isystem shim32 $(PORT_FLAGS) \
	        -c $$f -o $(BUILD)/m32.o || exit 1; \
	  done; echo "   ok"; done=1; \
	fi; \
	if command -v clang >/dev/null 2>&1; then \
	  echo "== clang -m32 (freestanding)"; \
	  inc=$$(clang -print-resource-dir)/include; \
	  for f in $(CORE_SRCS); do \
	    clang -m32 -ffreestanding -nostdinc -isystem $$inc -isystem shim32 $(PORT_FLAGS) \
	          -c $$f -o $(BUILD)/m32.o || exit 1; \
	  done; echo "   ok"; done=1; \
	fi; \
	if [ $$done = 0 ]; then echo "no gcc or clang for the -m32 check"; exit 1; fi
	@if [ -x $(EZ80_CC) ]; then \
	  echo "== ez80-clang (24-bit int, CE toolchain)"; \
	  for f in $(CORE_SRCS); do \
	    $(EZ80_CC) -target ez80 -nostdinc -isystem $(CEDEV)/include $(PORT_FLAGS) -Oz \
	               -S $$f -o $(BUILD)/ez80.s || exit 1; \
	  done; echo "   ok"; \
	else \
	  echo "== ez80-clang not found at $(EZ80_CC): skipped"; \
	fi

clean:
	rm -rf $(BUILD) $(OUT)
