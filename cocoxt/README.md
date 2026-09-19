# Burke & Burke CoCo XT — Using It From Disk BASIC (No OS-9)

Notes on running a **Burke & Burke CoCo XT** — a hard disk interface for the
TRS-80 Color Computer that adapts the CoCo's cartridge bus to a PC/XT-class
hard disk controller (WD1002-WX1 and friends, ST-506/412 MFM or RLL drives)
— from **Disk Extended Color BASIC** rather than OS-9. Written while
bringing up a clone of the original board.

Everything here was worked out from the Burke & Burke distribution disks and
manuals on the Color Computer Archive (links under *Sources*), disassembly of
the drivers, and byte-level analysis of real drive images. The B&B software,
ROM dumps and manuals are **not** redistributed in this repository; get them
from the archive. What *is* here is these notes, the schematics and Gerbers
for two revisions of the clone board (see "The clone boards"), and two small
tools.

## The short version

- **Hyper-I/O is the product you need.** It's Burke & Burke's "DOS overlay"
  that patches Disk Extended Color BASIC to talk to the CoCo XT. There is no
  other BASIC route; the base CoCo XT package is OS-9-only.
- The archive's disk image labeled **"(Coco 3)" is mislabeled — Hyper-I/O
  runs on a 64K CoCo 1, 2, or 3.** The loader detects your BASIC version
  (Color/Disk BASIC 1.0/1.1/2.0/2.1) and applies the matching patch.
- The `HyperIO-2.5.os9` image bundled in the same zip is **not Hyper-I/O at
  all** — it's B&B's **RSB 1.2** ("RS-DOS BASIC under OS-9"), a different
  product, mislabeled by the archive. Ignore it.
- **XT-ROM**, B&B's 8K OS-9 autoboot ROM (dumps are on the archive), is not
  needed for the BASIC route. It exists in "scrambled" and "unscrambled"
  forms; the ROM section below explains why.
- The one tool *not* on the Hyper-I/O disk is the low-level formatter,
  **XTFMT** — it ships on the CoCo XT master disk set (`CoCo XT-RTC (Burke &
  Burke).zip` under Disks/Hardware on the archive; use `XT25.DSK`).

## Where to get the software

