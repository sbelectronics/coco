*******************************************************************
* bubble.asm - CoCo bubble memory cartridge ROM
*
* Disk BASIC parity command set over an Intel 7110/7220 bubble.
*
* Build:  make            (hardware image, bubble.rom)
*         make mock       (emulator test image, bubble-mock.rom)
*******************************************************************

                setdp   $00

                include config.inc
                include equates.inc

                include header.asm
                include stubs.asm
                include hooks.asm
                include driver.asm
                include mpi.asm
                include fdc.asm
                include diag.asm
                include dskcon.asm
                include fs.asm
                include fcb.asm
                include cmds.asm

*******************************************************************
* Pad to the end of the cartridge window.
*
* CTS* covers $C000-$FEFF, so $FF00 upward is unreachable; the
* Makefile pads the file out to a full 16384 bytes for burning.
*******************************************************************
ROMEND          equ     *
                if      ROMEND/$FF01
                error   "ROM overflows the cartridge window"
                endc
