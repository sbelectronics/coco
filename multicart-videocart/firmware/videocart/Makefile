# Wrapper around cmake/ninja for this board. PICO_SDK_PATH comes from the
# environment, from ../local.mk (git-ignored, for a per-machine path), or
# falls back to ~/pico-sdk.
-include ../local.mk
export PICO_SDK_PATH ?= $(HOME)/pico-sdk

# Build on LOCAL disk, not in the source tree: on a network share CMake's
# in-place reconfigure can fail with "Operation not permitted" when file
# ownership churns between sessions. Only plain file copies touch the
# tree, landing the finished artifacts in ./build where flashing and OTA
# expect them. Override BUILD_DIR if $(HOME) isn't on local disk.
#
# Keyed by the checkout's path, so two clones (or a moved checkout) get
# their own CMake cache. Sharing one cache makes the second clone fail
# with "does not match the source used to generate cache".
BUILD_KEY := $(shell printf '%s' '$(CURDIR)' | cksum | cut -d' ' -f1)
BUILD_DIR ?= $(HOME)/.cache/coco-fw/$(BUILD_KEY)/videocart

all:
	cmake -G Ninja -B $(BUILD_DIR) -S .
	cmake --build $(BUILD_DIR)
	@mkdir -p build
	@cp $(BUILD_DIR)/coco-videocart.uf2 $(BUILD_DIR)/coco-videocart.bin \
	    $(BUILD_DIR)/coco-videocart.elf build/
	@echo "artifacts -> videocart/build/"

flash: all
	sudo picotool load build/coco-videocart.uf2 -f -x

clean:
	rm -rf build $(BUILD_DIR)

.PHONY: all flash clean
