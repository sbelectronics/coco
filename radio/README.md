# CoCo AM/FM Radio Cartridge

An AM/FM radio for the TRS-80 Color Computer, in a cartridge. Plug it in,
power on, and the CoCo boots straight into a radio player: big frequency
digits, twelve named presets that survive power-off, seek, stereo and
signal indicators, and audio played through the computer's own TV or
monitor sound.

By Dr. Scott M Baker, <https://www.smbaker.com/>.

The receiver is an NXP TEF6686 car-radio tuner module. The cartridge
around it is deliberately small: an EEPROM for the program and presets, a
22V10 PLD for address decoding, a latch and a buffer for the control
interface, and a dual op-amp for audio. The 6809 talks to the tuner over
I2C, bit-banged through the latch.

- FM 87.5-108 MHz, stereo, US (200 kHz) and European (100 kHz) channel
  steps
- AM 530-1620 kHz (US, 10 kHz) or 531-1602 kHz (Europe, 9 kHz)
- 12 presets with 7-character names, kept in the cartridge EEPROM
- Seek up and down, direct frequency entry, volume, mute, forced mono
- Works on a CoCo 1, 2 or 3, directly or in a Multi-Pak
- Runs in 16K of RAM

How it works, from the address decode up to the user interface, is in
[HOW-IT-WORKS.md](HOW-IT-WORKS.md).

## Hardware

The schematic and fabrication files are in [`hardware/`](hardware/)
*(to be added)*.

| Part | Function |
|---|---|
| TEF6686 tuner module (TDQ-230V-186, XFA-230V-186, XD-6686AF-5 and similar) | AM/FM receiver, I2C, +5 V |
| AT28C256 EEPROM (**must be the 32K part**, see below) | Program, firmware patch and presets |
| ATF22V10C (or GAL22V10) | Address decode, programmed from [`pld/decode/DECODE.jed`](pld/decode/DECODE.jed) |
| 74HCT273 | Control latch at `$FF5E`: I2C lines and EEPROM write enable |
| 74HCT244 | Status port at `$FF5F`: I2C data read-back and board ID |
| LM358 | Audio summer and output buffer to the cartridge SND line |

