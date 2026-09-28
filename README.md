# CoCo Projects

Assorted hardware and software projects for the TRS-80 Color Computer,
by Dr. Scott M. Baker — <https://www.smbaker.com/>

## [coco-bubble](coco-bubble/)

A cartridge that puts 128 KB of Intel 7110 **magnetic bubble memory**
behind a Disk BASIC-compatible command set: `DIR`, `SAVE`, `LOAD`,
`OPEN`, `COPY`, direct-access files and the rest work as they do on a
disk system, over a non-volatile solid-state medium with no moving
parts. The 16 KB ROM writes files byte-for-byte as Disk BASIC would, so
they move to and from real floppies with `COPY`, and a floppy controller
alongside it in a Multi-Pak becomes drives 1 to 3. Includes the ROM
source, the PLD for the bus interface, a parts list and assembly notes,
an emulator test suite, and a detailed account of both the hardware and
the ROM in [HOW-IT-WORKS.md](coco-bubble/HOW-IT-WORKS.md).

## [multicart-videocart](multicart-videocart/)

Two Raspberry Pi Pico 2 W cartridges for the CoCo 1/2 that hold your
whole ROM collection — pick a game from the OLED menu or a web browser
and the CoCo reboots straight into it. The **multicart** is the ROM
cartridge; the **videocart** is the same cartridge plus HDMI video and
audio out, rendering the VDG display by snooping the cartridge bus.
Includes firmware, schematics, gerbers, and an architectural guide in
[HOW-IT-WORKS.md](multicart-videocart/HOW-IT-WORKS.md).

## [realtalker](realtalker/)

My clone of the Colorware RealTalker speech synthesizer cartridge,
which used the Votrax SC-01-A speech IC. Includes the recreated
schematic and board files, notes on the original version 1 (CoCo 1) and
version 2 (CoCo 2) hardware, and BASIC and Python examples for driving
it.

## [cocoxt](cocoxt/)

A clone of the Burke & Burke CoCo XT hard disk interface, which puts a
PC/XT-class MFM controller (WD1002-WX1 and friends) on the CoCo's
cartridge bus, along with a long write-up on using it from **Disk
Extended Color BASIC** with B&B's Hyper-I/O rather than OS-9: setup
sequence, driver gotchas, MSA sizing, booting Hyper-I/O from ROM, and
why the address lines are inverted. Includes schematics and gerbers for
two board revisions (one adds an onboard 27C128 socket; untested) and
tools for verifying a drive image from a Gesswein MFM emulator.

## [MPI/TheLittleEngineers](MPI/TheLittleEngineers/)

A 3D-printable case (STL files) I made for the Multi-Pak Interface by
TheLittleEngineers
([CoCoDragon-MultiPakInterface](https://github.com/TheLittleEngineers/CoCoDragon-MultiPakInterface-CC-Multi-Pak_Version_1.2)).
Assembly requires M2.5 threadserts and machine screws.

## [disks](disks/)

Tools for building my disk images: a Makefile, some BASIC demo programs,
and `ccctobin.py` for converting CCC files into BIN files for the
CocoSDC.

## Related projects

- [coco-hdmi](https://github.com/sbelectronics/coco-hdmi) — an
  FPGA-based interposer (Tang Nano 9K) that adds native digital HDMI
  output to the CoCo 2 by intercepting the VDG's signals, rendering
  video at 720×480p with 48 kHz audio.
