# How It Works

This is a tour of the radio cartridge from the cartridge port up: the
address decode, the two cartridge registers, the I2C bus to the tuner, the
EEPROM and how it rewrites itself, the tuner's start-up sequence, the
preset store, the user interface and the audio path. Addresses are 6809
CPU addresses unless they say "chip".

## The big picture

```
          CoCo cartridge port
  ┌──────────────┬──────────────┬──────────────────┐
  │ CTS* reads   │ SCS* $FF40-  │ armed writes     │ SND (pin 35)
  ▼              ▼              ▼                  ▲
 ATF22V10C DECODE ──────────► AT28C256 EEPROM      │
  │       │                  (program, patch,      │
  ▼       ▼                   presets)             │
 '273    '244                                     LM358
 latch   status                                   summer + buffer
  │ SCL, SDA ──────────► TEF6686 tuner ── L, R ────┘
  └ ARM (write enable)    module
```

The 6809 runs the player straight out of the cartridge EEPROM. It talks to
the tuner over I2C, which it bit-bangs through two bits of a write-only
latch and reads back through one bit of a read-only buffer. Audio from the
tuner is summed to mono and sent to the CoCo's cartridge sound input,
which the computer routes to the TV or monitor.

There is no RF on the board. The receiver, its synthesiser, the filters
and the varicap supply are all inside the shielded tuner module.

## Memory map

The EEPROM is an AT28C256 (32K) with its A14 pin grounded, so the CPU sees
the lower 16K of the chip through the cartridge window.

| Address | Access | Contents |
|---|---|---|
| `$C000-$DEFF` | read | Player code (chip `$0000-$1EFF`) |
| `$DF00-$DFFF` | read | Preset store, four 64-byte pages (chip `$1F00-$1FFF`) |
| `$C000-$DFFF` | write | EEPROM write window, only while armed |
| `$E000-$FEFF` | read | Tuner firmware patch (chip `$2000-$3EFF`) |
| `$FF40-$FF5D` | read/write | 30-byte scratch area of the EEPROM (chip `$3F40-$3F5D`) |
| `$FF5E` | write | Control latch |
| `$FF5F` | read | Status port |

The cartridge window is 16,128 bytes, not 16K: chip bytes `$3F00-$3FFF`
would appear at `$FF00-$FFFF`, which is the CoCo's I/O page. The image
therefore ends at `$FEFF`.

The scratch area is a quirk worth knowing. The `$FF40` range is the
cartridge's SCS* select, and A13 is high there, so these addresses reach
chip `$3F40-$3F5D`: thirty bytes that nothing else can address. The
hardware test uses it to prove that the EEPROM can be written, without
any risk to the presets.

## Address decode

All the glue logic is one ATF22V10C, purely combinational. Its source is
[`pld/decode/DECODE.PLD`](pld/decode/DECODE.PLD).

- **ROM reads** use the CoCo's CTS* select, which the SAM asserts only for
  reads of `$C000-$FEFF`.
- **Register access** is SCS* with A4-A1 all high (`$FF5E`/`$FF5F`): a
  write clocks the latch, a read enables the status buffer.
- **EEPROM writes** get no help from the SAM, which never asserts CTS* on
  a write. The PLD decodes them itself: E high, a write, A15-A13 = `110`,
  and the ARM bit from the latch. Without ARM nothing can write the chip:
  not BASIC copying ROM to RAM, not OS-9 in all-RAM mode, not a stray
  store.
- **SLENB*.** Inside a Multi-Pak, a write to `$C000` never reaches a slot,
  because the Multi-Pak opens its data buffer only for CTS* reads, the
  `$FF40-$FF7F` range, or SLENB*. During an armed `$C000-$DFFF` write the
  PLD pulls SLENB* low (it drives low or floats, never high), which opens
  the buffer.
- **Autostart.** The CART* output carries the Q clock, the standard way a
  CoCo cartridge tells Color BASIC to jump to `$C000` at power-up.

Every strobe is ANDed with E, so the latch captures data at the falling
edge of E while write data is still valid, and the status buffer never
drives the bus in the half-cycle when the CoCo's own RAM buffer may be on.

## The two registers

**Control latch, `$FF5E`, write only.** A 74HCT273, cleared by RESET*.

| Bit | Name | Meaning |
|---|---|---|
| 0 | SCL | I2C clock: 1 = released, 0 = driven low |
| 1 | SDA | I2C data out: 1 = released, 0 = driven low |
| 7 | ARM | EEPROM write enable |

The latch cannot be read back, so the ROM keeps a shadow copy in RAM and
always writes the whole byte. ARM is never set in the shadow: only the
EEPROM save routine sets it, writes, and clears it again.

**Status port, `$FF5F`, read only.** A 74HCT244.

| Bit | Meaning |
|---|---|
| 0 | I2C SDA, read at the tuner's pin |
| 1-3 | Always 1 |
| 4-7 | Board ID, `1011` = `$B0`, set by solder jumpers |

