# ======================================================================
# Top-level firmware build. Per-board Makefiles live in multicart/ and
# videocart/.
#
# Run `make help` for the target list (authoritative — kept in the help
# recipe below, not duplicated here). A bare `make` builds both boards.
# ======================================================================

BOARDS = multicart videocart

all: $(BOARDS)

# Everything provable without the CoCo. REQUIRED before asking anyone to
# flash: PIO budget, host simulations of the serve pipeline and its
# timing, RAM residency, table alignment, loop shape. See tools/preflight.sh
# for what it deliberately does NOT prove.
preflight:
	@bash tools/preflight.sh

help:
	@printf '%s\n' \
	  'CoCo cartridge firmware — make targets' \
	  '' \
	  'Build:' \
	  '  make                     build both boards in place (default)' \
	  '  make multicart           build one board' \
	  '  make videocart' \
	  '  make test                host simulation suite (must stay green)' \
	  '  make audit               post-link RAM-residency check;' \
	  '                           REQUIRED after any change to bus.c/ota.c' \
	  '  make preflight           FULL pre-flash gate: PIO budget, serve-' \
	  '                           pipeline + timing simulations, alignment,' \
	  '                           loop shape. RUN THIS BEFORE FLASHING.' \
	  '' \
	  'Flash (picotool auto-reset over USB; -f reboots into BOOTSEL):' \
	  '  make flash-multicart     flash firmware only' \
	  '  make flash-videocart' \
	  '' \
	  'Baked-in carts (debug convenience — ROMs preloaded, no upload):' \
	  '  make carts-fs            build the cart image (auto-includes the' \
	  '                           generated Bank Diagnostic cart)' \
	  '  make multicart-carts     firmware + baked-in carts (see carts.conf)' \
	  '  make videocart-carts' \
	  '  make flash-multicart-carts   flash firmware AND carts in one shot' \
	  '  make flash-videocart-carts' \
	  '  make carts-selftest      prove an mklfs image mounts under romfs.c' \
	  '' \
	  'Utility:' \
	  '  make clean               remove all build products' \
	  '  make help                this list'

$(BOARDS):
	$(MAKE) -C $@

test:
	$(MAKE) -C test
	$(MAKE) -C test run

flash-multicart:
	$(MAKE) -C multicart flash

flash-videocart:
	$(MAKE) -C videocart flash

# ---- baked-in carts (debug convenience) ------------------------------
# The filesystem partition (offset/size in config.h) is identical on
# both boards, so one image serves either. mklfs is built from the same
# vendored littlefs the firmware mounts, guaranteeing on-disk format
# match; it reads geometry from multicart/config.h + the test shim.
CARTS_DIR  ?= carts
CARTS_CONF ?= carts.conf
CARTS_OUT   = build-carts
MKLFS       = $(CARTS_OUT)/mklfs
FS_UF2      = $(CARTS_OUT)/coco-carts-fs.uf2
LFS_DIR     = lib/littlefs

# The bank-switch diagnostic cart is generated from tools/mkdiag.py (no
# ROM dump needed) and baked into every carts image as a default cart.
# It is regenerated on each carts-fs build when python is available;
# mkdiag.py is deterministic, so this produces no git churn. A committed
# copy in carts/ keeps the diagnostic present even without python.
PYTHON   := $(shell command -v python3 2>/dev/null || command -v python 2>/dev/null)
DIAG_ROM  = $(CARTS_DIR)/Bank Diagnostic.rom

# Only pass --conf if the file actually exists (else: scan the dir).
CONF_ARG = $(if $(wildcard $(CARTS_CONF)),--conf $(CARTS_CONF),)

$(MKLFS): tools/mklfs.c $(LFS_DIR)/lfs.c $(LFS_DIR)/lfs_util.c
	@mkdir -p $(CARTS_OUT)
	cc -O2 -Wall -I multicart -I test/shim -I $(LFS_DIR) \
	   -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ASSERT \
	   -o $@ tools/mklfs.c $(LFS_DIR)/lfs.c $(LFS_DIR)/lfs_util.c

