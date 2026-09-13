# CoCo Multicart and Videocart

Two Raspberry Pi Pico 2 W cartridges for the TRS-80 Color Computer 1/2
that hold your whole ROM collection. Pick a game from the OLED menu or a
web browser and the CoCo reboots straight into it — no swapping
cartridges, no EPROMs, no disk drive.

- **Multicart** — the ROM cartridge. A0–A13 and D0–D7 on direct GPIOs.
- **Videocart** — the same cartridge plus **HDMI video and audio out**.
  It snoops every bus write into a 64 K mirror of the CoCo's RAM, renders
  the VDG display at 640×480 over HSTX, and reconstructs the CoCo's sound
  from its PIA writes into HDMI audio.

> **The videocart must be plugged directly into the CoCo.** It works by
> watching *every* bus cycle, and most Multi-Pak Interface implementations
> buffer the cartridge bus so a slot only sees the cycles addressed to it —
> which leaves the videocart with nothing to mirror. The multicart is
> fine in an MPI.

Also in this tree: `tools/bas2rom.py`, which converts cassette games into
autostart cartridge ROMs.

- **Author:** Dr. Scott M Baker — <https://www.smbaker.com>
- **Internals:** [HOW-IT-WORKS.md](HOW-IT-WORKS.md) — an architectural
  guide to both boards: the 6809E bus deadlines, the RP2350 features the
  design leans on, and how every subsystem fits together. Read it before
  changing the firmware.

---

## What it does

- **~2.5 MB of ROMs** in the Pico's flash — roughly 150 standard 16 KB
  carts — browsed on a 128×64 OLED with a rotary encoder, or over WiFi
  from any browser on your network.
- **Selecting a game cold-boots the CoCo into it.** The cartridge drives
  the bus, the `!RESET` line and the autostart line itself, so there is
  no power-cycling and no cable-swapping. A tiny 6809 stub runs for one
  boot to clear the warm-start flag, so every selection is a true cold
  start.
- **Banked carts** using the `$FF40` scheme that CoCoFLASH and FlashPak
  use (up to 128 KB on the multicart, 32 KB on the videocart).
- **Insertion-style start** for the cassette conversions that need a
  fully booted BASIC: the cart stays invisible until BASIC reaches READY,
  then appears and autostarts exactly the way pushing a cartridge into a
  running machine does.
- **Boot to Extended BASIC** with no cartridge mapped, straight from the
  menu.
- **Upload over WiFi.** Drag a `.rom`/`.ccc`/`.bin` onto the web page;
  firmware updates go the same way.
- **Baked-in carts.** Build a filesystem image with a directory of ROMs
  pre-installed and flash it in one go.
- **Cassette conversion.** `bas2rom.py` turns a `.cas` or `.bas` file
  into a cartridge that loads and starts by itself, including banked
  layouts for programs larger than 16 KB.
- **Bank-switching diagnostic** cart, generated at build time, that
  verifies every bank and stress-tests the bank register.
- **Videocart only:** 640×480@60 HDMI (or plain DVI) picture of whatever
  the CoCo is displaying — every VDG text and graphics mode, PMODE 4
  artifact colour with selectable phase — and 48 kHz HDMI audio
  reconstructed from the 6-bit DAC and the single-bit sound output.

## Hardware

### Multicart

| | |
|---|---|
| MCU | Raspberry Pi Pico 2 W (RP2350) |
| Bus | A0–A13 on GP0–13, D0–D7 on GP14–21, `!CTS` on GP22 |
| Bank writes | SCS write strobe on GP28 (74HCT125 + diode), sampled by PIO |
| Display | SSD1306 128×64 OLED, I²C1 on GP26/27 |
| Input | Rotary encoder with push-button, via a PCF8574 on the same bus |
| Control | `!RESET` and the CART* autostart line, gated through a 74HCT125 |

### Videocart

| | |
|---|---|
| MCU | Raspberry Pi Pico 2 W (RP2350) |
| Bus | D0–D7 on GP0–7, E on GP8; A0–A15, R/W, `!CTS`, `!SCS` through three 74HCT153 muxes on GP9–11/20/22, swept by PIO on GP26/27 |
| HDMI | HSTX on GP12–19 |
| Display / input | Same OLED, encoder and PCF8574 as the multicart, on I²C0 (GP28/21) |
| Control | `!RESET` and CART*, as the multicart |

On both boards core 1 runs the bus service entirely from SRAM with
interrupts off, so it keeps meeting the CoCo's bus deadlines while core 0
is writing flash or servicing WiFi. On the videocart the ROM serve path
is entirely hardware (PIO + DMA + a precomputed table per bank); core 1
only pairs snooped writes with their addresses.

Both boards run the RP2350 at 250 MHz. That is a requirement, not a
tweak: at the SDK default the multicart's serve loop misses the `!CTS`
window behind a Multi-Pak, and the videocart's snoop loop overruns the
1117 ns bus cycle and drops writes.

## Building

You need CMake, Ninja, `arm-none-eabi-gcc`, the Pico SDK (2.2.0 with
`lib/lwip`, `lib/cyw43-driver`, `lib/mbedtls` and `lib/tinyusb`
initialised) and `picotool`. On Debian/Ubuntu:

```sh
sudo apt install cmake ninja-build gcc-arm-none-eabi libnewlib-arm-none-eabi \
                 libstdc++-arm-none-eabi-newlib build-essential
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
```

The board Makefiles use `PICO_SDK_PATH` from the environment, from a
git-ignored `firmware/local.mk`, or default to `~/pico-sdk`. LittleFS is
vendored in `firmware/lib/littlefs`; nothing else needs fetching.

From `firmware/`:

```sh
make                        # build both boards
make multicart              # or one board
make videocart
make test                   # host-side simulation suite (Linux, ASan/UBSan)
make audit                  # verify the core-1 loop is RAM-resident
make preflight              # everything provable without a CoCo
make flash-multicart        # picotool load -f -x
make flash-videocart
make help                   # the full list
```

Artifacts land in `firmware/<board>/build/`: `coco-<board>.uf2` for USB
flashing, `coco-<board>.bin` for the web UI's firmware uploader. The
CMake build directory itself is on local disk (`~/.cache/coco-fw/`), not
in the source tree, so a source tree on a network share does not upset
CMake's reconfigure; override `BUILD_DIR` if `$HOME` is not local.

Run `make test` before flashing anything; `make audit` after any change
to `bus.c` or `ota.c` — RAM residency is a property of the linked image,
not of a source attribute, and GCC will inline a `__not_in_flash_func`
back into flash without a word. [HOW-IT-WORKS.md](HOW-IT-WORKS.md)
explains that rule, and the rest of the invariants a change has to keep.

### First flash

Hold BOOTSEL while plugging the Pico in and copy the `.uf2` to the
`RPI-RP2350` drive, or:

```sh
picotool load firmware/multicart/build/coco-multicart.uf2 -f -x
```

Later updates can go over WiFi: the web page's **Firmware** section takes
the `.bin`, stages it in the second flash slot, verifies it, reboots and
copies it into place. Do not remove power for ~15 s after the reboot.

### WiFi

Connect to the USB serial port and press any key to enter the
configuration menu, then:

```
SET SSID myssid
SET PSK  mypassword
SET AUTH WPA2          (OPEN | WPA2 | WPA3 | MIXED)
SAVE
REBOOT
```

`SHOW` prints the configuration, `LIST`/`PLAY n` select ROMs from the
console, `DIAG` reports the I²C devices and bus counters, `SET WIFI OFF`
keeps the radio down at boot, `HELP` lists everything. Credentials live
in a CRC-protected flash sector that survives firmware and cart-image
updates. `firmware/common/config_local.h` (copy the `.example`) can bake
in build-time defaults for a board that has never been provisioned.

Once connected the cart announces itself by mDNS:
`http://coco-multicart.local/` or `http://coco-videocart.local/` (the IP
is also shown in the OLED header).

## Using it

### OLED menu

- **Rotate** scrolls, **push** selects. Selecting a ROM resets the CoCo
  into it.
- **Long-push** on a ROM opens its settings: **Autostart** (on by
  default), **Insert** (insertion-style start, see below), and
  **Delete** (press twice).
- The top item, **Settings**, holds the device settings: WiFi on/off and
  the bank-write strobe delay (both staged until **Save & Reboot**), and
  on the videocart the artifact phase, HDMI audio on/off (staged), volume,
  output filter and single-bit level (all live). **Back (no save)**
  discards staged edits. **About** shows the firmware version.
- **Extended BASIC** boots the machine with no cartridge mapped.

### Web UI

