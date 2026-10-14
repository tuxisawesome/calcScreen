# ----------------------------------------------------------------------------
# RESIDINS - installer for the RESIDOCK resident fast-mirror flash app.
# *** EXPERIMENTAL - CEmu-ONLY. This program writes flash. See docs/RESIDENT.md.
# ----------------------------------------------------------------------------
# Built with fasmg (the CE assembly toolchain), NOT the C toolchain, because it
# reuses the standard CE asm include set. Supply those includes via INCLUDE:
#
#   make FASMG=/path/to/fasmg INCLUDE=/path/to/ce-asm-include
#
# INCLUDE must contain ez80.inc, tiformat.inc and ti84pceg.inc (the includes
# shipped with the CE assembly tools / CE-Programming asm-docs). The app image
# is taken from the resident app's .8ek, which must be built first.
FASMG   ?= fasmg
INCLUDE ?= $(CEDEV)/include/asm
RESIDOCK ?= ../resident/bin/RESIDOCK.8ek
APP2C    ?= ../tools/app2c.py

export INCLUDE

all: RESIDINS.8xp

# The raw app image (the .8ek minus its 78-byte TIFL header), embedded by
# installer.asm via `file 'RESIDOCK.bin'`.
RESIDOCK.bin: $(RESIDOCK) $(APP2C)
	python3 $(APP2C) --raw $(RESIDOCK) RESIDOCK.bin

RESIDINS.8xp: installer.asm RESIDOCK.bin
	$(FASMG) installer.asm RESIDINS.8xp

clean:
	rm -f RESIDINS.8xp RESIDOCK.bin

.PHONY: all clean
