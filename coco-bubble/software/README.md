# Bubble cartridge ROM

The 16 KB cartridge ROM: a Disk BASIC-compatible command set for Color
BASIC, backed by the Intel 7220/7110 bubble subsystem, with a Tandy
floppy controller in a Multi-Pak as drives 1-3. Written for lwasm.
`../README.md` covers installing and using the cartridge;
`../HOW-IT-WORKS.md` explains the design.

## Building

Requires lwasm (lwtools) and Python 3.

```
make            # both images: build/bubble.rom for the cartridge and
                # build/bubble-mock.rom, RAM-backed, for the emulator
make mock       # the mock image alone
make size       # how much of the $C000-$FEFF window is used
make help
make LWASM=/path/to/lwasm    # if lwasm is not on PATH
```

`make` builds both images because the regression suite runs the mock
one: a default that built only the hardware image would leave the
tests measuring a stale ROM.

Both images are padded to 16384 bytes so they can be burned straight
into a 16 KB bank of a W27C512. The cartridge window is `$C000-$FEFF`,
so the last 256 bytes of an image are never visible; the assembler
refuses an image that reaches them.

The Makefile checksums the sources into a 16-bit build stamp and passes
it to the assembler, and the boot screen prints it as `BUILD xxxx`. Two
machines showing the same four digits are running the same image. An
image assembled by hand, without the Makefile, shows `0000`.

One image covers every arrangement. Whether a Multi-Pak is present, and
which slots hold the bubble and a floppy controller, are detected at
cold start.

## Source layout

```
src/bubble.asm     top level: includes everything in order
src/config.inc     geometry, hardware addresses, 7220 constants, MOCK
src/equates.inc    Color BASIC / Extended BASIC / Disk BASIC addresses
src/header.asm     ROM header, cold start, warm start, banner
src/stubs.asm      token tables and dispatch
src/hooks.asm      the RAM hook handlers (file I/O, errors, RUN "file")
src/driver.asm     the 7220 driver - the only file that touches the bubble
src/mpi.asm        Multi-Pak detection and slot routing
src/fdc.asm        the WD1793 floppy driver
src/diag.asm       BUBDIAG
src/dskcon.asm     DSKCON: the sector layer
src/fs.asm         allocation table, directory, filename parsing
src/fcb.asm        the file engine: sequential and direct access
src/cmds.asm       the commands
```

## Testing

```
python3 test/suite.py              # the regression suite, 90 cases
python3 test/suite.py save load    # only the cases whose names match
python3 test/cocotest.py --rom build/bubble-mock.rom \
        --type 'DSKINI0\rDIR\r'    # drive it by hand and see the screen
python3 test/interop.py <decb>     # ToolShed accepts the media format
```

There is no bubble in an emulator, so the `MOCK` build swaps the 7220
driver for a RAM array and shrinks the geometry to 3 tracks of 12
sectors, granules of 3, with a dead zone. Everything above the driver is
the code the hardware image uses.

`cocotest.py` runs XRoar headless with its GDB target enabled, types
BASIC at the machine, then reads the 32 × 16 text screen out of video
RAM. `--peek ADDR LEN` dumps memory and `--regs` shows where the CPU
was, which is how a hard hang gets located to a program counter.
`--cart-type rsdos` gives the ROM XRoar's Tandy floppy controller and
`--fd IMAGE` (repeatable) inserts disk images, so the floppy driver is
tested against real `.DSK` files; `--mpi SLOT` and `--mpi-cart` place
the ROM and other cartridges in an emulated Multi-Pak.

`interop.py` builds a full-size default-geometry `.DSK` by the same
algorithms the ROM uses - DSKINI's allocation table with the dead-zone
marker, first-fit allocation, the directory entry layout - and has
ToolShed's `decb` list it, count its free granules and extract the
files byte-exact, including one that ends exactly on a granule boundary.

There are four hardware test programs. `test/bastest.bas` touches no
files at all: it checks that Color and Extended BASIC still behave -
expressions, `&H` and `&O` constants, strings, control flow, arrays,
`DEF FN`, `TIMER`, and the graphics `GET`/`PUT` and `CLS` whose RAM
vectors this ROM takes over. Run it after a reset, since it checks that
BASIC's program starts at `$2601` and `FILES` moves that on purpose.