At start-up the ROM checks the ID nibble. `$FF` means nothing answered
(no cartridge, or the wrong Multi-Pak slot); any other wrong value usually
means an open solder jumper, which leaves an input floating, so the ROM
prints the raw byte.

## I2C to the tuner

The latch outputs are push-pull, which is not how an I2C master should
drive a bus. A 1 kΩ resistor in series with each line, with 4.7 kΩ
pull-ups at the module, makes it safe: "releasing" a line means writing a
1, and if the module is holding that line low the resistor limits the
current. This is the arrangement the module's documentation asks for.

Each bus edge is a single 6809 store, about 5.6 µs, so the bus runs at
roughly 40 kHz with no delay loops. The ROM never assumes a line is high
because it wrote a 1; it reads SDA back through the status port for every
ACK and every received bit. SCL is not read back, so clock stretching
could not be detected; the TEF6686 does not stretch at this speed.

The tuner answers at address `$C8` (or `$CA`, depending on a pin strap
inside the module); the ROM probes both.

## Talking to the TEF6686

The tuner follows NXP's TEF668X User Manual. Every command is a frame of
*module, command, index*, then 16-bit parameters, most significant byte
first. The modules are FM (`$20`), AM (`$21`), AUDIO (`$30`) and APPL
(`$40`); the index is always 1.

### Start-up

At power-up the module is in **boot state**. It acknowledges its address
and answers every read with zeros, which makes it look alive but useless.
It needs its firmware patch before it does anything:

1. Probe the address. A module that acknowledges proves the bus, the
   resistors, the pull-ups and the latch wiring.
2. Read the operation status. 0 means boot state.
3. Load the patch: about 6 KB of patch code and a 118-byte lookup table,
   each sent in 24-byte frames after a target-select write. At 40 kHz
   this takes about a second and a half, so the screen shows a progress
   bar.
4. Send the start command and wait for the idle state.
5. Activate, and wait for the active state.
6. Unmute. The module powers up muted, so silence after a perfect-looking
   tune is almost always this.
7. Tune with mode **Preset**. Only Preset and Search take the radio out of
   standby and allow a band change, so the first tune must be one of
   them.

The patch is volatile: it is lost whenever the module loses power. If a
Multi-Pak has kept the module powered across a CoCo reset, the module is
already past boot state and the ROM skips straight to activation.

The firmware patch lives at `$E000`, which an 8K AT28C64 does not have.
On such a chip `$E000` reads back the same bytes as `$C000`, so the ROM
compares the first 32 bytes of each; if they match, it refuses to start
the tuner and says so.

### Tuning, volume and quality

- **Tune:** Tune_To with the frequency in 10 kHz units for FM and 1 kHz
  for AM. Internally the ROM keeps FM in 50 kHz units and multiplies by
  five.
- **Volume:** Set_Volume takes a signed value in 0.1 dB. The player's
  0-99 maps linearly onto -40 dB to -4 dB, a ceiling chosen so the audio
  stage cannot clip.
- **De-emphasis:** Set_Deemphasis is sent at start-up and on every region
  change: 75 µs with US channel spacing, 50 µs with European. The module
  powers up at 50 µs.
- **Quality:** Get_Quality_Status returns seven words: a status whose low
  ten bits are a time stamp (0 means "still tuning, ignore the rest"),
  signal level, ultrasonic noise, multipath, frequency offset, bandwidth
  and modulation. **Stereo** is a separate command, Get_Signal_Status.
  The main screen reads both four times a second for the STEREO and TUNED
  indicators.

### Seek

The TEF6686 has **no seek command**. Tune mode 2 is named "Search", but it
only means "tune and stay muted"; deciding whether a channel has a
station is the host's job. The ROM's seek is a loop: step one channel,
tune with Search, wait for the quality time stamp to settle, then accept
the channel if the level is high enough and, on FM, the noise and the
frequency offset are low. Otherwise step again, until it finds a station
or arrives back where it started. A final Tune_To with mode End releases
the mute. Any new key press aborts the seek.

## The EEPROM writes itself

The preset store lives in the same EEPROM the program runs from, and an
EEPROM cannot be read while it is busy writing: during its internal write
cycle every read returns the last byte written with bit 7 inverted. Code
running from the chip during a write would execute garbage. So:

- The save routine is copied to RAM at `$0600` at start-up, and the write
  runs entirely from RAM with interrupts masked.
