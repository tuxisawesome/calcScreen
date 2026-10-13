# ----------------------------------------------------------------------------
# RESIDOCK - calcScreen resident fast-mirror driver (EXPERIMENTAL flash app)
# ----------------------------------------------------------------------------
# A TI-84 Plus CE flash application whose only job is to host, at a stable flash
# address, a GetCSC hook that keeps the calcScreen fast USB mirror alive on the
# bare TI-OS homescreen after the DOCK program exits. See docs/RESIDENT.md.
#
# Built as an APPLICATION (.8ek) because, as of TI-OS 5.8.3, hook code is only
# allowed to live inside a flash app or the OS itself (docs/TIOS_HOOKS_SPEC.md
# section 1). The .8ek is embedded into the installer program by app2c.py and
# written to flash on-calc; it is never sent as a signed app.
NAME = RESIDOCK
DESCRIPTION = "calcScreen resident fast-mirror"
APPLICATION = YES
APPLICATION_DESCRIPTION = "calcScreen"
COMPRESSED = NO
COMMENT = "calcScreen"
# DOCK_RESIDENT_USB_CTX relocates usb_ce.c's state to a fixed RAM address so it
# survives this installer program's exit; it MUST equal RESIDENT_USB_CTX in
# src/resident.h (RESIDENT_BASE + 16 = 0xD13FD8 + 16 = 0xD13FE8).
CFLAGS = -Wall -Wextra -Oz -DDOCK_RESIDENT=1 -DDOCK_RESIDENT_USB_CTX=0xD13FE8
CXXFLAGS = -Wall -Wextra -Oz -DDOCK_RESIDENT=1 -DDOCK_RESIDENT_USB_CTX=0xD13FE8

include ../sdk/dock.mk
include $(shell cedev-config --makefile)