The module mounts flat on the board on a 2.0 mm pitch row (it is not a
0.1" header). Antenna input is a 3.5 mm jack shared by AM and FM.

**Jumpers.** JP2 (CART, fitted) makes the cartridge autostart; remove it
to come up in BASIC instead. JP1 (SND bias, open) adds a DC bias to the
sound line for a computer whose sound input is not biased; the CoCo 2 and
3 bias it internally, so leave it open on those. The solder jumpers set
the board ID the ROM checks (`$B0`); leave them at the default pattern.

**Debug header.** Six pins: GND, SCL, SDA, /EEWR, /CTLWR, /SLENB.

## Building the ROM

You need:

- The [Macroassembler AS](http://john.ccac.rwth-aachen.de:8000/as/)
  (`asl` and `p2bin`) on your PATH, or copied into `tools/bin/`.
  `make ASL=... P2BIN=...` also works.
- GNU make. On Windows, `mingw32-make` from a Git Bash shell works.
- Python 2.7 or 3.
- Network access the first time, to fetch the tuner firmware patch (see
  below), or a local copy of NXP's `PATCH.c`.

```
make            # build/radio.bin, the cartridge image
make dev        # build/radio-dev.bin, Multi-Pak development image
make emu        # build/radio-emu.bin, for UI work in an emulator
make clean
```

Each build stamps a four-digit build ID that appears on the main screen,
the test screen and the About page, so you can always tell which image a
chip holds. `tools/checkrom.py` runs after every build and refuses an
image with a bad header, a broken default preset store, or the wrong
size.

### The tuner firmware patch

The TEF6686 does not work out of the box: at every power-up the host must
load a firmware patch into it, or it sits in boot state and never tunes.
The patch is NXP/Catena copyrighted and **is not distributed with this
project**. `make` runs [`tools/mkpatch.py`](tools/mkpatch.py), which takes
it from a `PATCH.c` in the current directory if there is one (NXP ships it
with the TEF668X GUI program and prints it in the User Manual), and
otherwise downloads the same bytes from the public PE5PVB TEF6686_ESP32
driver source. It writes `rom/tefpatch.bin` and `rom/tefpatch.inc`, which
are gitignored. Do not commit them or distribute built images.

`make TEFPATCH=0` builds without the patch. That image runs the digital
bring-up and the test screen, but the radio will not tune.

## Programming the chips

**EEPROM.** Program `build/radio.bin` (16,128 bytes) into an AT28C256 at
offset 0. Then **turn off Software Data Protection** in the programmer as
the last step (in XGpro: the AT28C256's unlock option). The board grounds
A14, so the ROM can never send the SDP unlock sequence itself, and with
SDP on every preset save fails.

The chip must be the 32K AT28C256. An 8K AT28C64 fits the socket and
reads back as a working ROM, but it cannot hold the firmware patch; the
ROM detects this at start-up and shows `8K EEPROM FITTED - NO PATCH`.

**PLD.** Program `pld/decode/DECODE.jed` into an ATF22V10C (or GAL22V10)
**before the board is first powered**. Without it the EEPROM's control
lines float. The source is [`pld/decode/DECODE.PLD`](pld/decode/DECODE.PLD)
(WinCUPL).

## Using it

Insert the cartridge with the power off and turn the CoCo on. On a cold
start the screen shows `LOADING TUNER FIRMWARE` for about a second and a
half, then the main screen.

| Key | Action |
|---|---|
| Left / Right | Tune one channel down / up |
| Shift + Left / Right | Seek down / up (any key stops it) |
| Up / Down | Volume |
| 1-9, 0 | Recall presets 1-10 |
| Shift+1, Shift+2 | Recall presets 11 and 12 (shown as S1, S2) |
| W, then a preset key | Store the current station |
| N, then a preset key | Name a preset (7 characters, ENTER to save, BREAK to cancel) |
| F | Type a frequency on the current band: `973` for 97.3 MHz on FM, `1010` for 1010 kHz on AM |
| B | Switch between AM and FM |
| R | Switch between US and European channel spacing |
| M | Mute |
| S | Forced mono |
| H | Help |
| A | About (version and build ID) |
| T | Hardware test screen |
| BREAK | Return to BASIC with the radio still playing |

The current station, volume, band, region and mono setting are saved to
the EEPROM five seconds after the last change, and when you press BREAK.

### Hardware test screen

`T` opens a diagnostic screen showing the status port, the latch shadow,
the last I2C error, the signal level, the last EEPROM result and the
tuner bring-up code. BREAK returns to the radio.

| Key | Test |
|---|---|
| 0 / 1 | Toggle SCL / SDA (watch them on the debug header) |
| I | Probe the tuner and read its identification |
| P | Re-run the tuner bring-up (loads the patch if the module is in boot state) |
| L | Read signal level and stereo once |
| A | Write and verify the 30-byte scratch area of the EEPROM. Row 6 shows `00` on success, `02` if the chip never accepts the write (usually Software Data Protection still on) |
| E | Rewrite the preset header page in place |
| D | Draw the big-digit font |
| W | Live view of the tuner's level, noise, AGC and soft-mute registers |

## Multi-Pak development build

`make dev` builds an image meant to run from a multi-ROM cartridge in one
Multi-Pak slot while the radio cartridge sits in another (slot 4 by
default; `make dev DEVSLOT=2` to change it). The ROM leaves the ROM slot
where it is and points only the I/O slot at the radio. Because EEPROM
reads would come from the other cartridge, presets are kept in RAM only
in this build, and only the test screen's `A` exercises the radio's
EEPROM. The About page shows `DEV BUILD`. Do not program this image into
the radio cartridge.

## Known issues

**Audio "motorboating" on the first board revision.** On the first boards
the output-level trimmer's cold end returns to the op-amp's 2.5 V bias
rail. That rail is only 5 kΩ against 10 µF, so the trimmer feeds the
summer's output back into its own reference, and the stage oscillates at
1-3 Hz: audio ramps up and cuts out about once or twice a second. The fix
is to return the trimmer's cold end (RT1) to ground instead of the bias
rail: cut the trace from RT1 to the bias rail and wire that end of the
trimmer to ground.

**FM sounds dull: do not fit C11.** The 3.3 nF capacitor across the
summer's feedback resistor was sized as the FM de-emphasis network, but
the TEF6686 does its own de-emphasis, and the ROM sets it to the regional
standard (75 µs for US, 50 µs for European spacing) whenever you change
region. With C11 fitted, FM is de-emphasised twice and loses its treble.
Leave C11 out, or remove it from a built board.

## License

Copyright 2026 Dr. Scott M Baker, <https://www.smbaker.com/>.

Licensed under the [Apache License, Version 2.0](LICENSE); see also
[NOTICE](NOTICE).

The TEF6686 firmware patch is copyright Catena / NXP Semiconductors. It is
not part of this project and is not covered by this license. Built ROM
images contain it, so do not distribute them. The tuner command set
follows the NXP TEF668X User Manual.
