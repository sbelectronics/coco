# Building and flashing CoCo cartridge firmware

Linux host, Pico SDK 2.2.0. `firmware/multicart/` and
`firmware/videocart/` are the CMake projects; `firmware/common/` has no
CMakeLists of its own.

## One-time setup

```bash
sudo apt install cmake ninja-build gcc-arm-none-eabi libnewlib-arm-none-eabi \
                 libstdc++-arm-none-eabi-newlib build-essential

git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
```

The SDK needs `lib/lwip`, `lib/cyw43-driver`, `lib/mbedtls` and
`lib/tinyusb` initialised. `lib/tinyusb` is required by
`pico_enable_stdio_usb`; the SDK only warns rather than failing when it
is missing, so a build that "succeeds" without it has no USB console.
LittleFS is vendored in `lib/littlefs` (v2.9.3) — no extra fetch.

`PICO_SDK_PATH` is taken from the environment, then from a git-ignored
`firmware/local.mk` (`export PICO_SDK_PATH ?= /path/to/pico-sdk`), then
defaults to `~/pico-sdk`.

## WiFi credentials

Runtime provisioning is the normal path: connect USB-CDC, press any key
to enter the config menu, then

```
SET SSID myssid
SET PSK  mypassword
SET AUTH WPA2
SAVE
REBOOT
```

`config_local.h` (git-ignored; copy `config_local.h.example`) can bake in
build-time fallbacks used only when the flash cfg sector has never been
provisioned.

## Build

```bash
cd firmware
make                 # both boards
make videocart       # one board (or: cd videocart && make)
make test            # host simulation suite
make audit           # post-link RAM-residency check
make preflight       # everything provable without a CoCo
make flash-multicart # picotool load -f -x (per-board: make -C multicart flash)
```

Raw form, same thing:

```bash
cd firmware/multicart          # or firmware/videocart
cmake -G Ninja -B build .
cmake --build build
```

Output: `firmware/<board>/build/coco-<board>.uf2` (plus `.elf` / `.bin`).

The board Makefiles put the CMake build directory on **local disk**
(`$(HOME)/.cache/coco-fw/<board>`) and copy the finished artifacts back
into `<board>/build/`. A source tree on a network share can hand newly
written files inconsistent ownership across sessions, and CMake's
in-place reconfigure then fails with `Operation not permitted`; building
off the share sidesteps that. Override `BUILD_DIR` if `$(HOME)` isn't
local.

Footprints (0.5.1 / 0.58.0):

| Board | flash | static SRAM |
|---|---|---|
| multicart | 427 KB | 285 KB |
| videocart | 447 KB | 468 KB |

The videocart is tight: 96 KB of serve tables, a 64 KB bus mirror and a
125 KB framebuffer leave about 40 KB free.

### If ninja aborts "manifest still dirty ... system time is not set"

Clock skew between the file server and the build host: ninja sees the
share's source mtimes as perpetually "newer" than its own manifest. Fix
the clocks (NTP on both), not the build.

## Baked-in carts

Drop cartridge images in `carts/` and build a filesystem image that ships
them pre-installed:

```bash
cp carts.conf.example carts.conf   # optional — edit to set titles/flags
#   ... put .rom/.ccc/.bin files in carts/ ...
make multicart-carts               # firmware + a cart FS image
make flash-multicart-carts         # flashes both in one shot
```

If `carts.conf` is absent, every file in `carts/` is baked in with default
metadata (title from filename, autostart on, scheme by size); with it,
only the listed files are included, with the titles and flags you give.

`tools/mklfs.c`, built from the **same vendored littlefs** the firmware
mounts, writes a LittleFS image (geometry read from `config.h`) plus a
catalog, and emits a UF2 at the filesystem partition —
`build-carts/coco-carts-fs.uf2`. The partition is identical on both
boards, so one image serves either. Flashing it touches **only** the data
partition: the firmware slot and the cfg sector (WiFi credentials) are
untouched. The image is a full partition, erased pages included: picotool
writes only the blocks present in a UF2, so a sparse image leaves stale
LittleFS metadata behind and the next mount can stitch two filesystems
together.

`make carts-selftest` bakes a throwaway fixture and mounts it with the
shipping `romfs.c` — run it after any change to `mklfs.c` or the catalog
format. The generated Bank Diagnostic cart (`tools/mkdiag.py`) is
included in every image; a committed copy in `carts/` is the fallback
when python is absent.

## Post-link verification

Run `make audit` after any change to `bus.c` or `ota.c`. These symbols
MUST be RAM-resident or the design breaks in ways no bench test will
reliably reproduce, and GCC can inline a `__not_in_flash_func` into a
flash-resident caller without warning. The audit checks the linked
image:

```bash
ELF=multicart/build/coco-multicart.elf   # or videocart/build/coco-videocart.elf

arm-none-eabi-nm $ELF | grep -E 'bus_loop|bus_core1_main|copy_b_to_a|video_note_reg|s_main'
# every address must start with 2 (SRAM), not 1 (flash XIP)

arm-none-eabi-objdump -d $ELF \
  | awk '/^200/{r=1} /^100/{r=0} r && /bl\t10/{print "FLASH CALL:", $0}'
# must print nothing
```

`s_main` is in the list because the videocart's serve DMA dereferences it
on every window read; a call audit alone cannot see a data symbol.

## Host-side simulation tests

`firmware/test/` runs the actual firmware sources on a Linux host under
ASan/UBSan against simulated hardware:

```bash
cd firmware/test && make && make run     # "== ALL TESTS PASSED =="
```

| Test | What it simulates |
|---|---|
| test_sweep | the '153 mux board model vs `sweep_assemble()` — all 64K addresses × 8 flag combos with garbage on the unused pads |
| test_serve_pipeline | the videocart serve chain end to end: board → sampler SM instruction by instruction → DMA lookup → serve SM gates, 4 banks × 64K × read/write |
| test_sampler_timing | decodes the assembled PIO programs and asserts '153 settle, push latency, E-low fit, follower pairing and the clk_sys pinning |
| test_stub6809 | a 6809 interpreter RUNS the cold-boot stub, clobbering the cartridge window at the $FF5F write like the real swap does |
| test_hstx_frame | walks the scanout control blocks into an HSTX stream model: 525×800 VESA timing, sync placement, doubled rows, ring rewind, crossbar polarity, island placement and IRQ_QUIET |
| test_vdg | renders alpha/SG4/CG/RG modes from a fake 64K mirror, SAM offsets and half-height scans, the artifact model; writes PPMs (see test/results/) |
| test_audio | TERC4/BCH/InfoFrames decoded back out of the island words, sample-rate and delivery uniformity, the synth's box filter, DC blocking, DAC/PB latches, filter order |
| test_cfg | cfg pages from older versions migrate with defaults for the fields they predate |
| test_romfs | real romfs.c + real LittleFS over fake NOR flash (program-only-clears-bits semantics), catalog persistence across simulated reboots |
| test_select | the selection state machine's full choreography incl. insertion-style start, staging bounds, timeout and load-failure paths |
| test_ui / test_ui_vc | real ui.c + ssd1306.c against a wire-level SSD1306 model and a scripted quadrature encoder; the videocart's device-settings menu |

`make preflight` runs the build, the PIO instruction budget check, the
tests, the audit, the serve-table alignment check and a call-free check
on the core-1 loop, and prints what it deliberately cannot prove ('153
settle on hot silicon, SRAM arbiter contention, drive strength).

## Flash

```bash
picotool load multicart/build/coco-multicart.uf2 -f -x
```

or hold BOOTSEL while plugging in USB and copy the `.uf2` to the
`RPI-RP2350` drive.

**OTA:** `<board>/build/coco-<board>.bin` (not the `.uf2`) is what the web
UI's firmware upload expects. The staged image is CRC-checked, read back
from flash, and sanity-checked for this board's hostname string before it
is applied, so a multicart image cannot be applied to a videocart.

## Bring-up order

Each step is independently observable.

1. **Bench, USB only, no CoCo.** Expect the CDC banner. `DIAG` should
   report the expander and OLED as `ok` — that validates I2C, the
   PCF8574 and the display before any bus work. The videocart shows a
   colour-bar test pattern on HDMI.
2. **WiFi.** Provision credentials, `REBOOT`, confirm the IP prints and
   `http://coco-multicart.local/` loads. Upload a ROM; confirm it
   appears in both the web list and the OLED.
3. **In the CoCo, no ROM selected.** The cart must be inert: BASIC boots
   normally. This proves the power-on gate/reset idle state (the '125
   polarity) is right. On the videocart the BASIC screen appears on HDMI.
4. **Serve a flat ROM.** Select from the OLED. The CoCo should reset and
   launch. If it hangs, check the web panel: `cold boot` must count via
   the stub, `serving` must be YES, and the drop counters must be zero.
5. **Bank-write timing.** The 600 ns default (multicart) / 400 ns
   (videocart) is hardware-validated. If a different CoCo or a marginal
   part ever gives unstable banking, load the Bank Diagnostic, watch its
   `LOOP F=` count, and adjust with `SET STROBE <ns>` + `SAVE` + `REBOOT`
   or from the Settings page.