`test/bubrand.bas` covers direct-access files and `test/bubalt.bas`
covers the same ground as the other two by different means - checksums
rather than string compares, the directory and allocation table read raw
with `DSKI$` and checked against what BASIC reports, fields on array
elements, records read back in reverse. Two independent implementations
disagreeing is worth more than either one passing.

`test/bubtest.bas` is the original hardware test. It exercises the full
command set on both drives, then across them, including files longer
than a granule read back byte for byte twice with the sectors compared
before and after, a cross-device copy compared at both ends, and two
files open on different devices at once. It finishes by writing an
ASCII listing and chaining to it, which replaces the running program -
so that test is last. Load it once and save it crunched; ASCII loading
a 400-line program takes about a minute. `test/bubtold.bas` is an
earlier, larger revision kept as a control: at its size it stops at
`?OM ERROR IN 6080`, which is the correct answer for a `COPY` that does
not fit in free memory.

All of the disk programs assume the bubble is drive 0 and a floppy is
drive 1, create only `ZZ*` files and kill them, and never format.

Requires `lwasm` and `xroar` on the path, plus `bas13.rom` and
`extbas11.rom` in `~/.xroar/roms` (`disk11.rom` too for the floppy
tests). `XROAR-INSTALL.md` has the setup from scratch. Never run two
emulator sessions at once - they fight over the GDB port.

## What is verified

All 90 suite cases pass against the MOCK image, `interop.py` confirms
ToolShed reads the media format, and the hardware image has been
exercised on a CoCo with the bubble alone and with the bubble and a
Tandy floppy controller in a Multi-Pak, where all four hardware test
programs pass. A CoCo SDC presents the same register interface and
should be detected the same way, but has not been tried. Verified
against Color BASIC 1.3 with Extended BASIC 1.1; CB 1.2 and ECB 1.0 use
the same published entry addresses and are expected to work. The CoCo 3
is untested.

The hardware image assembles to just under 10 KB, leaving about 6 KB of
the window free; `make size` prints the exact figures.

## Not built

`BACKUP` and `DOS`, which report `?FC`.

## Deliberate divergences from Disk BASIC

None visible to a program that plays by the rules:

- Granules are allocated first-free-from-zero. Disk BASIC starts at
  granule 33 to even out head seeks; a bubble has no head.
- A transfer forces the CPU to normal speed and leaves it there. The
  SAM's rate bits are write-only, so the speed-up poke cannot be put
  back.
- `KILL`, and `SAVE` over an existing name, refuse with `?AO` while the
  file is open on any FCB. `DSKINI` closes all files first.
- `DSKI$`/`DSKO$` strings are clipped at 128 characters; Disk BASIC
  copies the whole string and tramples the buffer that follows.
- Corrupt allocation chains are detected and stopped with `?FS` rather
  than followed.
- `COPY` works at the sector layer and needs no free memory, so unlike
  Disk BASIC it has no limit on the size of the file. It refuses to copy
  a file onto itself (`?AE`), which Disk BASIC obeys as a no-op.
- `VERIFY` read-back sits inside DSKCON, so it covers writes made
  through the published `$C004` vector as well. Disk BASIC verifies in
  the wrapper its own file system calls, and leaves the vector
  unverified.
- The floppy motor is never switched off.
- Two Disk BASIC bugs in the compaction of the record buffer area, when
  a direct file in the middle of it closes, are not reproduced. Disk
  BASIC never updates the **other open FCBs'** buffer pointers after
  the slide, so closing two direct files in the order they were opened
  leaves the second pointing at where its buffer used to be; `FCBRNFRE`
  moves those pointers first. And its string-descriptor fix-up clears
  a descriptor *below* the hole instead of leaving it alone - the
  published listing marks the branch at `CACC` as a bug; `FLDMONE`
  leaves it alone, which is what a field on a still-open lower buffer
  needs.
- `LSET`/`RSET` bound "is this a field" by `RNBFAD` rather than
  `FCBADR`: buffers are handed out below `RNBFAD`, so anything at or
  above it is unallocated space, not a field.