# carts-fs is PHONY (always rebakes) so adding/removing/renaming ROMs in
# carts/ ALWAYS yields a matching image. A file-timestamp dependency
# can't be used: CoCo ROM names contain spaces, which make cannot
# represent as prerequisites. Rebaking is ~1 s, so just always do it.
# Family 'absolute' (0xe48bff57): the family-independent, fixed-address
# UF2 family — correct for a raw data partition, accepted by the RP2350
# bootrom and picotool regardless of the firmware's arm-s family.
carts-fs: $(MKLFS)
	@mkdir -p $(CARTS_OUT)
	@if [ -n "$(PYTHON)" ]; then \
	   echo "gen   $(DIAG_ROM)"; \
	   $(PYTHON) tools/mkdiag.py "$(DIAG_ROM)" >/dev/null; \
	 elif [ -f "$(DIAG_ROM)" ]; then \
	   echo "note: python not found; using committed $(DIAG_ROM)"; \
	 else \
	   echo "WARNING: no python and no committed $(DIAG_ROM) — diagnostic omitted"; \
	 fi
	$(MKLFS) --carts-dir $(CARTS_DIR) $(CONF_ARG) \
	         --family 0xe48bff57 --out $(FS_UF2)

multicart-carts: multicart carts-fs
videocart-carts: videocart carts-fs

# Self-test: bake a throwaway fixture and mount it with the shipping
# romfs.c — proves an mklfs image is firmware-mountable with correct
# metadata. Runs hermetically (its own temp carts dir), no hardware.
carts-selftest: $(MKLFS)
	@set -e; t=$(CARTS_OUT)/selftest; rm -rf $$t; mkdir -p $$t/carts; \
	 head -c 8192  /dev/urandom > $$t/carts/alpha.rom; \
	 head -c 40000 /dev/urandom > $$t/carts/big_game.ccc; \
	 head -c 2048  /dev/urandom > $$t/carts/util.bin; \
	 printf '%s\n' 'alpha.rom' 'big_game.ccc | Big Game | 1 | auto' \
	               'util.bin | Util | 0 | flat' > $$t/carts.conf; \
	 $(MKLFS) --carts-dir $$t/carts --conf $$t/carts.conf --bin $$t/fs.bin >/dev/null; \
	 cc -O1 -Wall -Wno-format-truncation -fsanitize=address,undefined \
	    -I test/shim -I multicart -I common -I $(LFS_DIR) \
	    -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ASSERT \
	    -o $$t/verify test/verify_carts.c test/shim/fakeflash.c \
	    $(LFS_DIR)/lfs.c $(LFS_DIR)/lfs_util.c; \
	 $$t/verify $$t/fs.bin 3

# Flash firmware AND the cart filesystem in ONE BOOTSEL session. Do NOT
# use two `-f` loads back-to-back: `-f` reboots to app after each, and
# the second `-f` then races the still-rebooting device ("returned an
# error: rebooting"). Instead reboot to BOOTSEL once, write both regions,
# and -x (execute) only on the last. The leading `-` tolerates a device
# already in BOOTSEL (nothing to force-reset). Neither region touches the
# cfg sector, so WiFi creds survive.
flash-multicart-carts: multicart-carts
	-sudo picotool reboot -f -u
	@sleep 3
	sudo picotool load $(FS_UF2)
	sudo picotool load multicart/build/coco-multicart.uf2 -x

flash-videocart-carts: videocart-carts
	-sudo picotool reboot -f -u
	@sleep 3
	sudo picotool load $(FS_UF2)
	sudo picotool load videocart/build/coco-videocart.uf2 -x

# The awk pass catches calls from RAM code into flash; the nm pass
# catches the symbols themselves. s_main is in the list because the serve
# DMA dereferences it every window read — if it ever landed in flash the
# lookup would fault, and a call audit alone cannot see a DATA symbol.
audit:
	@set -e; for b in $(BOARDS); do \
	  E=$$b/build/coco-$$b.elf; \
	  test -f $$E || { echo "audit: $$E is not built - run 'make $$b' first"; \
	                   exit 1; }; \
	  echo "== $$E =="; \
	  arm-none-eabi-nm $$E | grep -E \
	    ' (bus_loop|bus_core1_main|copy_b_to_a|video_note_reg|s_main)$$' \
	    | awk '{print; if ($$1 !~ /^2/) { bad=1 } } END { exit bad }'; \
	  n=$$(arm-none-eabi-objdump -d $$E \
	    | awk '/^200/{r=1} /^100/{r=0} r && /bl\t10/{c++} END{print c+0}'); \
	  echo "flash calls from RAM code: $$n"; \
	  test $$n -eq 0; \
	done; echo "audit OK"

clean:
	for b in $(BOARDS); do $(MAKE) -C $$b clean; done
	$(MAKE) -C test clean
	rm -rf $(CARTS_OUT)

.PHONY: all help test preflight flash-multicart flash-videocart audit clean $(BOARDS) \
        carts-fs multicart-carts videocart-carts carts-selftest \
        flash-multicart-carts flash-videocart-carts
