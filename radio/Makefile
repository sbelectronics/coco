# ===================================================================
# CoCo AM/FM radio cartridge - ROM build
#
# Needs the Macro Assembler AS (asl and p2bin), GNU make and Python 2.7
# or 3.  On Windows, mingw32-make from a Git Bash shell works.
#
#   make            build/radio.bin, 16,128 bytes, image offset 0 = $C000,
#                   with the module firmware patch built in (fetched on
#                   first use by tools/mkpatch.py)
#   make dev        Multi-Pak bench build, see below
#   make emu        build/radio-emu.bin for emulator UI work
#   make patch      re-extract the module firmware patch
#   make clean
#
# The firmware patch is NXP/Catena copyrighted and is not distributed with
# this source.  Without it the module never leaves boot state and will not
# tune.  make TEFPATCH=0 builds without it.
#
# BEFORE PROGRAMMING A CHIP: turn Software Data Protection OFF in the
# programmer.  A14 is grounded on this board, so the ROM can never send
# the AT28C256 unlock sequence (it needs chip address $5555), and with
# SDP on every preset save fails.  See rom/ramcode.asm.
# ===================================================================

# asl and p2bin from tools/bin if present, otherwise from PATH.  Either
# can be overridden:  make ASL=/opt/asl/asl P2BIN=/opt/asl/p2bin
ASL     ?= $(if $(wildcard tools/bin/asl.exe),tools/bin/asl.exe,asl)
P2BIN   ?= $(if $(wildcard tools/bin/p2bin.exe),tools/bin/p2bin.exe,p2bin)

# Probe for a working interpreter.  On Windows the WindowsApps "python"
# is a stub that fails when launched from a shell, so candidates are
# tried rather than just looked up on PATH.  Paths stay in a form native
# make can hand to CreateProcess, so C:/... rather than /c/...
PYTHON  ?= $(shell for p in python3 python C:/Python27/python.exe; do \
             "$$p" -c "" >/dev/null 2>&1 && echo "$$p" && break; done)

B        = build
# Seconds since the epoch, wrapped to 16 bits.  Shown on the main, test
# and About screens so a stale burn is obvious at a glance.
BUILDID := $(shell echo $$(( $$(date +%s) % 65536 )))
ASLFLAGS = -q -i rom -D BUILDID=$(BUILDID)

# The firmware patch is required for the radio to work at all, so it is
# built in by default and fetched by tools/mkpatch.py if missing.
# make TEFPATCH=0 builds without it, for a check of the digital bring-up.
TEFPATCH ?= 1
ifeq ($(TEFPATCH),1)
ASLFLAGS += -D TEF_PATCH=01
PATCHDEP  = rom/tefpatch.bin
endif

SRC      = rom/radio.asm rom/equates.inc rom/i2c.asm rom/tef.asm \
           rom/kbd.asm rom/screen.asm rom/font.inc rom/store.asm \
           rom/ui.asm rom/ramcode.asm

.PHONY: all emu dev patch clean dirs FORCE

all: $(B)/radio.bin

dirs:
	@mkdir -p $(B)

# ---- the real ROM ---------------------------------------------------
# -r fixes the image at $C000-$FEFF and p2bin fills the gaps between
# ORGs with $FF, so the source does not have to pad itself.
$(B)/radio.bin: $(SRC) $(PATCHDEP) | dirs
	$(ASL) $(ASLFLAGS) -L -olist $(B)/radio.lst -o $(B)/radio.p rom/radio.asm
	$(P2BIN) $(B)/radio.p $@ -r '$$C000-$$FEFF' -l '$$FF'
	$(PYTHON) tools/checkrom.py $@

# ---- emulator build -------------------------------------------------
# Skips the ID check, the probe and the patch, makes every I2C
# transaction succeed without touching the bus, and returns a canned
# strong stereo signal.  Never program a chip with this.
emu: $(B)/radio-emu.bin

$(B)/radio-emu.bin: $(SRC) $(PATCHDEP) | dirs
	$(ASL) $(ASLFLAGS) -D EMU=01 -L -olist $(B)/radio-emu.lst -o $(B)/radio-emu.p rom/radio.asm
	$(P2BIN) $(B)/radio-emu.p $@ -r '$$C000-$$FEFF' -l '$$FF'

# ---- Multi-Pak development build ------------------------------------
# For bench work with the radio cartridge and a multicart in one MPI.
# The ROM under test runs from the MULTICART, so CTS is left exactly as
# found and only SCS is forced to the radio's slot.  The preset store is
# RAM-only in this build, and the $FF40 scratch test is the only EEPROM
# path that can be exercised; see rom/equates.inc.
# Override the slot with  make dev DEVSLOT=2
DEVSLOT ?= 4

dev: $(B)/radio-dev.bin

# FORCE, because DEVSLOT is a make variable and make cannot see that it
# changed: "make dev DEVSLOT=2" after a slot-4 build would otherwise be a
# silent no-op.
$(B)/radio-dev.bin: $(SRC) $(PATCHDEP) FORCE | dirs
	$(ASL) $(ASLFLAGS) -D DEVMPI=01 -D MPI_SCS_SLOT=$(DEVSLOT) -L -olist $(B)/radio-dev.lst -o $(B)/radio-dev.p rom/radio.asm
	$(P2BIN) $(B)/radio-dev.p $@ -r '$$C000-$$FEFF' -l '$$FF'
	$(PYTHON) tools/checkrom.py $@

# ---- module firmware patch -----------------------------------------
# PATCHVER selects the silicon revision: 102 is the one the User Manual
# rev 1.6 documents and is the default; 205 is for later parts.
PATCHVER ?= 102

patch:
	$(PYTHON) tools/mkpatch.py rom/ --version $(PATCHVER)

rom/tefpatch.bin:
	$(PYTHON) tools/mkpatch.py rom/ --version $(PATCHVER)

# An empty rule anything can depend on to force a rebuild.  It must not
# be the first target in this file, or a bare "make" builds nothing.
FORCE:

clean:
	rm -rf $(B)
