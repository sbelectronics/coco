# How It Works

The cartridge is a 128 KB Intel bubble memory subsystem attached to the
Color Computer's expansion bus, with a 16 KB ROM that gives Color BASIC
the Disk BASIC command set over it. This document explains both halves:
how the hardware bridges a 6809 bus to a controller designed for an
8080, and how the ROM extends BASIC the way Disk BASIC does while
speaking to a medium Disk BASIC never met.

## Part 1: Hardware

### The bubble subsystem

Intel's bubble memory family divides the work among six chips:

| Part | Role |
|---|---|
| 7110-1A | The memory itself: one megabit of magnetic bubbles in a garnet film, organised as 2048 pages of 64 bytes plus redundancy. Non-volatile. |
| 7220-1 | Bubble memory controller. Presents an 8080-style bus to the host - chip select, read and write strobes, one address bit - with a 40-byte FIFO, a command register and six parametric registers. |
| 7230 | Current pulse generator: drives the generate, replicate and swap functions in the module, and watches both supply rails for power failure. |
| 7242 | Dual formatter / sense amplifier: recovers data from the module's detectors and handles the error-correcting code. |
| 7250, 7254 | Coil predriver and drive transistors for the rotating field coils. |

An 8 MHz oscillator divided by a 74HCT74 supplies the 4 MHz clock. The
power-fail path follows Intel's application note AP-187: the 7230's
open-collector PWR.FAIL output, gated through a 75463, drives the
subsystem's reset net whenever either rail drops below 80 %. That same
net is the 7220's RESET input, which is why an external reset is
introduced there and nowhere else.

This block is the same circuit as the author's Bubble Basic single-board
computer, carried over without change. The interface to it is five
signals - BBCS*, IOR*, IOW*, A0 and the eight data lines - plus reset
and power.

### The CoCo interface

The 7220 wants a chip select that is stable for the whole bus cycle and
separate read and write strobes. The 6809 offers a read/write line and
the E clock, and the CoCo's SAM has already decoded `$FF40-$FF5F` into
SCS* and `$C000-$FEFF` into CTS*. One ATF22V10 does the translation:

```
BBADDR = !A4 & !A3 & !A2 & !A1        $FF40 or $FF41, nothing else
BBCS   = SCS & BBADDR                 stable for the whole cycle
IOR    = SCS & BBADDR & E &  RW       read strobe, E-gated
IOW    = SCS & BBADDR & E & !RW       write strobe, E-gated
ROMOE  = CTS & RW
BUFEN  = CTS # (SCS & BBADDR)         74HCT245 enable
```

A0 goes straight to the 7220, which uses it to distinguish the data
FIFO (`$FF40`) from the command/status register (`$FF41`). The decode
is deliberately exact rather than a mirror across the SCS* range: the
Tandy floppy controller's WD1793 sits at `$FF48-$FF4B`, and software
of the period probes those addresses to detect a disk system. Had the
7220 answered at a mirror of `$FF49`, that probe would have been a
7220 command with stale parametric registers - a data-corruption path.
The one overlap that cannot be avoided is the floppy controller's
drive-select latch at `$FF40`, which shares an address with the FIFO;
the driver resets the FIFO before every operation, so a stray write
there is discarded.

At 0.895 MHz the E clock is high for about 490 ns, comfortably above the
minimum strobe widths the 7220 was designed for. On writes, IOW* rises
when E falls; the 74HCT245 stays enabled until SCS* itself deasserts,
tens of nanoseconds later, so the 7220 sees data held past its write
strobe without any extra latching. The buffer's direction pin is R/W
directly: the internal bus is on the A side and the CoCo on the B side,
so a read (R/W high) drives A→B toward the CoCo.

The 74HCT30 NANDs A8-A15 into a `$FFxx` page qualifier. It is not used
by the present equations - SCS* already implies the page - but it
reaches the GAL along with A0-A7, so any single byte in the `$FFxx`
page could be decoded by reprogramming the GAL alone, with no board
change.

### ROM

