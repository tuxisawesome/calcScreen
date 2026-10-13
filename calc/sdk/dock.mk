# Include from a CE C toolchain (v15) makefile, BEFORE the toolchain include:
#   include ../path/to/calc/sdk/dock.mk
#   include $(shell cedev-config --makefile)
#
# Pulls in the portable engine (core/) plus the embedded CE USB/LCD/key driver
# (calc/hal_ce/). The program then has the full dock.h API.
DOCK_SDK_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
DOCK_ROOT     := $(DOCK_SDK_DIR)..
DOCK_CORE_DIR := $(DOCK_ROOT)/../core
DOCK_HAL_DIR  := $(DOCK_ROOT)/hal_ce

EXTRA_C_SOURCES += \
	$(DOCK_SDK_DIR)dock.c \
	$(DOCK_CORE_DIR)/dock_api.c \
	$(DOCK_CORE_DIR)/dock_dev.c \
	$(DOCK_CORE_DIR)/dock_proto.c \
	$(DOCK_CORE_DIR)/dock_screen.c \
	$(DOCK_HAL_DIR)/src/usb_ce.c \
	$(DOCK_HAL_DIR)/src/lcd_ce.c \
	$(DOCK_HAL_DIR)/src/time_ce.c \
	$(DOCK_HAL_DIR)/src/cfg_ce.c

EXTRA_PREASM_SOURCES += $(DOCK_HAL_DIR)/src/dma_ce.S

CFLAGS   += -I$(DOCK_SDK_DIR) -I$(DOCK_CORE_DIR) -I$(DOCK_HAL_DIR)/include
CXXFLAGS += -I$(DOCK_SDK_DIR) -I$(DOCK_CORE_DIR) -I$(DOCK_HAL_DIR)/include

# The link-time-optimization step recompiles with $(LTOFLAGS), which defaults to
# $(CFLAGS). Our -I include paths are meaningless there (no C input), so the
# compiler warns "argument unused during compilation". Drop them from LTOFLAGS
# so the build is warning-clean while keeping them for the real compiles.
LTOFLAGS ?= $(filter-out -I%,$(CFLAGS))
