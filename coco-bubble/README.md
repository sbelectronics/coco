# CoCo Bubble Memory Cartridge

A cartridge for the TRS-80 Color Computer 1 and 2 that puts 128 KB of
Intel 7110 magnetic bubble memory behind a Disk BASIC-compatible
command set. Plug it in, type `DSKINI 0`, and `DIR`, `SAVE`, `LOAD`,
`OPEN`, `PRINT#`, `COPY` and the rest work the way they do on a disk
system - over a non-volatile solid-state medium with no moving parts.

```
BUBBLE EXTENDED COLOR BASIC 1.0
INTEL 7110 BUBBLE CARTRIDGE
BY SCOTT BAKER
BUILD 224C
MPI: BUB 3 FDC 4
128K BUBBLE READY

OK
```

The cartridge carries its own 16 KB ROM. Programs saved by it are
byte-for-byte what Disk BASIC would have written, the media format is
the standard 35-track granule layout, and a real floppy controller in a
Multi-Pak alongside it becomes drives 1 to 3 - so files move between
bubble and floppy with a single `COPY`.

## What it does

**Commands.** `DIR`, `DRIVE`, `DSKINI`, `FREE`, `KILL`, `RENAME`,
`COPY`, `VERIFY`, `UNLOAD`, `SAVE`/`LOAD` (crunched and `,A` ASCII),
`SAVEM`/`LOADM`, `RUN "file"`, `MERGE`, `OPEN`/`CLOSE`, `PRINT#`,
`WRITE#`, `INPUT#`, `LINE INPUT#`, `EOF`, `LOC`, `LOF`, `POS`, `DSKI$`,
`DSKO$`; the direct-access set `OPEN "D"`, `FIELD`, `GET`/`PUT`,
`LSET`/`RSET`, `MKN$`/`CVN`, `FILES`; plus one of its own, `BUBDIAG`.

**Compatibility.** The keyword table reproduces Disk BASIC's exactly, so
`DIR` tokenises to `$CE` and `LOAD` to `$D3` just as on a disk system,
and a program saved crunched on the bubble lists and runs unchanged
from a floppy or a `.DSK` image. The ROM header at `$C000` carries the
same `DK` signature and the same four entry vectors Disk BASIC
publishes (`DSKCON` at `$C004`, its variable block at `$C006`, init at
`$C008`, DOS at `$C00A`), and the RAM layout from `$0600` up is Disk
BASIC's, so machine-language utilities that call `DSKCON` or peek Disk
BASIC's variables find what they expect.

**Drives.** Drive 0 is the bubble. If a floppy controller is found in
the Multi-Pak at cold start, its drives are 1 to 3. `DRIVE n` sets the
default; a `:n` suffix on any filename overrides it.

**Capacity.** 512 sectors of 256 bytes are backed by the bubble. The
virtual disk is Disk BASIC's 35 × 18 geometry so standard tools accept
the format; the granules beyond the physical end are marked
unallocatable, leaving **54 granules, about 121 KB**, for files.

## Hardware

The board is two circuits on one card:

- **The bubble subsystem** - the Intel 7220-1 bubble memory controller,
  a 7110-1A one-megabit module, the 7230 current pulse generator, 7242
  dual formatter/sense amplifier, 7250 coil predriver and two 7254
  drive-transistor packs, clocked from an 8 MHz oscillator through a
  74HCT74 divider, with the 75463/7230 power-fail circuit from Intel's
  application note AP-187. This is the same circuit as the author's
  Bubble Basic single-board computer.
- **The CoCo interface** - an ATF22V10 that turns the 6809 bus into the
  8080-style chip select and read/write strobes the 7220 wants and
  decodes the two registers at `$FF40`/`$FF41`, a 74HCT245 data buffer,
  a 74HCT30 that collapses A8-A15 into a `$FFxx` page qualifier, and a
  28-pin ROM socket.

The board files are published separately. The parts list below is what
the current board is built from.

### Parts list

Interface:

| Ref | Part | Package | Notes |
|---|---|---|---|
| P1 | CoCo cartridge edge, 40 pin | card edge | |
| U20 | ATF22V10C | DIP-24 | address decode and strobes; programmed with `pld/cocobub.jed` |
| U21 | 74HCT245 | DIP-20 | data bus buffer |
| U22 | 28-pin ROM socket | DIP-28 | W27C512 default; see the strap table |
| U23 | 74HCT30 | DIP-14 | A8-A15 NAND → `$FFxx` page qualifier |
| RN2 | 10 kΩ × 4, common | SIL-5 | holds the 7220 strobes idle when the CoCo is off |
| D3 | 1N5819 | DO-41 | reset isolation (must be Schottky) |
| JP1 | 2-pin header | | Q → CART* autostart - **leave open** |
| JP4, JP5 | 3-pin headers | | ROM socket pins 1 and 27 |
| C23, C24, C35, C36 | 0.1 µF | | one per interface IC |

Power:

| Ref | Part | Notes |
|---|---|---|
| J4 | DC barrel jack | +18 V in |
| IC2 | 7812, TO-220 | +18 V → +12 V - see *Power* below before fitting a linear part |
| IC11 | 7805, TO-220 | +12 V → +5 V for the bubble subsystem |
| C51, C112, C113 | 220 µF, 100 µF, 33 µF | +18 V input |
| C101, C102, C114 | 100 µF, 33 µF, 33 µF | +12 V |
| C103, C22 | 33 µF, 0.1 µF | +5 V |
| D1, D2 | 1N4004 | reverse clamps on the 5 V rails |
| D4 | 12 V TVS, DO-15 | +12 V clamp |
| L2 | 500 µH | filter for the cart-powered +5 V option; U$2 and U$9 are alternative footprints for the same position - fit one |
| JP3 | 2-pin header | cart-powered +5 V option - **never with IC11 fitted** |
| U$3, U$6, U$14, U$17, U$18 | test points | cartridge +12 V pin, +5 V, and three grounds |

Bubble subsystem:

| Ref | Part | Package |
|---|---|---|
| U$1 | Intel 7220-1 bubble memory controller | DIP-40 |
| U5 | Intel 7110-1A bubble memory module | |
| U6 | Intel 7230 current pulse generator | DIP-22 |
| U4 | Intel 7242 dual formatter / sense amplifier | DIP-20 |
| U2 | Intel 7250 coil predriver | DIP-16 |
| U1, U3 | Intel 7254 quad drive transistor | DIP-14 |
| U8 | 75463 dual peripheral driver | DIP-8 |
| IC3 | 74HCT74 | DIP-14 |
| QG1 | 8 MHz oscillator module | DIP-8 can |
| Q1 | VN0106 (2N3819 footprint) | TO-92 |
| CR1, CR2 | 1N914 / 1N4148 | DO-35 |
| LED1, LED2 | 5 mm LED | +12 V and +5 V indicators |
| RN1 | 1 kΩ × 4 isolated | SIL-8 |
| JP2 | 2-pin header | 7110 SWAP select |
| SJ1-SJ4 | solder jumpers | tie the unused half of IC3 |
| R1, R17 | 1 kΩ | R17 is the reset pull-up - **must be 1 kΩ**, see below |
| R2, R12, R13, R19 | 5.1 kΩ | |
| R3, R4 | 5.1 Ω | |
| R5 | 2.2 kΩ | |
| R7 | 10 Ω | |
| R8 | 3.9 kΩ | |
| R9 | 3.48 kΩ | |
| R10 | 33 kΩ | |
| R11 | 47 Ω | |
| R18 | 5.6 kΩ | |
| C2, C4, C7, C9-C12, C28-C30, C34, C37, C39 | 0.1 µF | 13 pieces |
| C1, C3, C8, C13, C17, C19, C26 | 15 µF electrolytic | 7 pieces |
| C15, C16, C27, C33 | 47 µF electrolytic | |
| C42, C43 | 470 µF electrolytic, axial | supply hold-up, +5 V and +12 V |
| C25, C31, C32, C41 | 1 µF | |
| C5, C6 | 120 pF | |
| C14 | 2.2 µF | |
| C18 | 8.2 µF | |
| C20, C21 | 0.01 µF | |
| U$4 | test point | +5 V |

### Assembly notes

**ROM straps.** JP4 and JP5 are 3-pin headers with +5 V on pin 1, the
ROM socket pin in the centre and ground on pin 3. A shunt selects a
rail; no shunt leaves the pin open.