A standard 28-pin JEDEC socket selected by CTS* takes the whole
27C64-through-27C512 family; two 3-pin straps set pins 1 and 27 per
device. The default W27C512 is electrically erasable and holds four
16 KB images, with the straps acting as bank select. The cartridge
window is `$C000-$FEFF`, so the last 256 bytes of a 16 KB image are
never visible to the CoCo; the source ends with an assertion that refuses
to assemble an image reaching them.

### Reset

CoCo RESET* is coupled into the bubble subsystem's reset net through a
1N5819 Schottky diode, anode toward the bubble. The net is Intel's
POWERFAIL/BUS from AP-187 Figure 13 - an open-collector wire-OR node
the app note expects to be pulled low from several sources - so this is
the sanctioned way in. The diode isolates the two machines: a
power-fail event on the cartridge cannot reset the CoCo, and with the
CoCo switched off its RESET* holds the bubble in reset passively, with
nothing on the cartridge side powered.

Two component values on that net are load-bearing. The pull-up R17 is
1 kΩ because the 7230 specifies up to 1 mA of output leakage; at 1 kΩ
that costs 1 V and the node still reads a valid high, while at 4.7 kΩ
worst-case leakage would drag it to 0.3 V and hold the bubble in reset
forever - on a part that leaks toward its limit, and only that part.
The diode must be Schottky because its forward drop is in the signal
path: the 7220 accepts only 0.8 V as a low, the bubble side sits at the
CoCo's output low plus the diode's drop, and a 1N4148's 0.7 V would put
it over.

There is no supervisor IC. The SBC design had an MCP130; it was removed
because AP-187's circuit has none - the 7230 is the detector, watching
both rails, and Intel states its circuit "is required for power-up".
During the supply ramp both rails are below 80 %, so PWR.FAIL holds the
bubble in reset until they are good.

### Power

Two power domains share the board. The interface logic - GAL, buffer,
ROM, NAND - runs from the cartridge port's 5 V, so the bus-facing
chips are powered exactly when the CoCo bus is. The bubble subsystem
runs from its own supply and stays alive with the CoCo off. RN2 pulls
the three 7220 strobes to the bubble's 5 V so they idle deasserted when
the GAL is unpowered, and a dead CoCo holds reset low through the diode.

The reason for the split is the budget. Intel's Table 3 for this
configuration gives 384 mA at +5 V and 400 mA at +12 V maximum, and
requires +12 V within ±5 %. The Multi-Pak's entire +12 V rating is
400 mA across all four slots, from a transformer secondary already
carrying the -12 V load as well. There is no margin; the failure mode
would not be a refusal to start but sag under load or ripple tripping
the 7230's power-fail detector - which surfaces as intermittent data
corruption, with the driver suspected long before the supply.

Regulating on board (+18 V → 7812 → +12 V → 7805 → +5 V) holds the
bubble rails inside their tolerance regardless of adapter quality, at
the cost of about 7.4 W of dissipation as linears - beyond what a
cartridge shell will shed. Switching drop-ins in the same footprints, or
a regulated +12 V input with the first regulator omitted, are the
workable choices.

AP-187 also specifies how fast the rails may fall at power-off: 1.1 V/ms
on +12 V, 0.45 V/ms on +5 V, or data may be lost. With `C ≥ I / (dV/dt)`
at the maximum currents that is roughly 370 µF on +12 V and 850 µF on
+5 V; the board carries 470 µF on each, so the +5 V rail's decay is
worth measuring on a finished board.

### Why the host polls

The 7110 delivers a byte every 80 µs at its 100 kbit/s burst rate. A
6809 at 0.895 MHz polling the FIFO - read status, test the ready bit,
move a byte, count, branch - takes about 29 cycles, or 32 µs per byte,
and the 40-byte FIFO adds 3.2 ms of slack on top. Interrupt-driven
transfer would cost more cycles per byte than that, and the only
interrupt line at the cartridge edge is CART*, which autostart owns. So
the ROM masks IRQ and FIRQ for the duration of a sector and polls.

One datasheet detail changes that arithmetic: without the MFBTR bit set
in the 7220's enable register, the controller expects the host to drain
the FIFO at 50 KB/s during the last page of a read - 20 µs per byte,
faster than this loop. With MFBTR set the requirement drops to
12.5 KB/s. The driver sets it for reads and clears it for everything
else, as Intel's note requires.