Everything comes from the [Color Computer Archive](https://colorcomputerarchive.com/).
You need two zips:

| Zip | Where | What you need out of it |
|---|---|---|
| `Hyper-IO (Burke & Burke) (Coco 3).zip` | [Disks/Applications](https://colorcomputerarchive.com/repo/Disks/Applications/) | `HyperIO-2.6c.dsk` — Hyper-I/O itself (the `HyperIO-2.5.os9` in the same zip is RSB, ignore it) |
| `CoCo XT-RTC (Burke & Burke).zip` | [Disks/Hardware](https://colorcomputerarchive.com/repo/Disks/Hardware/) | `XT25.DSK` — the CoCo XT master disk: `XTFMT`, `PARK`, and the v2.5 `XT.DR` driver |

Direct links:

- <https://colorcomputerarchive.com/repo/Disks/Applications/Hyper-IO%20(Burke%20&%20Burke)%20(Coco%203).zip>
- <https://colorcomputerarchive.com/repo/Disks/Hardware/CoCo%20XT-RTC%20(Burke%20&%20Burke).zip>

The manuals are under [Documents/Manuals/Hardware](https://colorcomputerarchive.com/repo/Documents/Manuals/Hardware/)
on the same site; individual links are in *Sources* at the end. XT-ROM
dumps (`XT-ROM v2.2`–`v3.0 (Burke & Burke).zip`) sit next to the XT-RTC zip
in Disks/Hardware, but you don't need them for BASIC.

## How Hyper-I/O works

`RUN"HYPERIO"` switches the CoCo to all-RAM mode, copies the BASIC ROMs to
RAM, patches Disk BASIC, and installs a device driver (`XT.DR`) that talks to
the CoCo XT at **$FF50–$FF53** (plus $FF7F for Multi-Pak slot selection).

The hard disk is divided into **MSAs (Mass Storage Areas)**. To BASIC, each
MSA looks exactly like a floppy. The default geometry clones a 35-track
floppy exactly (68 granules, 161,280 bytes), but MSAs can be made much
larger — up to 2.9 MB — see "MSA sizing" below. MSATOOL names them `A`–`Z`,
so 26 per device; App Note 5 quotes up to 200 across one or two hard disks,
which needs several device descriptors. You map MSAs onto the four DECB
drive numbers, either via a saved default table or at runtime with
Hyper-I/O's extended syntax:

    OPEN DRIVE 2,"/H0/C"   ' drive 2 is now MSA "C" on device /H0

After that, all the normal commands (`DIR`, `LOAD`, `SAVE`, `KILL`, `COPY`,
`DSKI$`/`DSKO$`…) work against the hard disk. The on-disk format follows OS-9
RBF conventions, so the same disk can hold OS-9 and BASIC data side by side —
MSAs just look like big files to OS-9, and space can be shifted between the
two worlds at any time without reformatting.

## What's on the Hyper-I/O 2.6 disk

`HyperIO-2.6c.dsk` and `HyperIO-2.6c.os9` in the archive zip are
byte-identical. It's a dual-format floppy: an OS-9 volume label ("HYPER-I/O
Version 2.6") *and* a normal DECB directory on track 17. All the tools are on
the DECB side:

| File | Purpose |
|---|---|
| `HYPERIO.BAS` | The loader. Detects BASIC version, installs the system in RAM. Run this first, every boot (unless you go the ROM route). |
| `CONFIG.BAS` → `CNFTOOL.BAS` | Configuration menus: edit device descriptors, install/remove drivers, edit the default drive table, save the configured system back to floppy. |
| `MSATOOL.BAS` | Create / delete / erase / fix MSAs; display the index file. |
| `HDFMT.BAS` | *Logical* format — writes RBF structures (ID sector, allocation bitmap, root dir) and does a read-verify bad-sector scan. Does **not** low-level format. |
| `RECLAIM.BAS` | Convert an OS-9 partition back to BASIC use. |
| `SAVESYS.BAS` | Builds `HYPERIO.BIN`, the $C000–$FDFF EPROM image (see ROM section). |
| `XT.DR` | CoCo XT hard disk device driver. |
| `FR.DR` | Floppy device driver. |
| `HYPERDEV.BIN` / `HYPERDRV.BIN` | Saved device table / driver images; `HYPERIO.BAS` reloads these, which is how your configuration persists. |
| `HYPER1-0/1-1/2-0/2-1.BIN` | The Disk BASIC patches for the four BASIC version combos. |

A device descriptor holds: 2-character device name (`/H0` style), driver,
heads (1–16), tracks (1–1024), sectors/track, step code, precomp/RWC/park
tracks, physical drive number, and the MSA layout (tracks per MSA, sectors
per MSA track, half-tracks per granule).

## Setup sequence (all from BASIC)

### Hardware and jumpers

64K CoCo, Multi-Pak (or Y-cable with a small board mod), floppy system, CoCo
XT + WD controller + drive. The CoCo XT owns $FF50–$FF53, so nothing else
may live there. MFM controllers do 32 sectors/track of 256 bytes; RLL do 48
(with the 32-spt emulation jumper, multiply your track count by 1.53
instead).

Controller jumpers are in App Note 5 §1.5.4. For the WD1002-WX1 — and the
WD1002**A**-WX1, which §1.5.4.2 says takes the same settings:

| Jumper | Setting | What it does |
|---|---|---|
| W3 | IN | BIOS ROM enabled |
| W4 | 2-3 | I/O port 0x320 (1-2 = 0x324) |
| W6 | 2-3 | the dual-purpose drive-cable line is RWC, ≤8 heads (1-2 makes it Head Select 3 for 16-head drives, losing RWC) |
| W8 | 2-3 | primary/single controller |
| S1 | all OUT | absent on some revisions; not needed |
| W1, W2 | leave | factory use, do not change |
| W5, W7 | don't care | BIOS ROM type (only matters if you put a ROM in that socket) and IRQ select (the CoCo XT is deliberately interrupt-free, App Note 5 §1.5.3) |

W6 = 2-3 is why the descriptor has an **RWC TRACK** field: it is the cylinder
from which the controller asserts reduced write current. Take the number from
the drive's datasheet; many later drives ignore the line entirely.

Slots: put the **floppy controller in slot 4** with the MPI switch on it (its
ROM has to be live at power-on so you can `RUN"HYPERIO"`), and the **CoCo XT
in slot 3**, which is what every B&B default and utility name assumes. The
driver switches slots itself via `$FF7F` for each hard disk access, so you
never touch the switch again after boot. (Other slots work — see the note on
the descriptor option byte under "Optional" in the ROM section.)

### The sequence

1. **Build a work disk.** Take a fresh 35-track DECB disk and copy onto it
   every DECB-side file from `HyperIO-2.6c.dsk`, plus `XTFMT.BAS`,
   `XTFMT.BIN`, `PARK.BAS`, `XT.DR` and `XTDSKINI.DR` from `XT25.DSK` (both
   images from the archive zips listed under "Where to get the software"). Work
   from a copy — CNFTOOL saves your configuration back onto it. Then read
   "Driver version gotcha" below before going further; depending on your
   controller you may need the v2.5 driver resident, which takes one more
   step.
2. `RUN"HYPERIO"` — load Hyper-I/O into RAM.
3. `RUN"CONFIG"` — **usually nothing to do here.** B&B shipped the defaults
   already configured for an ST-225-class drive: the `XT` driver is installed
   and `/H0`–`/H3` exist with 4 heads, 612 tracks, 32 sectors, precomp 300,
   park 668, RWC 612, step 5. Menu 3 ("install device driver") will just tell
   you `DRIVER ALREADY INSTALLED`, and menu 1 wants a *filename* for a
   descriptor **file**, which you don't need. To change geometry, use menu
   **2 → 1** (edit descriptor table entry), where `/H0` is entry 5 — that
   edits the live table with no file involved. Other drives' numbers are in
   the DDMAKER cheat sheet (v2.5 release notes, p. 2.5-6). On an MFM emulator,
   keep the park track inside the emulated cylinder count or just never run
   `PARK`; there are no heads to park.
4. `RUN"XTFMT"` — **low-level format**. Prompts for device name and
   interleave: use 22 for 32-spt MFM, 33 for 48-spt RLL. ~8 minutes.
5. **Reboot.** (See "Reboot between steps" below — this is not optional.)
6. `RUN"HYPERIO"`, then `RUN"HDFMT"` — logical format. It asks for the
   device (`/H0`) and a volume name, then writes the ID sector, allocation
   bitmap and root directory. The bad-sector scan at the end is optional and
   slow; skip it on an emulated drive.
7. **Reboot.** Then `RUN"HYPERIO"` and `RUN"MSATOOL"` → option 1 to create
   MSAs, e.g. `/H0/A-D` to make four at once.
8. `RUN"CONFIG"` → menu **6** ("edit default device table") to say which MSA
   each of drives 0–3 gets at startup, and answer yes to saving. Keep at
   least one drive on a floppy so you can still load the tools. Done —
   `DIR 2` now lists your hard disk.

Backup: once it works, copy `HYPERDEV.BIN` and `HYPERDRV.BIN` off the floppy
and keep them somewhere safe. Those two files are the entire configuration.

### Reboot between steps

**Reboot (and re-`RUN"HYPERIO"`) after XTFMT, and again after HDFMT.**

A first attempt that ran the steps back-to-back in one session produced a
drive that ended up unusable, with MSATOOL failing at its menu
(`?ST ERROR IN 12030`). Repeating the sequence with a reboot between each
step produced a completely clean filesystem on the first try. That is the
whole of the direct evidence — the reboots are cheap, so do them.

Be careful how much you read into the mechanism. MSATOOL itself is *not*
buggy: run unmodified on an emulated CoCo 2 with Disk BASIC 1.1 it reaches
its menu normally, so the failure came from machine state, not the program.
A plausible culprit is the XT driver's sector cache — the driver packs two
256-byte logical sectors into each 512-byte physical sector and caches that
physical sector between accesses (manual §A.1.2), so after a format or a
burst of structure writes the cached copy no longer matches the platter and a
read-modify-write can push stale data back. **That mechanism is
unconfirmed.** (A missing ID sector seen on the first attempt turned out to
have an unrelated cause — a debugging program that wrote to LSN 0 — so don't
count it as evidence.)

### Driver version gotcha

The `XT.DR` on the Hyper-I/O 2.6 disk is the older v2.4-era driver (778-byte
file, 768-byte module). Per the v2.5 release notes it fails to initialize
newer WD controllers (WDXT-GEN2 and similar), which then fall back to 10 MB
default parameters. `XT25.DSK` carries the fixed v2.5 driver (781-byte file,
771-byte module).

Disassembling the two shows what changed: v2.5 widens the post-reset ready
poll from an 8-bit to a 16-bit counter (255 → 65535 polls) and relaxes the
status handshake mask from `$0F` to `$09` in both polling loops. Longer reset
recovery, different handshake — consistent with B&B's description.

The catch: **the driver that actually runs is the one embedded in
`HYPERDRV.BIN`**, which is the 1988 build, not the loose `XT.DR` file on your
disk. Copying the v2.5 `XT.DR` onto the work disk changes nothing until it is
installed. Two ways to get the v2.5 module resident:

- Through CONFIG (untested here): menu 2→4 to remove descriptors `/H0`–`/H3`
  (a driver can't be removed while a descriptor references it), menu 4 to
  remove driver `XT`, menu 3 to install driver `XT` (it loads `XT.DR` from
  the disk), then recreate the descriptors, and say yes to saving.
- By splicing the module directly into `HYPERDRV.BIN` offline (what was done
  here). `HYPERDRV.BIN` is a DECB binary loading at `$FA20`: bytes 0–1 are an
  end pointer, then a chain of driver modules each with its size at module
  offset 2–3 and its 2-character name at offset 6–7. Stock layout is `FD` (17
  bytes) at `$FA22` then `XT` (768 bytes) at `$FA33`. Replace the `XT` module
  with the 771-byte module from the v2.5 `XT.DR` (its DECB header stripped),
  set the end pointer to `$FD36`, and pad the rest of the 992-byte region.
  The descriptors point at `$FA33`, so they stay valid.

Use the v2.5 `XTFMT` as well.

## Skipping RUN"HYPERIO": booting Hyper-I/O from ROM

You don't have to load Hyper-I/O from floppy every boot. `SAVESYS.BAS`
snapshots the *running, configured* system —
`SAVEM "HYPERIO",&HC000,&HFDFF,&HA027` — producing a 15,872-byte image:
patched Disk BASIC (`$C000-$DFFF`, device table included) plus the Hyper-I/O
driver code above `$E000`.

Why bother: instant boot straight into Hyper-I/O, and the upper 32K of RAM
stays free, so 64K machine-language software that conflicts with the
RAM-resident version works normally. This is the configuration B&B sold as
the "EPROM-based Hyper-I/O" (App Note 5 §2.2).

To turn `HYPERIO.BIN` into a ROM image: strip the 5-byte DECB header and
5-byte trailer, and pad the 15,872 bytes with `$FF` to 16,384. The result
has `DK` at offset 0 and maps straight onto `$C000`. It is a 15.5K image, so
it needs a full 16K of cartridge ROM space. Places that can provide that:

### A cartridge that serves 16K (verified working)

Scott Baker's CoCo Multicart serves the image straight out of flash, which
beats burning EPROMs. It decodes A0–A13, so it covers the whole 16K `$C000`
window, and it is fine in an MPI. carts.conf line:

    HYPERIO.ROM | HYPER-I/O | 1 | flat

`flat` because it is ≤16K, autostart on, and **`insert` must stay 0** —
Hyper-I/O is a DOS, and it has to be present while BASIC is still coming up.
The multicart's **reset path** checks `$C000` for `DK` during BASIC's cold
init and enters at `$C002` with the machine half-initialised, which is
exactly what a DOS ROM wants. Its **insertion path** lets BASIC boot all the
way to READY first and only then starts the cart; that is for cassette
conversions, not for a DOS. (The insertion path entering at `$C000` and
executing the `DK` bytes is harmless and by design — `$44` is `LSRA`, `$4B`
is harmless, and control falls through to the real entry. That is not the
reason to avoid it.)

Any other cartridge that presents a full 16K at `$C000–$FFFF` from CTS\*
should do as well — e.g. a plain 27128 EPROM cart with /CE and /OE tied to
CTS\* and VPP//PGM to +5V. The v0.2 clone board's onboard 27C128 socket is
exactly that, wired straight to the CoCo XT's own CTS\* (see "The clone
boards"); it is untested so far.

### A CoCo SDC flash bank

The CoCo SDC has 128K of in-system-programmable flash as eight 16K banks,
selected at power-on by its 4/2/1 DIP switches; SDC-DOS ships in bank 0 and
includes commands to program the others. Putting the Hyper-I/O image in a
bank is attractive because the SDC then provides the ROM *and* the floppy
controller from one slot, which makes the slot problem below vanish. Leave
the SDC's **AUTO** jumper off; the SDC manual says not to use it with Disk
BASIC-style ROMs. Not tried here; the SDC as a *floppy controller* under
Hyper-I/O is verified, see below.

### The floppy controller's ROM socket (what B&B documented)

The EPROM replaces the Disk BASIC ROM, which the image already contains in
patched form. Burn the 16K image to a 27128. The controller must map the
full 16K rather than 8K — check your controller's documentation for that
before burning; that assumption is exactly what wasted a morning on the WD
card below. Nothing else in the machine changes, because the MPI stays on the
floppy slot where SCS already points.

### The WD controller's BIOS ROM socket — **this cannot work**

It is tempting: the socket is selected by CTS\* at `$C000`, so a ROM there
would boot. **But that socket tops out at 8K**, and the image is 15.5K.
Per the WD1002A-WX1 documentation, jumper W5 selects between a **2716 (2K)**
and a **2732/2764 (4K/8K)** BIOS ROM — there is no 16K setting — and on the
board checked here socket pin 26 is tied to +5V, so a 27128's A13 is stuck
high and only its upper 8K is ever visible. No amount of scrambling or
jumper-fiddling gets a 15.5K image in there. Don't try it.

That socket is for **XT-ROM**, B&B's 8K OS-9 autoboot ROM. Nothing in a
BASIC-only setup uses it.

### Optional: booting the ROM from a slot that isn't the floppy controller's

**Most people can skip this.** It only applies if you boot the Hyper-I/O
image from a cartridge in a *different* MPI slot than the floppy controller
— e.g. a multicart in slot 3 with the FDC in slot 4. If your ROM is in the
floppy controller's own socket (or an SDC bank), none of this applies.

During startup Hyper-I/O executes:

    $DCDF:  86 FF        LDA #$FF
    $DCE1:  B7 FF 7F     STA $FF7F

which forces **both** MPI fields to slot 4. Bits 5-4 route CTS and CART, not
just SCS, so on a separate cartridge in another slot this yanks the ROM out
from under the running code: `$C000-$DFFF` silently becomes the floppy
controller's stock Disk BASIC. The machine still boots and floppies still
work — because it is now running stock DECB — but the hard disk is dead and
you get a `?FC ERROR IN 0` from whatever routine was executing at the moment
of the swap.

B&B's `$FF` is not a bug: their EPROM lives *in* the floppy controller, so
ROM and FDC are the same card in the same slot and the write changes nothing.

The fix is one byte — the operand at **`$DCE0`** (offset `$1CE0` in the
header-stripped image) — set to `(CTS_slot - 1) * 16 + (SCS_slot - 1)`:

| Layout | `$DCE0` |
|---|---|
| ROM in the floppy controller / SDC bank | `$FF` (leave alone) |
| ROM cart in slot 3, FDC in slot 4 | `$23` |

`XT.DR` preserves the CTS nibble (`ANDA #$F0`) and restores the whole byte
after each access, so the hard disk still switches SCS to its own slot and
back without disturbing the ROM.

**Where the hard disk's own slot lives:** the driver reads it from byte 18 of
the device descriptor — the **DRIVER OPTION BYTE** in CNFTOOL — as slot
minus one (`$02` = slot 3, `$01` = slot 2). That byte is referenced exactly
once in the whole driver, so it is safe to change with CONFIG menu 2→1 (or
directly at `$DFA7`/`$DFBA`/`$DFCD`/`$DFE0` for `/H0`–`/H3` in the image) if
the card is somewhere other than slot 3.

### Caveats for any ROM route

- **Chicken and egg**: the image is a snapshot, so you must do the RAM-based
  install and configuration at least once to create it.
- **Configuration is baked in.** Descriptors and the default drive table are
  frozen in the image; changing them means reconfigure in RAM →
  `RUN"SAVESYS"` → rebuild. Runtime `OPEN DRIVE n,"/H0/X"` remapping still
  works.
- The default drive table is baked in too. If it maps a drive to an MSA and
  the hard disk does not answer at boot, you get an `?FC ERROR` from the
  startup open — a symptom of the hard disk problem, not a separate fault.

## Using a CoCo SDC as the floppy controller

Verified working. The SDC hardware "normally operates in FDC Emulation
Mode" — it looks like a standard floppy controller to anything that drives a
WD1793 at `$FF48–$FF4B`, which includes Hyper-I/O's patched DSKCON. Booting
Hyper-I/O from ROM sidesteps the DOS-compatibility question entirely, because
Hyper-I/O *is* the DOS and SDC-DOS never runs. Pre-mount images with a
`STARTUP.CFG` in the SD card root (`0=WORK.DSK`, `1=OTHER.DSK`) since the
SDC-DOS `DRIVE` command won't be available; that covers drives 0 and 1.

Do not try to run Hyper-I/O *on top of* SDC-DOS. Its version sniffer only
distinguishes `$D66C` (Disk BASIC 1.0) and Super ECB, so it classifies any
other DOS as plain DB 1.1 and drops its patches at fixed addresses —
`$D8D0–$DF36` and friends — on top of code that isn't stock DECB.

## MSA sizing

The default MSA is a byte-exact 35-track floppy clone — 68 granules of 9
sectors, 161,280 bytes — which is what you want for maximum compatibility
with software that expects a real floppy. It is not a limit. Three fields in
the *device descriptor* (CONFIG menu 2 → 1) set MSA geometry, and because they
live in the descriptor, **every MSA on that device shares them**:

| Field | Symbol | Range enforced by CNFTOOL |
|---|---|---|
| TRACKS / MSA | `KS` | 1–192 |
| SECTORS / MSA TRACK | `ST` | 1–124, and even (`HT = ST/2`) |
| HALF-TRACKS / MSA GRANULE | `SG` | `ceil(2·KS/192)` … `int(62/HT)` |

From those: **granules = 2·(KS−1)/SG**, **sectors per granule = SG·ST/2**.
The defaults `KS=35, ST=18, SG=1` give 2×34/1 = 68 granules of 9 sectors.

Two hard ceilings, both from the one-byte-per-granule DECB granule map:
values `$00`–`$BF` chain to the next granule, so at most **192 granules**;
`$C0+n` marks the last granule with *n* sectors used and `$FF` means free, so
at most **62 sectors per granule**. Biggest possible MSA is therefore
191 × 62 × 256 = **2.9 MB**.

| KS / ST / SG | Granule | Granules | MSA size | Fit in 19 MB |
|---|---|---|---|---|
| 35 / 18 / 1 *(default)* | 2,304 B | 68 | 158 KB | 26 (name limit) |
| 96 / 18 / 1 | 2,304 B | 190 | 428 KB | 26 (name limit) |
| 96 / 62 / 1 | 7,936 B | 190 | 1.44 MB | 13 |
| 192 / 62 / 2 | 15,872 B | 191 | 2.9 MB | 6 |

`96 / 62 / 1` is the sweet spot: nine times the default capacity while
keeping a 31-sector granule, so small files don't waste much. The 2.9 MB
maximum sounds better than it lives — with 15,872-byte granules a 2 KB BASIC
program still consumes a full granule.

**Changing these fields reinterprets every existing MSA on the device**, so
delete the MSAs first (MSATOOL option 2), edit the descriptor, save, reboot,
then re-create them. Software that bypasses DECB and drives DSKCON with
hardcoded 35-track assumptions will not understand a non-floppy-shaped MSA;
if you need one for that, give it its own descriptor (`/H1`) rather than
mixing geometries on `/H0`.

## The clone boards

Two revisions, both in `schematics/` (PDF) and `gerbers/` (RS-274X plus
Excellon drill). The circuit is the original CoCo XT-RTC: a 74HCT04 inverts
A4/A5/A6/A14 and RESET, a 74HCT00 derives IOR\*/IOW\* from E, R/W\* and SCS\*,
and a DS1215 phantom clock sits in the SCS\* chain (its CEI\* in, CEO\* out) to
provide the real-time clock. The 62-pin edge connector carries the WD card.

### v0.1 — `cocoxt-0.1` (tested, the board these notes were written on)

A straight clone. Q is tied to CART\* on the board, as on the original. It
was built and tested **with a DS1215 installed** and JP1 open; that is the
only configuration verified to work.

Leave JP1 open. It looked like a "no DS1215" bypass, but it shorts CEO\* to
the OE\* net rather than passing SCS\* through, and with JP1 closed the card
does not work. The DS1215 sits in the SCS\* chain (SCS\* into CEI\*, CEO\*
out to the gate logic), so a working bypass would have to connect **socket
pins 11 to 10** (CEI\* to CEO\*). **That strap is unverified** — it follows
from the schematic, but nobody has run the board without the clock chip.
Until someone does, fit the DS1215.

### v0.2 — `cocoxt-withrom-0.2` (**not yet tested**)

Same circuit plus:

- **IC3, a 27C128 socket** on the board, selected by CTS\* at `$C000-$FFFF`.
  It gets the CoCo's un-inverted address lines, so a ROM image goes in
  as-is — no scrambling. Being a full 16K it can hold the Hyper-I/O image
  from the ROM section above, which the WD card's own 8K BIOS socket cannot,
  and because the CTS\* source is right there on the card, none of the
  `$DCE0` slot business applies.
- **JP2 (CTSE/CTSI)** selects where CTS\* goes: **1-2** routes it out to the
  WD card's MEMR\* as before (for XT-ROM in the WD socket), **2-3** routes it
  to the onboard EPROM. 10K pull-ups on both branches so the unselected one
  idles high.
- **JP1** makes the Q→CART\* link a jumper instead of a trace.
- **JP3** replaces the v0.1 JP1: **open** with a DS1215 installed, **2-3**
  intended to bypass a missing DS1215 (CEI\* to CEO\*, marked *experimental*
  on the schematic because that bypass has never been tried). Position 1-2
  reproduces the old v0.1 short; don't use it.

The boards have been laid out and the netlist reviewed pin-by-pin against
the 27C128 and DS1215 datasheets, but v0.2 has not been built or powered up.
Treat it as a draft until someone reports back.

## Why the address lines are inverted

The CoCo XT inverts A4, A5, A6 (and A14) on their way to the 62-pin
connector. This is not arbitrary — the CoCo's windows are fixed, the WD
card's addresses are fixed, and they don't line up. Inverting a few bits is
the cheapest transform that makes them agree.

**A4/A5/A6 — I/O.** The WD controller decodes port `0x320` (jumper W4 offers
only 0x320 or 0x324). On the CoCo, cartridge I/O is the SCS window
`$FF40-$FF5F`, and `$FF40-$FF4F` belongs to the floppy controller, so the
hard disk had to go at `$FF50-$FF53`. The low 10 bits of `$FF50` are `0x350`;
`0x350 XOR 0x070 = 0x320`, and `0x070` is exactly A4+A5+A6. Three gates of a
74LS04 you are already using — no decoder, no PAL. As a bonus the floppy
window `$FF40-$FF4F` maps to `0x330`, which is not a WD port, so the two
controllers cannot collide in the shared SCS space.

**A14 — the BIOS ROM.** On the 62-pin side, SA19/SA18 are tied high,
SA17/SA16 tied low, SA15 comes from CoCo A15, SA14 from *inverted* A14 and
SA13 from A13. For a CoCo address in `$C000-$DFFF` that gives
SA19..SA13 = `1100100`, i.e. **`0xC8000` — the standard PC/XT hard disk BIOS
ROM address**, exactly where the controller's own ROM decoder is looking.

The ROM socket needs real address bits to address a ROM, so the same inverted
lines feed it; the scrambled EPROM image is the price of the trick, not its
purpose. `tools/scramble-rom.py` undoes it (`out[j] = in[j ^ $70]`, its own
inverse), and was validated by reproducing B&B's own scrambled XT-ROM dump
byte-for-byte from the unscrambled dump.

**That socket is 8K and cannot be made bigger.** Jumper **W5** on the
WD1002A-WX1 selects the BIOS ROM *type*, not a 16K/32K size:

    W5  1-2 (default, an etch on the board)  BIOS is a 2732 or 2764
        2-3 (must cut the etch at 1-2)       BIOS is a 2716

There is no 16K position. On the board checked here, socket pin 26 — A13 on a
27128 — is tied to +5V, and the part that shipped in it was a TMM2464AP, an
8K×8 28-pin ROM. So the scrambling is only of interest for **XT-ROM**, which
is 8K. It is a dead end for the 15.5K Hyper-I/O image.

Beware of jumper tables found online for "the WD1002A-WX1" that describe W5
as selecting 16K vs 32K/64K — at least one widely-mirrored summary says
exactly that, and it does not match this board. Setting W5 to 2-3 on the
strength of it configures the socket for a 2716 and nothing will read
correctly.

## Verifying a drive from a Gesswein MFM emulator image

If the drive is a [Gesswein MFM emulator](https://www.pdp8online.com/mfm/),
you can check the filesystem without touching the CoCo. `mfm_util` decodes an
`.emu` file into raw sectors:

    mfm_util -m drive.emu --format Intel_iSBC_214_512B --sectors 17,0 \
      --heads 4 --cylinders 615 --header_crc 0xffff,0x1021,16,0 \
      --data_crc 0xffffffff,0x140a0445,32,6 --sector_length 512 \
      --extracted_data_file out.bin

(`mfm_util --analyze` prints that command line for you. Building it on a PC
takes three small fixes: supply a `libiberty.h` declaring `buildargv` and
`freeargv`, add a whitespace-splitting `buildargv` of your own, and drop
`-liberty` from the Makefile. The *declaration* matters — without a prototype
the returned pointer is truncated to `int` and it segfaults immediately.)

Two things to know when reading `out.bin`:

- **Physical geometry is 17 sectors × 512 bytes** — the WD controller's native
  XT format. This is correct and expected: the driver packs two 256-byte
  logical sectors into each 512-byte physical sector.
- **The driver only uses 16 of the 17 sectors** (32 logical = the descriptor's
  sector count). Sector 16 of every track is never touched and keeps the
  format filler (`$E6`). So logical sector *L* lives at:

      P = L // 2;  half = L % 2
      track = P // 16;  s = P % 16
      cyl = track // 4;  head = track % 4
      offset = (cyl*68 + head*17 + s) * 512 + half*256

`tools/extract-msa.py` does that arithmetic for you — point it at the sector
dump and it lists the drive's contents or writes any MSA back out as an
ordinary 35-track `.DSK` you can open with `decb`, mount in an emulator, or
diff against known-good files. (One trap it handles: a directory's allocated
sectors run past its `size` field, and the tail is still `$E6` format filler,
so an unfiltered walk "finds" garbage files.)

A correctly built drive shows: LSN 0 = RBF ID sector (total sectors 78336,
32 spt, root FD at LSN 40, attributes `$FF`, your volume name at offset 31);
LSN 40 = root directory FD, attributes `$BF`; a `MSA` subdirectory holding
`H0.IDX` plus one `H0x.MSA` file per MSA, each 161280 bytes (630 sectors =
a 35-track floppy). Each MSA's own granule map (its track 17, sector 2) is
`$FF`-filled when empty.

## Emulation

XRoar does not emulate the CoCo XT / WD1002, but it runs Hyper-I/O itself
fine, which is enough to test the BASIC tools (menus, prompts, patching)
without hardware. Useful trick for headless debugging: run with
`-vo null -ao null -gdb`, then read the text screen out of video RAM at
`$0400`–`$05FF` over the GDB remote protocol. `-load-text FILE` types commands
in. MAME's CoCo drivers have a Multi-Pak and hard disk story that may be worth
investigating for fuller pre-hardware testing.

## What's in this repository

| Path | What it is |
|---|---|
| `README.md` | These notes. |
| `schematics/cocoxt-v0.1.pdf`, `gerbers/cocoxt-0.1/` | The tested clone board. |
| `schematics/cocoxt-withrom-v0.2.pdf`, `gerbers/cocoxt-withrom-0.2/` | Revision with onboard 27C128 and DS1215-bypass jumper. **Untested.** |
| `tools/scramble-rom.py` | Address scrambler/unscrambler for the CoCo XT's BIOS ROM socket (XOR `$70`). Only useful for 8K XT-ROM images; validated against B&B's own dump. |
| `tools/extract-msa.py` | Walks the RBF filesystem in an `mfm_util` sector dump and writes any MSA out as a `.DSK`. |

Not included, by design: the Hyper-I/O and CoCo XT disk images, XT-ROM
dumps, and the Burke & Burke manuals — all on the Color Computer Archive
(links below) — and any drive images or configuration snapshots, which
contain B&B code.

No standalone Hyper-I/O manual scan appears to exist online (checked
2026-09); between App Note 5 and the disk's own BASIC sources (which
detokenize readily), everything needed is covered. The archive also has
"Hyper-IO The Disk Doctor v2.0" (an MSA repair tool) under
Disks/Applications if it's ever needed.

## Sources

- [CoCo XT App Note 5 (setup guide)](https://colorcomputerarchive.com/repo/Documents/Manuals/Hardware/CoCo%20XT%20Hard%20Disk%20Interface%20Application%20Note%205%20(Burke%20&%20Burke).pdf) — hardware, controller jumpers, product overview. The best single reference.
- [CoCo XT v2.4 manual](https://colorcomputerarchive.com/repo/Documents/Manuals/Hardware/CoCo%20XT%20Hard%20Disk%20Interface%20v2.4%20(Burke%20&%20Burke).pdf) — Appendix A has the drive geometry tables.
- [CoCo XT v2.5 release notes](https://colorcomputerarchive.com/repo/Documents/Manuals/Hardware/CoCo%20XT%20Hard%20Disk%20Interface%20v2.5%20(Burke%20&%20Burke).pdf) — controller compatibility list, DDMAKER cheat sheet, the v2.5 driver/XTFMT fixes.
- [XT-ROM v3.0 manual](https://colorcomputerarchive.com/repo/Documents/Manuals/Hardware/XT-ROM%20v3.0%20(Burke%20&%20Burke).pdf) — OS-9 only.
- [Color Computer Archive — Disks/Hardware](https://colorcomputerarchive.com/repo/Disks/Hardware/) — `CoCo XT-RTC (Burke & Burke).zip`, the master disks (XTFMT, v2.5 drivers).
- [Color Computer Archive — Disks/Applications](https://colorcomputerarchive.com/repo/Disks/Applications/) — `Hyper-IO (Burke & Burke) (Coco 3).zip`.
- [CoCo SDC User Guide](https://colorcomputerarchive.com/repo/Documents/Manuals/Hardware/CoCo%20SDC%20User%20Guide%20v4%20(Darren%20Atkinson).pdf) — FDC emulation mode, flash banks, `STARTUP.CFG`, jumpers.
- [Lomont, *Color Computer 1/2/3 Hardware Programming*](https://www.lomont.org/software/misc/coco/Lomont_CoCoHardware.pdf) — the `$FF7F` Multi-Pak register layout.

## Scott's Notes

Specifics of the installation these notes were written against. Nothing in
this section is needed to use the notes above.

**Hardware.** CoCo 2 with Disk Extended Color BASIC 1.1, Tandy Multi-Pak,
the **v0.1** clone board with a real DS1215 fitted and JP1 open (JP1 was
briefly populated during bring-up and found to kill the card), a Western
Digital WD1002**A**-WX1, and a
Gesswein MFM emulator configured as an ST-225 (`--cylinders 615 --heads 4`).
The WD card's BIOS socket shipped with a TMM2464AP and has pin 26 tied high —
that is the board the W5/8K findings above come from. A CoCo SDC is also in
the Multi-Pak; it is what verified the "SDC as floppy controller" section.

**Slots, current.** Multicart (serving the Hyper-I/O ROM) in 3, CoCo XT in
2, floppy controller or SDC in 4. The ROM image in use has the `$DCE0`
operand set to `$23` and the descriptor option bytes set to `$01` for slot 2.
The floppy-boot configuration (`HYPERDEV.BIN`) was adjusted the same way.

**Status.** Working since 2026-09-17. The drive is low-level formatted, RBF
volume `SCOTT`, with four default-geometry MSAs on `/H0`; the complete B&B
toolset is on `/H0/A` and every file there was byte-verified against the
archive originals. Booting from the multicart ROM works as of 2026-09-18.

**Local files (not published).** A ready-made work disk holding all 22 files
from the two archive images with the v2.5 driver already spliced into
`HYPERDRV.BIN`; the SAVESYS snapshot and the ROM variants built from it (stock
slot-3, slot-2, and slot-2-with-`$23`); an image of the physical system
floppy; three Gesswein `.emu` drive images capturing the empty-MSA state, a
partial-copy failure (a bad floppy read left the last 6,666 bytes of
`HYPERIO.BIN` on `/H0/A` as `$E6` filler — undetectable from `DIR`, found
only by byte comparison), and the repaired current drive; the
extracted and detokenized B&B sources; local copies of the archive manuals;
the XT-ROM dumps; and two bring-up debugging programs (`PROBE.BAS`, which
writes one test sector, and `BUSTEST.BAS`) that the "Reboot between steps"
section alludes to.

**Diagnostics.** The Tandy Diagnostic Utilities disk from the archive
(`TANDYDU.DSK`) has `FDCTST`, a factory drive write/read/verify test with a
per-track error history and a loop mode, plus FDC alignment software. Load
with `LOADM"FDCTST":EXEC`. It drives the FDC registers directly, so it tests
whichever slot SCS points at — with the SDC in 3 and the FDC in 4, load it
from an SDC image, `POKE &HFF7F,&H23`, then `EXEC` to test the real drive.