| Device | Size | JP4 (socket pin 1) | JP5 (pin 27) | Notes |
|---|---|---|---|---|
| W27C512 / 27C512 | 64 KB | +5 V or GND (A15) | +5 V or GND (A14) | **Default.** Four 16 KB images, strap-selected; GND/GND is the image at offset 0 |
| 27C256 | 32 KB | +5 V (VPP) | +5 V or GND (A14) | two strap-selected images |
| 27C128 | 16 KB | +5 V (VPP) | +5 V (PGM*) | |
| 27C64 | 8 KB | +5 V (VPP) | +5 V (PGM*) | **too small for the current ROM** - the socket takes these parts, and an 8 KB image mirrors at `$E000`, but the ROM is now larger than 8 KB |
| 28C64 | 8 KB | **open** (RDY/BUSY) | +5 V (WE*) | as above. The RDY/BUSY pin is open-drain on some vendors, which is why JP4 must be open and never +5 V for this part |

The W27C512 is electrically erasable in the programmer and holds four
images, which is a convenient way to carry more than one firmware
version. Burn `build/bubble.rom` (exactly 16384 bytes) into the bank
the straps select. A 16 KB part is now the minimum: the ROM outgrew
8 KB, so the 27C64 and 28C64 rows are there for the socket's sake
rather than as a usable option.

The 28-pin socket has two traps: the data outputs run 11, 12, 13 and
then skip ground at 14 to resume at 15, and A11 (pin 23) sits between
A9 (24) and A10 (21). Check against a datasheet, not the pattern.

**The PLD.** Program the ATF22V10 with `pld/cocobub.jed`. In XGPRO /
MiniPro, select the device as **ATF22V10C(UES)** - the UES variant
matters. `pld/cocobub.pld` is the WinCUPL source.

**Reset.** D3 lets the CoCo's RESET* pull the bubble subsystem's reset
net low while preventing the bubble's power-fail detector from resetting
the CoCo. It must be a Schottky part: the 7220 accepts only 0.8 V as a
low, and a silicon diode's forward drop puts the level over that. Anode
to the bubble side, cathode to the cartridge edge. R17, the pull-up on
that net, must be 1 kΩ - the 7230's open-collector output can leak up
to 1 mA, and a higher value lets that leakage hold the bubble in
permanent reset on a part that leaks toward its limit.

**JP1 stays open.** The ROM starts through Color BASIC's `DK` cold-start
hook, exactly as Disk BASIC does; JP1 is a fallback that asserts CART*
from Q and is not needed.

### Power

The bubble subsystem needs its own supply. Intel's figures for this
configuration (one 7110 with its support parts) are **384 mA at +5 V
and 400 mA at +12 V** maximum, 6.7 W total, and +12 V must stay within
±5 %. Neither the cartridge port nor a Multi-Pak can provide that: the
MPI's entire +12 V rating is 400 mA across all four slots, with no
margin for regulation, ripple, or anything else plugged in. The
interface logic (about 50 mA) runs from the cartridge port's 5 V; the
bubble does not.

Feeding +18 V into J4 and fitting IC2 and IC11 as ordinary linear
regulators works electrically but dissipates about 7.4 W inside the
cartridge shell - too much for bare TO-220 packages. Either fit
switching drop-in regulators in the TO-220 footprints, or supply a
regulated +12 V and omit IC2 (the ±5 % is met by any decent regulated
adapter, and that removes the larger dissipation).