## Part 2: Software

### How Disk BASIC extends Color BASIC

Color BASIC was built to be extended. Disk BASIC uses five mechanisms,
and this ROM uses the same five in the same way:

1. **The `DK` cold-start hook.** Color BASIC's reset code looks for the
   characters `DK` at `$C000` and, finding them, jumps to `$C002`. That
   is how Disk BASIC starts, and it is how this ROM starts. Warm starts
   go through the RSTVEC vector, which the ROM points at its own warm
   start.
2. **The command interpretation stubs.** COMVEC at `$0120` holds four
   ten-byte stubs - Color BASIC, Extended BASIC, Disk BASIC, user - each
   pointing at a keyword dictionary and a dispatch routine. The ROM
   fills the Disk BASIC slot. Because a token's value is its dictionary
   index plus a base, keeping Disk BASIC's dictionary order gives Disk
   BASIC's exact token values, which is what makes crunched program
   files interchangeable.
3. **The RAM hooks.** Twenty-five three-byte slots at `$015E-$01A8` that
   Color BASIC calls at strategic points - opening a file, sending a
   character, reading one, closing, on error, on `RUN` - and leaves as
   `RTS`. Cold start writes a `JMP` to a handler into each slot Disk
   BASIC patches.
4. **The header vectors.** `$C004` (DSKCON), `$C006` (its parameter
   block), `$C008` (initialise) and `$C00A` (DOS) are the published
   entry points for machine-language software.
5. **The RAM claim.** Disk BASIC owns `$0600` through its buffers, plus
   nine direct-page bytes at `$EA-$F2`. The ROM uses the identical map,
   so software that peeks Disk BASIC's variables finds them.

### Memory map

| Address | Contents |
|---|---|
| `$00EA-$00F0` | DSKCON parameters: op code, drive, track, sector, buffer, status |
| `$00F3-$00FF` | driver state: chained hook targets, sector number, byte count, not-ready flag, last error code, close guard |
| `$0600` | DBUF0, the sector buffer the file system works in |
| `$0700` | DBUF1, the spare sector buffer: the verify read-back, LOADM's offset, BUBDIAG working storage |
| `$0800` | four FAT images, 74 bytes each, one per drive |
| `$0928` | FCBV1, pointers to the file control blocks |
| `$0948` | RNBFAD, where the next record buffer will be handed out; `$094A` FCBADR, where the FCBs begin |
| `$094C` | filename, extension, type, ASCII flag, RUN/merge flags, default drive |
| `$0973-$0978` | directory search results |
| `$097E-$0988` | floppy state: head position per drive, NMI flag and vector, DSKREG image, verify flag, retry counter |
| `$0989` | the ROM's own state: Multi-Pak slots, the verify retry count, COPY's names and cursors - 43 bytes |
| `$09B4` | the record buffer area, one sector by default, handed out by `OPEN "D"` |
| `$0AB4` | the FCBs: two user files and the system file, 281 bytes each |
| `TXTTAB` | BASIC's program text, rounded up to a graphics-page boundary plus four pages |

Cold start reproduces Disk BASIC's arithmetic for where BASIC's text
begins, so a program's `MEM` and `PEEK`s match what they would be on a
disk system. `$2601` is the default rather than a constant: `FILES`
changes the number of file control blocks and the size of the record
buffer area, and everything above them - the graphics pages, the
program and the variables - moves to suit.

The ROM's own block sits *below* the record buffers rather than above
them, which is what lets `FILES` move the FCBs and the buffer area
around without disturbing it. Its size is fixed at assembly: the FCB
area has to end at or below `$0E00` for BASIC's text to start at
`$2601`, and one byte more would round the end up a page and move the
program. The assembler checks it, so the failure is a build error rather
than a machine whose `PEEK`s disagree with every other disk system.

### Cold start

