# Makefile - CoCo bubble cartridge ROM
#
# Requires lwasm (lwtools) and python3.

LWASM   ?= lwasm
SRC     := src/bubble.asm
DEPS    := $(wildcard src/*.asm) $(wildcard src/*.inc)
BUILD   := build

# A 16 bit checksum of the sources, shown on the boot screen as
# "BUILD xxxx", so the image in the socket can be identified.
BUILDID := $(shell cat $(sort $(DEPS)) | cksum | awk '{print $$1 % 65536}')

# The cartridge window is $C000-$FEFF; images are padded to a full
# 16K so they can be burned directly into a 16K bank.
ROMSIZE := 16384

# Both images, because the regression suite runs the mock one: a
# default target that built only the hardware image would leave the
# tests measuring a stale ROM, which is a very expensive kind of
# quiet.  Check the BUILD stamp on the boot screen if in any doubt
# about which image is running.
all: $(BUILD)/bubble.rom $(BUILD)/bubble-mock.rom

mock: $(BUILD)/bubble-mock.rom

help:
	@echo 'CoCo bubble cartridge ROM'
	@echo
	@echo 'Targets:'
	@echo '  make          both images (default)'
	@echo '  make mock     bubble-mock.rom RAM-backed, for the emulator'
	@echo '  make size     how much of the $$C000-$$FEFF window is used'
	@echo '  make clean    remove $(BUILD)/'
	@echo
	@echo 'Both images pad to $(ROMSIZE) bytes for burning.  One image'
	@echo 'covers every arrangement: whether a Multi-Pak is present, and'
	@echo 'which slots hold the bubble and a floppy controller, are'
	@echo 'detected at cold start.'
	@echo
	@echo 'Tests - need xroar (with its GDB target) and the BASIC ROMs:'
	@echo '  python3 test/suite.py            the regression suite'
	@echo '  python3 test/suite.py dir save   only the matching cases'
	@echo '  python3 test/interop.py <decb>   ToolShed media format check'
	@echo
	@echo 'On hardware, BUBDIAG exercises the driver: bare for the'
	@echo 'read-only tests, BUBDIAG 1 adds a destructive spot'
	@echo 'write/verify, BUBDIAG 2 the whole surface.'
	@echo
	@echo 'lwasm not on PATH?   make LWASM=/path/to/lwasm'

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bubble.rom: $(DEPS) | $(BUILD)
	$(LWASM) --6809 --format=raw --define=BUILDID=$(BUILDID) \
		--output=$(BUILD)/bubble.bin \
		--list=$(BUILD)/bubble.lst --symbols $(SRC)
	@$(MAKE) --no-print-directory pad IN=$(BUILD)/bubble.bin OUT=$@

$(BUILD)/bubble-mock.rom: $(DEPS) | $(BUILD)
	$(LWASM) --6809 --format=raw --define=MOCK=1 --define=BUILDID=$(BUILDID) \
		--output=$(BUILD)/bubble-mock.bin \
		--list=$(BUILD)/bubble-mock.lst --symbols $(SRC)
	@$(MAKE) --no-print-directory pad IN=$(BUILD)/bubble-mock.bin OUT=$@

# Pad an assembled image out to ROMSIZE with $FF.
pad:
	@python3 -c "import sys; \
d=open('$(IN)','rb').read(); \
n=$(ROMSIZE); \
sys.exit('image is %d bytes, larger than %d' % (len(d), n)) if len(d)>n else None; \
open('$(OUT)','wb').write(d + b'\xff'*(n-len(d))); \
print('$(OUT): %d bytes of code, padded to %d' % (len(d), n))"


size: $(BUILD)/bubble.rom
	@python3 -c "d=open('$(BUILD)/bubble.bin','rb').read(); \
print('code %d bytes, %d free in the \$$C000-\$$FEFF window' % (len(d), 16128-len(d)))"

clean:
	rm -rf $(BUILD)

.PHONY: all mock help pad size clean