JP3 is for one specific arrangement: bubble +5 V from the cartridge
port (434 mA of the MPI's 1.35 A is comfortable) and only +12 V from
the external supply, with IC11 removed. **Never fit JP3 with IC11 in
place** - that ties the regulator output to the CoCo's 5 V rail.

Intel specifies that the supplies must not decay faster than 1.1 V/ms
on +12 V and 0.45 V/ms on +5 V at power-off, or data may be lost. C42
and C43 (470 µF each) are the hold-up for that; at the maximum
currents +5 V would want closer to 850 µF, so the decay rate is worth
checking with a scope on a finished board.

## Building the ROM

Requires [lwtools](http://www.lwtools.ca/) (`lwasm`) and Python 3.

```
cd software
make            # build/bubble.rom, the cartridge image, and
                # build/bubble-mock.rom for the emulator tests
make help
```

`bubble.rom` is padded to 16384 bytes so it can be burned straight into
a 16 KB bank. The Makefile checksums the sources into a build stamp
that the boot screen prints as `BUILD xxxx`, so the image in the socket
can always be identified. If `lwasm` is not on your path,
`make LWASM=/path/to/lwasm`.

## Installing

**Directly in the cartridge port.** Connect the external supply, plug
the cartridge in, and power on. The banner reports `NO MPI` and
`128K BUBBLE READY`.

**In a Multi-Pak Interface.** Set the MPI's selector switch to the slot
holding the ROM. At cold start the ROM detects the Multi-Pak and scans
the slots from the selected one **upward** to slot 4, looking for the
bubble and for a floppy controller, and routes the SCS* select line to
each device as it is used. The banner reports what it found, for
example `MPI: BUB 3 FDC 4`.

Because the scan runs upward, a floppy controller must be in a
**higher-numbered slot** than the selector switch. The conventional
arrangement - floppy controller in slot 4, bubble in slot 3, switch on
3 - works. A CoCo SDC presents the same controller registers and should
be detected the same way, but this has not been tried on hardware.

The first bubble is not formatted for use until you type

```
DSKINI 0
```

which writes an empty allocation table and directory. On a floppy,
`DSKINI n` does the same to a disk that is already formatted; it does
not format tracks - use Disk BASIC for that.

## Using it

Filenames are `NAME.EXT:drive`, exactly as in Disk BASIC: up to eight
characters, an optional three-character extension after `.` or `/`,
and an optional drive number. `SAVE` and `LOAD` default the extension
to `BAS`, `SAVEM`/`LOADM` to `BIN`, `OPEN` to `DAT`; `COPY`, `KILL` and
`RENAME` take the name exactly as given and need the extension spelled
out.

```
DSKINI 0                        prepare the bubble
SAVE "GAME"                     crunched, to the default drive
SAVE "GAME",A                   as an ASCII listing
LOAD "GAME:1",R                 from floppy drive 1, and run it
COPY "GAME.BAS:1" TO "GAME.BAS:0"
DIR 1
KILL "OLD.DAT:0"
PRINT FREE(0)
DRIVE 1                         make the floppy the default
```

Sequential files work as in Disk BASIC: `OPEN "O",#1,"LOG"`,
`PRINT#1,...`, `WRITE#1,...`, `CLOSE#1`; `OPEN "I",#1,"LOG"`,
`INPUT#1,...`, `LINE INPUT#1,...`, `EOF(1)`, `LOC(1)`, `LOF(1)`.
`DSKI$` and `DSKO$` read and write raw sectors of the virtual 35 × 18
geometry as two 128-character strings.

`COPY` moves a file between any two drives, a sector at a time, so the
size of the file does not matter and it needs no free memory. (Disk
BASIC buffers as many whole sectors as fit below the stack and reports
`?OM` for anything larger.) Copying a file onto itself is `?AE`.

### BUBDIAG

The ROM includes a diagnostic that exercises the bubble hardware
through the same driver code the rest of the ROM uses:

```
BUBDIAG        read-only tests: bus presence, data bus, register
               file, abort, FIFO reset, controller init, status
               decode, sector reads
BUBDIAG 1      adds a destructive write/verify of four spot sectors
BUBDIAG 2      adds a destructive write/verify of every sector
```

**The destructive levels overwrite the allocation table and directory
with the rest of the surface.** Run `DSKINI 0` afterwards.

Failures decode into English rather than a bare code:

```
T8 WRITE/VERIFY .. FAIL E8
  DATA MISMATCH
  SECTOR 0 OFFSET 02
  WANT 59 GOT 58
```

If the banner says `BUBBLE NOT READY E=xx`, the two-digit code names
the step of controller initialisation that failed; `BUBDIAG` will
usually show where. `PEEK(252)` returns the most recent failure code.

### Direct-access files

Records of a fixed length, readable and writable in any order, as in
Disk BASIC. `OPEN "D"` (or `"R"`) takes the record length; `FIELD`
divides the record into string variables; `LSET` and `RSET` write them;
`PUT` and `GET` move whole records to and from the bubble.

```basic
10 OPEN "D",#1,"PHONE",32
20 FIELD #1,20 AS N$,12 AS P$
30 LSET N$="ADA LOVELACE"
40 LSET P$="01234 567890"
50 PUT #1,1
60 GET #1,1
70 PRINT N$;" ";P$
80 CLOSE #1
```

A `FIELD`ed variable *is* part of the record buffer rather than a copy
of it, so a `GET` changes what it holds with nothing to copy. Two
consequences follow, both the same as Disk BASIC:

- Write one with `LSET` or `RSET`, never with plain assignment.
  `N$ = "X"` points the variable back at ordinary string space and
  quietly disconnects it from the record.
- Assigning *from* one, as in `A$ = N$`, does make a proper copy, so
  `A$` keeps its value when the next `GET` arrives.

The file is created if it does not exist, and opening one that does
leaves its contents alone. A record number runs from 1; leaving it out
uses the next one, so a bare `GET #1` walks the file. `PUT` past the end
extends the file, `GET` past it is `?IE`. Numbers go into a record with
`MKN$` and come back with `CVN`. `PRINT#` and `INPUT#` also work, within
the current record: `PRINT#` fills the record from its start, `PUT`
sends it, and after a `GET` `INPUT#` reads it back. `LOC(n)` is the
record the file is at and `LOF(n)` the number of records in it.

`FILES n,m` changes how many files may be open at once (up to 15) and
how much room the record buffers get; a record longer than 256 bytes
needs the second argument. Everything above the buffers moves to make
room, so a program that issues `FILES` survives it, but its variables
are cleared and anything that depends on where memory starts will
disagree until the next reset - as in Disk BASIC.

### Errors

Errors use Disk BASIC's vocabulary: `?NE` file not found, `?AO` file
already open, `?DF` disk full, `?FS` bad file structure, `?WP` write
protected (also a write into the unbacked part of the virtual disk),
`?IO` a transfer that failed after retries, `?VF` a sector that did not
read back as written, `?DN` a drive that is not there, `?AE` a file that
already exists.

Direct-access files add a few more: `?OB` the record buffers have no
room for another record, `?BR` a record number of zero or one too far
out to address, `?FO` the fields add up to more than the record, `?SE`
an `LSET` or `RSET` on a variable that was never `FIELD`ed, `?ER` a
`PRINT#` past the end of the record, and `?IE` a `GET` past the end of
the file.

### VERIFY

`VERIFY ON` reads every sector back after writing it and compares, which
is the state the cartridge starts in, as on a disk system. A sector that
does not match is written again, and after three attempts the operation
reports `?VF`. `VERIFY OFF` turns the read-back off, which roughly
halves the time a write takes.

## Limitations

- **`BACKUP` and `DOS` are not implemented** and report `?FC`.
  `DSKINI` takes no formatting or skip-factor arguments: there is
  nothing to format on a bubble.
- **`COPY` refuses to copy a file onto itself** with `?AE`, because it
  clears the destination before reading the source.
- **A floppy's motor stays on** from the first access until the machine
  is reset. Disk BASIC counts it down from its 60 Hz interrupt handler;
  this ROM leaves the interrupt to Extended BASIC and runs no timer.
- **`TIMER` loses ticks during bubble transfers**, which run with
  interrupts masked - the same trade-off as Disk BASIC's halted-CPU
  transfers.
- **A bubble transfer forces the CPU to normal speed and leaves it
  there.** The speed-up poke cannot be restored because the SAM's rate
  bits are write-only.
- `SAVE "X",A` clears the program's variables when it completes, as it
  does in Disk BASIC.
- `DSKI$`/`DSKO$` strings are clipped at 128 characters.
- Verified on Color BASIC 1.3 with Extended BASIC 1.1. Color BASIC 1.2
  and Extended BASIC 1.0 use the same published entry points and are
  expected to work. The CoCo 3 is untested.

## Testing

`software/test/` holds an emulator regression suite that runs the ROM
under XRoar with a RAM-backed stand-in for the bubble, a media-format
check against ToolShed's `decb`, and four BASIC programs for the real
hardware:

| | |
|---|---|
| `BASTEST.BAS` | Color and Extended BASIC itself - expressions, `&H` and `&O`, strings, control flow, arrays, `TIMER`, and the graphics `GET`/`PUT` and `CLS` whose vectors this ROM hooks. Touches no files. |
| `BUBTEST.BAS` | the sequential command set, across the bubble and a floppy |
| `BUBRAND.BAS` | direct-access files: `OPEN "D"`, `FIELD`, `GET`/`PUT`, `LSET`/`RSET`, `MKN$`/`CVN`, `LOC`/`LOF` |
| `BUBALT.BAS` | the same ground as the other two, checked differently - by checksum, by reading the directory and allocation table raw, and by reading records back in reverse |

Each prints a pass or fail line per test and a count at the end; the
disk ones create only `ZZ*` files, kill every one, and never format.
`software/README.md` covers the test environment.

## Documentation

- `HOW-IT-WORKS.md` - how the hardware and the ROM work, in detail.
- `software/README.md` - building and testing the ROM.

## License

Apache License 2.0. See `LICENSE`.