In order: clear the working RAM; copy the command stub into COMVEC and
terminate the user table; interpose on Extended BASIC's two dispatch
vectors (so `PMODE` accounts for the RAM claimed and `DLOAD` closes
files first); record the handlers already in the three hooks that must
chain onward; hook the NMI for the floppy driver; point the IRQ at
Extended BASIC's handler and enable the 60 Hz interrupt at the PIA;
install every hook; relocate the USR vector table; initialise the file
system variables and lay out the FCBs; hand the rest of RAM to BASIC;
initialise the hardware; print the banner; arm the warm start; return
to BASIC.

The interrupt step is one Disk BASIC does not need. Extended BASIC's
cold start is what normally enables the vertical-sync interrupt and
points the vector at the handler that counts `TIMER`, and a cartridge
reached through the `DK` hook runs *instead of* that cold start, not
after it. Without the step `TIMER` reads zero forever.

The hardware initialisation is where the Multi-Pak scan runs (below),
before anything touches `$FF40`. If the bubble cannot be brought up
after one retry, a flag is set so that every later command reports
`?IO` with a diagnostic code rather than hanging.

### DSKCON

Everything above the device layer is Disk BASIC logic and device
independent. DSKCON is the device layer, with Disk BASIC's contract:
op code 0-3 (restore, format, read, write), drive, track, sector,
buffer pointer, and a status byte shaped like a WD1793's so that
machine-language utilities find the bits they expect.

Drive 0 dispatches to the bubble; drives 1-3 to the floppy driver, if
the cold-start scan found a controller. For the bubble, track and
sector become a logical sector number against Disk BASIC's 35 × 18
virtual geometry. The 7110 backs 512 of those 630 sectors. The
remainder is the *dead zone*: a read there returns a sector of zeros,
so a program that walks the whole disk completes, and a write there
fails with `?WP`, because silently discarding a write is how data
disappears. Restore and format are no-ops on a bubble.

Around each transfer DSKCON forces the CPU to normal speed (both SAM
rate bits, since the common `POKE 65495` sets one and the full
double-speed poke the other; they are write-only and cannot be put
back) and masks IRQ and FIRQ, then retries the sector up to three times
before reporting `?IO`.

With `VERIFY ON` - the cold-start default, as on a disk system - a
successful write is followed by a read of the same sector into a second
buffer and a byte-for-byte comparison; a mismatch re-issues the write,
and after three attempts reports `?VF`. The check lives at this level
because every write in the ROM passes through DSKCON, whereas Disk BASIC
puts it in the wrapper its own file system calls, leaving writes through
the published vector unverified. The read-back goes into DBUF1, the
spare sector buffer, which is where Disk BASIC puts it too - so nothing
that has to survive a write is kept there.

### The 7220 driver

The 7220 is programmed through two ports. A write to the command port
with bit 4 set issues the command in bits 0-3; with bit 4 clear it
loads a register address into the controller's address counter, after
which the parametric registers are read and written through the data
port, the counter incrementing after each access and coming to rest on
the FIFO. So a transfer is: point the counter at the block-length
register, write block length (four 64-byte pages per 256-byte sector),
the enable byte, and the two-byte bubble address (sector × 4), and the
counter is now on the FIFO exactly where the data needs to go.

Every operation opens with an abort (the datasheet says it is accepted
even when the controller is busy) and a FIFO reset (which also flushes
anything a stray write to `$FF40` left there), then issues Read or Write
Bubble Data and moves 256 bytes through the FIFO, testing the ready bit
before each byte, then waits for BUSY to fall and reads the final
status. `$40` (operation complete) is success; `$42` (complete with a
corrected parity error) is accepted for data transfers, because the
ninth data line is unconnected on this board as on the SBC.

Two behaviours of the part matter to the code. First, FIFO READY is not
an error: an idle controller reports `$41` after an abort, so
completion tests mask it off. Second, OP COMPLETE does **not** clear
when the status register is read, although the datasheet says it does.
The driver accepts either BUSY or OP COMPLETE as acknowledgement that a
command was taken, because a quick command can finish before this
processor's first status poll - and if the bit had cleared on that
poll, every wait loop would then spin for a completion that had already
been consumed. `BUBDIAG` prints an abort trace (`80 40 40 40` on this
part) as a check against a part that behaves differently.