The page lists every ROM with its title, size, **Auto** and **Ins**
flags, and **Play / Save / Del** buttons. Drop files on the upload zone
to add ROMs. **Reset** re-runs the current cart's cold boot; **Boot to
Extended BASIC** does what it says. The **Settings** section mirrors the
OLED's, plus a **Firmware** uploader and a live **Bus diagnostics** panel
(lap timing, drop counters, serve echo, the renderer's decoded video mode
and the audio pipeline's state on the videocart).

The JSON API behind it:

```
GET    /api/status                 firmware, active ROM, free space, bus counters
GET    /api/roms                   the catalog
POST   /api/roms/<file>            upload (raw body)
DELETE /api/roms/<file>
POST   /api/select/<file>          run the selection sequence
POST   /api/basic                  boot to Extended BASIC
POST   /api/reset                  cold-boot the active cart again
POST   /api/meta/<file>?title=&autostart=&scheme=&insert=
GET    /api/settings               device settings
POST   /api/settings?wifi=&strobe=&artifact=&hdmiaudio=&audiogain=&audiofilter=&audioonebit=&reboot=
POST   /api/ota                    firmware .bin (raw body)
GET    /api/mirror?addr=<hex>&len=<dec>   videocart: hex dump of the RAM mirror
GET    /api/screen                 videocart: the bytes the last frame rendered
GET    /api/audiolog, /api/pcm     videocart: the audio register log and PCM capture
POST   /api/atest?on=0|1|2         videocart: test tone off / 1 kHz / 500 Hz
POST   /api/afilt?n=0..3           videocart: output filter, live
```

### Per-ROM settings

- **Autostart** — the cart asserts CART* so BASIC starts it; off leaves
  the ROM mapped for `EXEC &HC000`-style use.
- **Scheme** — `flat` (≤16 KB) or `bank16k` (bank number written to
  `$FF40`, 16 KB banks). Chosen automatically by size; images smaller
  than 16 KB and a power of two are mirrored across the window the way a
  small ROM's unconnected address lines would.
- **Insert** — insertion-style start. A CoCo starts a cartridge two
  different ways: on reset BASIC finds "DK" at `$C000` partway through its
  own initialisation, while a cart pushed into a running machine raises
  FIRQ and starts from a fully initialised BASIC. Cassette games converted
  to cartridges were written for the second, and some (Seadragon,
  Klendathu) only run that way. It costs about two seconds per selection,
  so it is off by default.

### Baked-in carts

Drop images in `firmware/carts/` (git-ignored) and optionally describe
them in `firmware/carts.conf` (see `carts.conf.example`: one line per
cart, `filename | title | autostart | scheme | insert`). Then:

```sh
make carts-fs                # build-carts/coco-carts-fs.uf2
make multicart-carts         # firmware + cart image (or videocart-carts)
make flash-multicart-carts   # flash both in one BOOTSEL session
make carts-selftest          # prove an image mounts with the real romfs code
```

The image is a full LittleFS partition built with the same vendored
LittleFS the firmware mounts, and it only touches the data partition, so
the firmware slot and your WiFi credentials are untouched. It always
includes the generated **Bank Diagnostic** cart, which selects each bank
in turn and prints `OK` / `SWFAIL` / `BAD` per bank plus a loopback
stress count (`LOOP F=0000` means no switch glitches).

### Converting cassette games

```sh
python3 tools/bas2rom.py game.cas game.rom
python3 tools/bas2rom.py game.bas game.rom
python3 tools/bas2rom.py game.bin game.rom --ml --load 2600 --run 2900
```

Handles tokenized BASIC and cassette images holding either BASIC or
machine-language programs. The BASIC loader forces the program's original
load address into TXTTAB (so embedded machine code keeps its absolute
addresses), relinks the line chain, enables the 60 Hz field-sync IRQ that
a cartridge autostart otherwise skips, and enters the RUN handler.
Programs larger than one bank are laid out for the `bank16k` scheme with
a loader that copies each bank into RAM and **verifies it** before
starting the game. The entry points it uses are identical across Color
BASIC 1.0–1.4 and Extended BASIC 1.0–2.0, so one ROM runs on a CoCo 1, 2
or 3 (Extended BASIC required).

## The videocart's HDMI output

- 640×480@60, RGB. The 256×192 VDG image is doubled to 512×384 inside
  black side borders; graphics modes take the colour-set border (green or
  buff) as the real chip does.
- All VDG modes: alphanumeric with inverse video, semigraphics 4, CG1–CG6,
  RG1–RG6, the SAM display offset and the SAM's half-height row-doubling.
- **PMODE 4 artifact colour** is modelled the way an NTSC monitor invents
  it. The phase is arbitrary on real hardware, so it is a live setting:
  if a game's colours look swapped, pick the other phase.
- **HDMI audio** (48 kHz, stereo PCM) is reconstructed from the CoCo's
  writes to the 6-bit DAC, the single-bit sound output and the sound
  gate / mux registers, with a model of the analogue output stage
  (coupling capacitor + selectable low-pass) so it sounds like the
  machine's speaker rather than the raw ladder. Turn HDMI audio off (a
  staged setting) to get plain DVI signalling if a display refuses the
  HDMI stream.
- The **Bus diagnostics** panel shows the renderer's decoded mode and
  display base, the measured audio sample rate, and every drop counter;
  `/api/screen` and `/api/mirror` let you check what was captured before
  suspecting the renderer.

## Known limitations

- **Insert applies when you change carts, not at power-on.** Power-on
  serves the last ROM immediately; select it again from the menu to get
  the insertion-style start.
- **The physical RESET button takes the reset path.** The firmware has
  no reset-detect input, so an `insert` cart restarted with the button
  behaves as it would from a reset, which some do not survive.
- **Videocart images are limited to 32 KB** (two 16 KB banks); larger
  images load truncated. The multicart serves up to 128 KB.
- **Per-scanline effects** (games that change the VDG mode many times
  per frame) cannot be shown by a whole-frame renderer; the picture
  alternates between the modes. The panel reports how often the mode
  register changes per frame.
- **Carts that switch the SAM to all-RAM (64 K) mode** are not handled
  on the videocart: it keeps serving the cartridge window, which a real
  cart would not.
- **Audio is reconstructed, not recorded.** The cartridge port carries
  no analogue audio, so what comes out is a model of the CoCo's output
  stage; it is close, not identical.

## License

Copyright 2026 Scott M Baker. Licensed under the Apache License, Version
2.0 — see the `LICENSE` file. The vendored LittleFS in
`firmware/lib/littlefs` is BSD-3-Clause, per its own headers.