Faithfully preserved quirks, because programs may depend on them:
`SAVE "X",A` erases the variables when it completes (the close comes in
through the line-input hook, as in Disk BASIC), `FILES` erases them
too, and `LOC`/`LOF` of something that is not a file is `?FC`, not
`?DN`.

## Bugs worth remembering

Found while building this. Each looked like a hardware fault from the
outside.

- **Accumulator offsets are signed.** `sta b,u` with a buffer index of
  128 or more writes *backwards*, over the FCB control bytes. Anything
  indexing a 256-byte buffer has to form the address with a 16-bit
  offset.
- **`LA171` masks off bit 7**, so it cannot read binary data; `LA176`
  returns the byte untouched.
- **`LAD21`/`LAD26`/`LAD33` reset the stack pointer.** They are not
  callable subroutines: whatever calls them can never return, which is
  why Disk BASIC's LOAD ends in a JMP.
- **The FCBs live above the region cold start clears**, so opening a
  file must zero its control bytes or it inherits whatever was there.
- **`LA35F` calls RVEC2**, and a hook that takes over that call has to
  set PRTDEV itself - Color BASIC reads it immediately afterwards to
  decide what terminates a string being read from a file.
- **Chaining a displaced RAM hook needs a self-reference check**, or a
  second initialisation captures the ROM's own handler and the error
  driver calls itself forever.
- **Direct-page state must be initialised at cold start.** Power-on RAM
  is random; a guard byte that is only ever cleared "back" to zero after
  use is a landmine an emulator's zeroed RAM never steps on.
- **Allocate granules lazily.** Allocating the next granule the moment
  the previous one fills means a file that ends exactly on a granule
  boundary owns a granule that was never written - chained in, marked
  as holding data, full of garbage.
- **A function that parses a device number must put DEVNUM back.**
  `LOC(1)` inside a `PRINT` otherwise redirects the rest of the
  statement into the file - no error, nothing visibly wrong at the site.
- **Disk BASIC clears its run flag with the ASR trick**; keep an
  equivalent, or the flag from `LOAD ,R` leaks into the next thing that
  passes through the load-finish path.
- **BASIC echoes what it reads.** During an ASCII load the console-out
  hook receives every character with the *input* file selected as the
  device. Writing it steps the reader's buffer pointer, and when the
  pointer wraps the buffer is flushed back over the file - so a read
  that appears to merely garble a listing has in fact damaged it on the
  media.
- **`STRTAB` is not the top of free RAM.** It is the string allocation
  point inside the string space; the stack sits between free memory and
  the strings. A buffer fenced against `STRTAB` walks through the stack
  and the machine leaves for a random address, which BREAK cannot
  interrupt. Fence against the stack pointer, as Disk BASIC does.
- **The floppy controller's halt enable outlives the transfer.** The
  transfer loop writes DSKREG with the halt bit set; unless it is
  cleared on the way out, a later DRQ halts the 6809 outright and
  nothing - no NMI, no BREAK - gets it back.
- **A `COM` on a flag arms nothing if the flag was already set.** The
  NMI-escape flag must be stored outright, or one stray NMI disarms the
  next transfer's exit.
- **A bare `RTS` in a RAM hook is not neutral.** Extended BASIC already
  owns some slots, and Disk BASIC's handlers *chain* to it rather than
  replace it. Chain every hook that was not `RTS` to begin with.
- **Recording a slot to chain to only works if it is populated yet.**
  `PRINT &H10` was `?SN` for a long time. The expression hook had been
  recorded like the others, but Extended BASIC fills that slot in its
  variable initialisation, which cold start calls *after* the hooks go
  in, so what was recorded was Color BASIC's `RTS`. Disk BASIC jumps to
  the Extended BASIC address outright for exactly this reason.
- **The interrupt is not on.** A cartridge started through the `DK`
  hook runs instead of Extended BASIC's cold start, which is what
  normally enables the vertical-sync interrupt at the PIA and points
  the vector at the handler that counts `TIMER`. Do both, or `TIMER`
  reads zero forever.
- **Two FCB fields share offsets 9 and 10.** A sequential file keeps
  its type and format there; a direct file keeps its record length. An
  open path that wrote the type bytes into a direct FCB produced a
  record length of `$01FF`, and a close that restated them wrote the
  record length into the directory as the file's type.