Every wait loop is bounded at 65,535 iterations - about 1.5 seconds -
and each site that can time out has its own error code, `$E1` through
`$EC`, left in a direct-page byte that `PEEK(252)` reads. A missing or
dead bubble therefore fails initialisation in about three seconds and
reports, rather than hanging.

### The file system

The media format is Disk BASIC's, resized by constants. The allocation
table is one byte per granule on the directory track (17), sector 2:
`$FF` free, `$00-$43` a link to the next granule in the file, `$C0+n`
the last granule with n sectors in use. The directory is sectors 3-11
of that track, 32 bytes per entry: name, extension, type, ASCII flag,
first granule, bytes used in the last sector.

`DSKINI` adds one marker Disk BASIC does not have: `$FE` for a granule
whose sectors reach past the physical end of the bubble. It is neither
free nor a legal link, so the allocator passes over it without a bounds
test and a corrupt chain that wanders into it is caught. Granules 54-67
are marked that way - the directory track takes 18 of the 512 backed
sectors, and granule 54 reaches one sector past the end - leaving 54.

A RAM image of the table is kept per drive so allocation needs no media
access; it is written back when enough granules have been taken and
whenever a file closes, and it is not re-read from the media while any
file is open, because it may hold allocations not yet written.

Allocation is first-free-from-zero. Disk BASIC starts at granule 33 and
spreads outward to shorten head seeks; a bubble has no head. Allocation
is also lazy: when a granule fills, the next one is not allocated until
a byte actually arrives for it. Allocating eagerly looks harmless and is
not - a file that ends exactly on a granule boundary would own a
granule that was never written, chained in and marked as holding data,
and every read of that file would return 256 bytes of garbage at the
end.

Corrupt chains - a link out of range, a cycle, a link into the dead
zone - are detected and stopped with `?FS` rather than followed.

### The FCB engine

A file control block is 25 bytes of state and a 256-byte sector buffer.
Reading fills the buffer a sector at a time and hands out bytes;
writing accumulates bytes and flushes whole sectors, allocating
granules as it goes. Two rules the code enforces are worth knowing.

The buffer offset reaches 255, and the 6809's accumulator-offset
indexing is signed: `STA B,U` with B = 128 or more writes *backwards*,
over the FCB's control bytes. Every buffer access forms its address with
a 16-bit offset instead.

A write through a handle opened for input is refused with `?FM` at two
levels. Were it allowed, the byte would land in the sector buffer the
reader is working through, step the buffer pointer past the reader's
place, and - once the pointer wrapped - have the flush write the
mangled buffer back over the file on the media. This matters because
BASIC echoes every character it reads through the console-out hook, so
during an ASCII `LOAD` that echo arrives with the input file selected as
the output device; the hook swallows it.

### Direct-access files

A direct file is opened with a record length and read or written a
whole record at a time, in any order. The FCB is the same 281 bytes,
but it is used differently: the sector buffer holds whichever sector
was last touched, and the record itself lives in a separate *record
buffer*, carved from an area between the ROM's private block and the
FCBs. Cold start makes that area one sector; `FILES n,m` resizes it.
Buffers are handed out upward from RNBFAD as files open, and a file
that closes in the middle has the buffers above it slid down over the
hole.

`GET` and `PUT` reach the ROM through an unlikely door. Extended
BASIC's parser for graphics `GET`/`PUT` shares a code path with `CLS`,
and Disk BASIC's way in is the `CLS` RAM hook: when that hook fires
with the parser's return address on the stack and `#` as the next
character, the statement is a file `GET`/`PUT` and the hook takes it
over. `CLS` itself, and graphics `GET`/`PUT`, pass through untouched.
Whether the data is going in or out is read from Extended BASIC's own
direction flag, which the parser has already set.