- It sets ARM, stores up to one 64-byte page (one byte every 19 µs, well
  inside the chip's 150 µs page-load window), and clears ARM.
- It waits about 500 µs so the internal write cycle has really started,
  then polls the last address written until the data reads back
  correctly, then verifies every byte.
- It refuses any destination outside `$DF00-$DFFF` and the scratch area,
  and any range that crosses a 64-byte page, because an armed write to
  the wrong address would overwrite the ROM's own code.

The AT28C256's **Software Data Protection** must be turned off when the
chip is programmed. Its unlock sequence needs chip address `$5555`, which
has A14 set, and A14 is grounded on this board, so the ROM can never send
it. With SDP on, the chip silently ignores every write and the save
reports a timeout.

## The preset store

256 bytes at `$DF00`, as four 64-byte pages, so a save never writes more
than one page:

- **Header page:** the magic bytes `R` `D`, a format version, flags
  (region, mono), the current station, the volume, and a checksum.
- **Twelve 16-byte records:** a station word, an 8-byte name field (7
  characters shown), five reserved bytes and a checksum.

Every block's checksum makes the block sum to zero. A bad header loads
the defaults; a bad record simply shows as empty. A freshly programmed
chip already contains a valid default store, built and checksummed by the
assembler.

The station word holds the band in bit 15 (1 = FM) and the frequency in
the low 15 bits: 50 kHz units for FM, kHz for AM.

All edits happen in a RAM copy. A preset is written as soon as you store
or name it. The header is written five seconds after the last change of
station, volume, band, region or mono, so tuning across the band costs
one EEPROM write rather than one per step. It is also written when you
exit to BASIC.

## The user interface

Interrupts stay masked the whole time the ROM runs. The CoCo's CART*
line carries the Q clock, so once the cartridge is running the FIRQ line
toggles constantly; on exit to BASIC the ROM disables that interrupt and
installs a small RAM handler as a second line of defence.

The main loop is paced by the VDG's 60 Hz vertical sync flag, polled
rather than taken as an interrupt. Each tick it scans the keyboard
(two-scan debounce, auto-repeat on the arrow keys only), handles the key
for the current screen, refreshes the indicators every fifteenth tick,
saves the header when it has settled, and expires messages after three
seconds.

### Display

The screen is the standard 32 x 16 text page at `$0400`. The big
frequency digits are drawn from a 3 x 5 font using semigraphics cells. In
the VDG's semigraphics mode a lit quadrant shows the cell's colour and
colour 0 is green, the same as the text background, so a fully lit cell
is invisible; the digits use **unlit** cells, which are solid black. The
volume meter uses cells with only the top-left quadrant unlit, giving
separated segments that stay clear of the black screen border.

The indicators sit in two columns beside the digits: band, STEREO and
TUNED on the left, MUTE and MONO on the right above the unit (MHZ or
KHZ). The indicators are in inverse video and the unit in plain text, and
every field is written in place at a fixed width so the four-times-a-
second refresh does not flicker.

### Exit to BASIC

BREAK saves the header if needed, restores what BASIC expects of the PIAs
and warm-starts BASIC. The tuner keeps playing: it needs no host once it
is tuned, and the CoCo's sound selector stays on the cartridge.

## The Multi-Pak

The Multi-Pak's slot register at `$FF7F` holds the ROM (CTS*) slot in
bits 5-4 and the I/O (SCS*) slot in bits 1-0. At start-up the ROM copies
its own ROM slot into the I/O field, so the radio's registers follow the
radio cartridge whichever slot it is in, even with a disk controller
elsewhere. On a CoCo without a Multi-Pak the write goes nowhere.

The `make dev` build is different: it runs from a multi-ROM cartridge in
another slot, so it leaves the ROM slot alone, forces only the I/O slot
to the radio, and restores the original value on exit. Because ROM reads
then come from the other cartridge, a preset save would write one chip
and verify against another, so that build keeps presets in RAM.

## Audio

The tuner's left and right outputs are AC-coupled into an LM358 inverting
summer (10 kΩ inputs, 22 kΩ feedback, gain 2.2) biased at 2.5 V. A trimmer
sets the output level once, and the second half of the LM358 buffers it
into the cartridge SND pin through a 1 µF non-polarised capacitor and a
100 Ω resistor. Set the trimmer for about 1.5 Vpp at SND on a strong
station; the CoCo's limit is 3.9 Vpp.

On the CoCo 2 and 3, SND goes straight into the SC77526 analogue chip,
which biases the input internally, so the cartridge's optional bias
jumper stays open.

FM de-emphasis is done inside the TEF6686, set by the ROM to match the
region. The board position C11 (3.3 nF across the feedback resistor) was
meant to do the same job and should be left unfitted; see the known
issues in the [README](README.md).

## Hardware test screen

`T` opens a diagnostic screen built for bring-up, listed in the
[README](README.md). Its rows show the raw status byte, the latch shadow,
the last I2C error, the signal level, the last EEPROM result, and the
tuner bring-up code with a patch-loaded flag. Its keys exercise each
piece in isolation: the I2C lines one at a time, the tuner probe and
identification, the full tuner bring-up, the scratch-area EEPROM write
(which needs no SLENB*, so it works even where the `$DF00` path does
not), and a live view of the tuner's gain and soft-mute registers.
