# CoCo Projects

Assorted hardware and software projects for the TRS-80 Color Computer,
by Dr. Scott M. Baker — <https://www.smbaker.com/>

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