The record engine turns a record number into a byte offset - a 32-bit
product, since both factors are 16-bit - splits that into a sector
index and an offset within the sector, walks the granule chain to that
sector, and then moves the record through one sector at a time, since a
record need not be aligned to a sector or shorter than one. A `PUT`
reads each sector before writing it, because the rest of that sector
belongs to other records. A `PUT` past the end of the chain allocates
granules to reach it; a `GET` past the end is `?IE`. What "the end"
means is the byte count in the file's last sector, which only a `PUT`
that reaches further ever raises, and which is reset when the last
sector moves - the same rule Disk BASIC applies.

`FIELD` is what makes the scheme fast and what makes it delicate. It
points each named string variable's descriptor straight at a slice of
the record buffer, so the variable *is* that part of the record: a
`GET` changes what it holds with nothing copied. Three things follow.
`LSET` and `RSET` exist because plain assignment to such a variable
would point it back into string space and quietly disconnect it; they
write through the descriptor into the buffer, and refuse (`?SE`) a
variable whose bytes are not inside the buffer area. Assigning *from* a
fielded variable must copy, or the target would change under the next
`GET` and dangle when the file closed; the expression-evaluation hook
recognises the right-hand side of a `LET` by the return addresses on
the stack and copies a value that lies in a record buffer into string
space, as Disk BASIC's `LET` modifier does. And when a buffer moves or
disappears at close, every descriptor pointing into it has to be moved
or emptied, which means walking the simple variables and the string
arrays.

Two places here differ from Disk BASIC on purpose, both in that
compaction at close: Disk BASIC leaves the other open files' buffer
pointers where they were after the slide, and its descriptor fix-up
clears a descriptor that lies *below* the hole - a branch its own
published listing marks as a bug. Both are corrected here.

`PRINT#` and `INPUT#` on a direct file work within the record the file
is holding, each keeping its own position from the start of the record;
`GET` and `PUT` reset both. `MKN$` and `CVN` move a number into and out
of the five-byte packed form BASIC keeps in a variable. `LOC` is the
record the file is at; `LOF` is the record count, computed in floating
point because the byte count can pass 65535.

### The hooks

| Hook | What the ROM does with it |
|---|---|
| RVEC0 `OPEN` | parses mode, file number, name and record length; opens the FCB |
| RVEC1 | accepts file numbers 1 through the number of FCBs |
| RVEC2 | print parameters for a file: Disk BASIC's exactly, a width of 256 so nothing wraps and a 16-column tab field, so a comma in `PRINT#` pads rather than separates |
| RVEC3 console out | writes the byte to the file, or into the current record of a direct file (`?ER` past its end); swallows it if the file is open for input |
| RVEC4 console in | reads the next byte, or the next byte of a direct file's record; raises the end-of-input flag at the end of either |
| RVEC5, RVEC6 | INPUT and PRINT mode checks; a direct file passes both |
| RVEC7, RVEC8 | close all files; close one file (chaining onward) |
| RVEC10 `INPUT#` | collects one item into the line buffer, honouring quotes, commas, CR and CR-LF, then re-enters BASIC's INPUT to parse it |
| RVEC11 | no break check while the console is a file |
| RVEC13 | end of an ASCII load: BASIC's direct-mode loop found console-in empty |
| RVEC14 `EOF` | reports the FCB's data-left flag; `?FM` on a direct file |
| RVEC15 expression evaluation | the `LET` modifier for fielded strings, then Extended BASIC's handler for `&H` and `&O` |
| RVEC17 errors | prints codes 25 and up (Extended BASIC's two plus Disk BASIC's eleven); chains lower codes to the displaced handler; throws away a half-loaded program if an error interrupts a crunched load |
| RVEC18 `RUN "file"` | loads and runs; chains plain `RUN` onward |
| RVEC22 `CLS` | `GET`/`PUT` on a direct file, recognised by the caller's return address and a `#`; `CLS` and graphics `GET`/`PUT` pass through |

Three of the hooks chain onward - close-one, error and `RUN` - and
record whatever was in the slot at cold start rather than hardcoding
an Extended BASIC address, so both ECB 1.0 and 1.1 work. Chaining
matters because Extended BASIC already owns some of these slots and its
handlers must still be reached. The recording checks that the slot does
not already point into this cartridge: on a second initialisation it
would otherwise capture its own handler and the error driver would call
itself forever.

