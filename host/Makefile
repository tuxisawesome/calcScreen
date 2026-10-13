# host/Makefile - builds the Raspberry Pi dock daemon and the calculator
# simulator.
#
#   make -C host                 -> build/dockd, build/dock-sim (repo root)
#   make -C host CC=aarch64-linux-gnu-gcc PKG_CONFIG=aarch64-linux-gnu-pkg-config
#                                -> cross build for the Pi 4 (needs arm64 -dev
#                                   packages / a sysroot visible to pkg-config)
#   make -C host install [PREFIX=/usr/local] [DESTDIR=...]
#   make -C host e2e             -> headless end-to-end test (e2e_test.sh)

ROOT    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
BUILD   ?= $(ROOT)/build
OBJ     := $(BUILD)/obj
PREFIX  ?= /usr/local
DESTDIR ?=

CC         ?= cc
PKG_CONFIG ?= pkg-config

# Build version, baked into dockd (--version, the diagnostics overlay and the
# USB updater's compare). Comes from the repo-root VERSION file; a short git
# hash is appended when this is a git checkout.
VERSION    := $(shell cat $(ROOT)/VERSION 2>/dev/null || echo 0.0.0)
GITHASH    := $(shell git -C $(ROOT) rev-parse --short HEAD 2>/dev/null)
ifneq ($(GITHASH),)
DOCKD_VERSION := $(VERSION)+g$(GITHASH)
else
DOCKD_VERSION := $(VERSION)
endif

WARN     := -Wall -Wextra -Werror
CFLAGS   ?= -O2
CPPFLAGS += -I$(ROOT)/core -I$(ROOT)/host/hal_linux -I$(ROOT)/host/dockd \
            -I$(ROOT)/host/hal_sim -MMD -MP \
            -DDOCKD_VERSION='"$(DOCKD_VERSION)"'
PKGS     := libdrm alsa libusb-1.0 libcurl
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS))
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs $(PKGS))

# core/ is portable C99; host code is Linux-only gnu99.
CORE_STD := -std=c99
HOST_STD := -std=gnu99

CORE_DOCKD := dock_proto dock_screen dock_host dock_keymap dock_settings \
              sha256 dock_ui dusb
CORE_SIM   := dock_proto dock_screen dock_dev dock_api
HAL_LINUX  := util display display_drm input_evdev usb_link unix_link storage \
              audio_alsa usb_devices net settings_store sdnotify sysstat
DOCKD_SRC  := main hhal settings_ui dusb_session overlay update
SIM_SRC    := sim_main sim_hal

DOCKD_OBJS := $(CORE_DOCKD:%=$(OBJ)/core/%.o) $(HAL_LINUX:%=$(OBJ)/hal_linux/%.o) \
              $(DOCKD_SRC:%=$(OBJ)/dockd/%.o)
SIM_OBJS   := $(CORE_SIM:%=$(OBJ)/core/%.o) $(OBJ)/hal_linux/util.o \
              $(SIM_SRC:%=$(OBJ)/hal_sim/%.o)

.PHONY: all clean install uninstall e2e overlay
all: $(BUILD)/dockd $(BUILD)/dock-sim

$(BUILD)/dockd: $(DOCKD_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(PKG_LIBS)

$(BUILD)/dock-sim: $(SIM_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(OBJ)/core/%.o: $(ROOT)/core/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CORE_STD) $(WARN) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(OBJ)/hal_linux/%.o: $(ROOT)/host/hal_linux/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_STD) $(WARN) $(CFLAGS) $(CPPFLAGS) $(PKG_CFLAGS) -c -o $@ $<

$(OBJ)/dockd/%.o: $(ROOT)/host/dockd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_STD) $(WARN) $(CFLAGS) $(CPPFLAGS) $(PKG_CFLAGS) -c -o $@ $<

$(OBJ)/hal_sim/%.o: $(ROOT)/host/hal_sim/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_STD) $(WARN) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BUILD)/dockd $(DESTDIR)$(PREFIX)/bin/dockd
	install -m 0755 $(BUILD)/dock-sim $(DESTDIR)$(PREFIX)/bin/dock-sim

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dockd $(DESTDIR)$(PREFIX)/bin/dock-sim

e2e: all
	$(ROOT)/host/e2e_test.sh

overlay: all
	$(ROOT)/host/overlay_test.sh

clean:
	rm -rf $(OBJ) $(BUILD)/dockd $(BUILD)/dock-sim

-include $(DOCKD_OBJS:.o=.d) $(SIM_OBJS:.o=.d)