RVEC15 is the exception, and is hardcoded to Extended BASIC's handler
as Disk BASIC's is. Extended BASIC does not install that slot until its
variable initialisation runs, which cold start calls *after* the hooks
go in, so recording the slot beforehand captures the `RTS` Color BASIC
left there - and `&H` constants vanish from the language with no error
to say so.

### Loading and saving

A crunched program is written as `$FF`, a two-byte length, and the
program image - Disk BASIC's format. Loading reads to the end of the
file rather than trusting the length, so a truncated file cannot run
on into whatever follows it in memory. `SAVEM` writes one block with
Disk BASIC's five-byte preamble and postamble; `LOADM` reads back each
byte after storing it, so a load aimed at ROM or absent RAM reports
`?IO` rather than silently doing nothing.

An ASCII listing is not parsed by the ROM at all. With the file selected
as the input device, BASIC's own direct-mode loop reads, crunches and
stores each line exactly as if it had been typed; when the file runs
out, the line-input termination hook finishes the load. This is why
ASCII loads are slow on large programs - each line is inserted into the
program by moving everything after it - and why loading once and
re-saving crunched is the era's answer.

`COPY` works below the file engine, at the sector layer: it reads the
source's directory entry, walks its granule chain, and builds the
destination's chain as it goes, moving one sector at a time through the
shared sector buffer. It therefore needs no free memory and has no
limit on the size of the file - Disk BASIC buffers as many whole
sectors as fit between the arrays and the stack and reports `?OM` past
that.

Two details make a failed copy safe. The destination's directory entry
is written *last*, so an error part way through - a full disk, a bad
sector - leaves the granules it had taken marked only in the RAM image
of the allocation table, which the next validate discards, and no entry
pointing at them. And the destination's chain terminator is updated
after every sector, so the chain always describes the sectors actually
written. Copying a file onto itself is refused with `?AE`: the
destination is cleared before the source is read, so obeying it would
destroy the file rather than copy it.

### Multi-Pak detection

The Multi-Pak switches only three signals among its slots - SCS*, CTS*
and CART* - through a latch at `$FF7F` whose bits 0-1 select the SCS*
slot and bits 4-5 the CTS* slot. Everything else, including HALT and
NMI, reaches every slot. The two nibbles are independent, so the ROM
can keep serving from its own slot (CTS*) while SCS* points at whichever
device is being used.

Detecting the latch at all is the first problem: a floating data bus
returns the last value driven, which would make a naive write-and-read
test pass with nothing there. The scan writes two complementary
patterns through the four latch bits that have no hardware function,
with a ROM read in between to put a different value on the bus, and
requires both to read back.

With a Multi-Pak confirmed, the scan walks from the switch's slot
upward to slot 4, probing each. The floppy probe round-trips two values
through the WD1793 sector register, which is read/write and inert until
a command consumes it; the bubble is not decoded at that address so it
is safe in any slot. The bubble probe issues an abort through `$FF41`
(not decoded by a floppy controller) and waits for a completion, then
confirms with a round-trip through the 7220's address register, because
a floating bus that happens to read `$40` would pass the first test on
its own. Probes never touch `$FF40` - a floppy controller's drive-select
and motor latch - until the slot has been shown not to hold one.

While it works, the scan carries the full latch byte for the slot under
test: the CTS* nibble exactly as found, plus the slot in the SCS* bits.
Getting that wrong drops CTS* to slot 1 and the ROM vanishes
mid-instruction. Afterwards SCS* is left on the bubble; the floppy
driver moves it for the duration of each operation and moves it back.

### The floppy driver

The Tandy controller cannot be polled at 0.895 MHz - there is not enough
time between double-density bytes to test status, move a byte and
branch. Instead the controller board stalls the processor: with the
halt flag set in DSKREG, the WD1793's DRQ drives the 6809's HALT line,
and the transfer loop becomes "move a byte, re-arm the halt, repeat",
with the hardware supplying the waiting. The loop has no exit of its
own. When the sector completes the controller raises INTRQ, wired to
NMI; the NMI handler overwrites the return address the interrupt pushed
so that its RTI lands in the completion code rather than back in the
loop.

Three details keep that from being a trap. The halt-enable bit is
cleared on every exit from a transfer, because left armed, any later
DRQ halts the processor outright, and a halted 6809 takes no interrupt,
no NMI and no break - only a reset. The flag that marks an NMI as the
transfer's is set outright rather than toggled, so one stray NMI cannot
disarm the next transfer's escape. And the loops are bounded at twice a
sector, so a missed NMI produces a disk error rather than a machine
spinning with interrupts masked.

Each operation seeks only if the head is believed to be elsewhere,
retries up to five times with a restore between attempts, and applies
write precompensation from track 22 inward as Tandy's controller does.
The motor-off timer is loaded the way Disk BASIC loads it but is not
counted down - Disk BASIC does that in an interrupt handler of its own,
where this ROM leaves the interrupt to Extended BASIC - so the motor
runs from the first access until reset.

### BUBDIAG

`BUBDIAG` is a ladder of tests from "is anything there" to a full
surface write, in the order a broken board fails: bus presence (all
ones means nothing is driving the bus), data bus (walk `$00/$FF/$AA/$55`
through a register and report the stuck bits), register file (write
the three readable parametric registers and read them back, proving the
address counter increments), abort, FIFO reset, controller
initialisation, status decode, sector reads, and - only when asked -
destructive spot and full-surface write/verify. Every rung uses the
same driver entry points the rest of the ROM uses; there is no
diagnostic-only copy of the hardware code.

The write tests write every sector before verifying any. Checking each
as it goes would miss an address line stuck so that two sectors alias
onto the same storage; two passes will not. The pattern carries the
sector number in its first two bytes and seeds the rest from it, so a
sector read back from the wrong place is caught by its contents.

## Part 3: Testing

There is no bubble in any emulator, so the ROM has a `MOCK` build that
replaces the one hardware file with a RAM array and shrinks the
geometry to fit a 32 KB machine (3 tracks × 12 sectors, granules of 3,
with a dead zone). Everything above the driver - DSKCON, the allocation
table, the directory, the FCB engine, every command - is the same code
the hardware image uses.

`software/test/cocotest.py` runs XRoar headless with its GDB remote target
enabled, types BASIC at the emulated machine, then halts it and reads
the 32 × 16 text screen out of video RAM; it can also dump memory and
registers, which is how a hard hang can be located to a program counter.
`software/test/suite.py` runs ninety cases through it - the command set,
the sequential and direct-access file engines, `FILES`, granule and
sector boundaries, error paths, and regressions for specific bugs found
along the way. With XRoar's Tandy floppy controller emulation the
floppy driver is tested against real disk images the same way.

`software/test/interop.py` builds a full-size `.DSK` image by the same algorithms
the ROM uses and has ToolShed's `decb` list it, count its free granules
and extract the files byte-exact, which validates the format against the
reference implementation.

Four BASIC programs run on the hardware itself. `BASTEST` touches no
files and checks that Color and Extended BASIC still behave - the
expressions, `&H` constants, graphics `GET`/`PUT` and `CLS`, and `TIMER`
that this ROM's hooks and vectors could have disturbed. `BUBTEST`
exercises the sequential command set on the bubble and a floppy -
including files longer than a granule read back byte for byte, twice,
with the sectors compared before and after to prove that reading does
not write, a cross-device copy compared at both ends, and two files
open on different devices at once - and finishes by writing an ASCII
listing and chaining to it. `BUBRAND` covers the direct-access family.
`BUBALT` covers the same ground as the other two by different means -
checksums, the directory and allocation table read raw, records read
back in reverse - so that a defect in one test's method does not hide
a defect in the ROM.

## Known limitations

- `BACKUP` and `DOS` are not implemented and report `?FC`.
- `COPY` refuses to copy a file onto itself (`?AE`).
- The floppy motor is never switched off.
- `TIMER` loses ticks during bubble transfers.
- A bubble transfer leaves the CPU at normal speed.
- The Multi-Pak scan runs upward from the selected slot, so a floppy
  controller in a lower-numbered slot than the switch is not found.
