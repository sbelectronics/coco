; ===================================================================
; radio.asm - CoCo AM/FM radio cartridge ROM, TEF6686 module version.
;
; Top level: entry header, start-up, Multi-Pak handling, the exit path
; back to BASIC, and the image layout including the default preset store
; and the optional firmware patch blob.
;
; Build with the Makefile.
;
; GROUND RULES:
;   1. Never execute from the EEPROM while it is writing itself.
;   2. Two registers, one address each: $FF5E write, $FF5F read.
;   3. ARM is set only inside the RAM save routine.
;   4. Interrupts stay masked the whole time the ROM runs.
;   5. Never touch $FF60-$FF7F except the single $FF7F slot write.
;   6. All multi-byte values are big-endian.
;   7. Every store write is verified, every load is checksum-checked.
;   8. The RAM footprint stays below $1000 so this runs on a 16K CoCo.
; ===================================================================

                .cpu    6809
                include "equates.inc"

                org     $C000

; ---- 16-byte entry header ------------------------------------------
; The image must not begin with "DK", which is the Disk BASIC signature.
; JMP is $7E, so that is guaranteed.  Offset 14 is the board byte: 1
; marks the module cartridge, and tools/checkrom.py asserts it.

ENTRY           jmp     START
                fcc     "COCORADIO"
                fcb     VER_MAJOR,VER_MINOR     ; see equates.inc
                fcb     1                       ; board 1 = TEF6686 module
                fcb     ROM_FLAGS               ; bit 0 = Multi-Pak dev build
                fdb     BUILDID                 ; image offsets 16-17

; ---- START ----------------------------------------------------------

START
; 1. Mask IRQ and FIRQ.  CART* carries Q, a 0.9 MHz square wave, so the
;    FIRQ line toggles forever once the cartridge is running and BASIC's
;    handler would re-enter $C000 endlessly.
                orcc    #$50
; 2. Stack.
                lds     #STACKTOP
                clra
                tfr     a,dp                    ; DP = 0, do not inherit BASIC's

; 2a. Zero the variable page, before anything below reads it.  This RAM
;     is arbitrary at power-on, and after EXEC &HC000 it still holds the
;     previous run's values.  Several variables are written on only one
;     branch (MUTEFLG is set by the store defaults but not by a valid
;     store), so nothing may assume a starting value it did not write.
                ldx     #VARBASE
                clrb                            ; 256 iterations
VARCLR          clr     ,x+
                decb
                bne     VARCLR
; 3. PIA1 first, so no cartridge FIRQ can be re-enabled.
                lda     #$3C
                sta     PIA1CRB                 ; CB2 = 1 sound enable, CB1 IRQ off
                lda     PIA1DB
                anda    #$07                    ; VDG alphanumeric, green
                sta     PIA1DB
; 4. PIA0.  Sound mux select A = 0, B = 1 selects the cartridge.
                lda     #$34
                sta     PIA0CRA                 ; CA2 = 0, HSYNC IRQ off
                lda     #$3C
                sta     PIA0CRB                 ; CB2 = 1, VSYNC IRQ off but the
                                                ; flag still sets: our tick source
                lda     #$FF
                sta     PIA0DB                  ; keyboard columns idle
; 5. SAM: text screen at $0400, normal speed.
                sta     SAMV0C
                sta     SAMV1C
                sta     SAMV2C
                sta     SAMF0C
                sta     SAMF1S
                sta     SAMF2C
                sta     SAMF3C
                sta     SAMF4C
                sta     SAMF5C
                sta     SAMF6C
                sta     SAMR0C
                sta     SAMR1C
; 6. Multi-Pak FIRST, because everything below reaches the cartridge
;    through SCS.  Until the slot register is set, a write to $FF5E and a
;    read of $FF5F land on whichever slot the machine powered up pointing
;    at; if that slot does not answer, the status read floats to $FF and
;    the board check below reports the cartridge missing.
                jsr     MPIINIT

; 7. Clear the control latch.  This is what makes EXEC &HC000 entry safe:
;    if a previous run was interrupted inside the save routine the latch
;    may still hold ARM, leaving $C000-$DFFF writable.  Nothing may touch
;    the EEPROM before this.  It has to follow the slot switch above, or
;    the write goes to the wrong cartridge and the real latch keeps
;    whatever ARM state it had.  Not CLR, which is a read-modify-write.
                clra
                sta     CTLSHD
                sta     CTLREG

; 8. Board check.  The ID nibble is set by solder jumpers, so a wrong
;    value on an otherwise good board almost always means an open jumper,
;    and an open HCT input floats, so the bit is unstable rather than
;    merely wrong.  The raw byte is printed for that reason.
                if      EMU=0
                lda     STATREG
                anda    #ST_IDMASK
                cmpa    #ST_IDVAL
                beq     STOK
                jsr     CLS
                lda     STATREG
                cmpa    #$FF
                bne     STWRONG
                ldy     #MSGNOCART
                bra     STBAD
STWRONG         ldy     #MSGWRONGID
STBAD           lda     #7
                ldb     #2
                jsr     PUTSAT
                lda     #9
                ldb     #2
                jsr     SCRADDR
                lda     STATREG
                jsr     PUTHEX
                ldb     #MSG_SECS
                jsr     DELAY
                jmp     EXITBAS
                endif
STOK

; 9. Copy the RAM-resident code down.
                ldx     #RAMCODE_ROM
                ldy     #RAMCODE
                ldb     #RAMCODE_LEN
RCCPY           lda     ,x+
                sta     ,y+
                decb
                bne     RCCPY
                ldx     #FIRQSTUB_ROM
                ldy     #FIRQSTUB
                ldb     #FIRQSTUB_LEN
FSCPY           lda     ,x+
                sta     ,y+
                decb
                bne     FSCPY

; 10. Put the I2C bus into its defined idle state.
                jsr     I2CIDLE

; 10b. Is the chip really 16K?  An AT28C64 fits the socket and reads
;     back as a working ROM, but it has no A13, so $E000-$FEFF, where the
;     module firmware patch lives, aliases $C000-$DEFF.  Streaming that
;     to a module in boot state would feed it our own code as a patch,
;     and a module kept powered and already patched hides the fault until
;     the next cold start.  The first 32 bytes of the patch cannot equal
;     the first 32 bytes of code on a 16K part, so if they do, say so and
;     go to the test screen without touching the module.
                if      EMU=0
                ldx     #$C000
                ldy     #TEF_PATCH_BASE
                ldb     #32
CHK8K           lda     ,x+
                cmpa    ,y+
                bne     CHK8KOK                 ; differ: a 16K chip
                decb
                bne     CHK8K
                jsr     CLS
                ldy     #MSG8K
                lda     #7
                ldb     #2
                jsr     PUTSAT
                ldy     #MSG8K2
                lda     #9
                ldb     #2
                jsr     PUTSAT
                ldb     #MSG_SECS
                jsr     DELAY
                jsr     STLOAD
                lda     #UI_TEST
                sta     UIMODE
                jsr     TESTDRAW
                jmp     MAINLOOP
CHK8KOK
                endif

; 11/12. Bring the module up: probe, patch, start, activate, unmute.
                jsr     CLS
                ldy     #MSGBOOT
                lda     #7
                ldb     #4
                jsr     PUTSAT
                jsr     DRAWHINT
                jsr     TEFINIT
                beq     TEFUP
                jsr     CLS
                lda     TEFFAIL
                cmpa    #TEFF_BOOT
                bne     TEFM1
                ldy     #MSGNEEDPATCH           ; alive, just unpatched
                bra     TEFMSHOW
TEFM1           cmpa    #TEFF_NOACK
                bne     TEFM2
                ldy     #MSGNOTUNER             ; nothing on the bus at all
                bra     TEFMSHOW
TEFM2           ldy     #MSGTUNERFAIL           ; talked, then stalled
TEFMSHOW        lda     #7
                ldb     #2
                jsr     PUTSAT
                lda     #9
                ldb     #2
                jsr     SCRADDR
                lda     TEFFAIL
                jsr     PUTHEX
                ldb     #MSG_SECS
                jsr     DELAY
                jsr     STLOAD                  ; still let the user reach
                lda     #UI_TEST                ; the diagnostics
                sta     UIMODE
                jsr     TESTDRAW
                bra     MAINLOOP
TEFUP

; 13. Preset store.
                jsr     STLOAD

; 14/15. Draw, then tune and set the audio from the store.
                clr     UIMODE
                jsr     DRAWALL
                lda     #TEFT_PRESET            ; Preset is what enables the radio
                jsr     TEFTUNE
                jsr     TEFVOL
                jsr     TEFMONO
                jsr     TEFDEEMPH
                lda     MUTEFLG
                jsr     TEFMUTE

; 16. Main loop.
MAINLOOP        jsr     UIMAIN
                bra     MAINLOOP

; ---- PUTHEX: A = byte -> two hex digits at X ------------------------

PUTHEX          pshs    a,b
                tfr     a,b
                lsra
                lsra
                lsra
                lsra
                jsr     PUTNIB
                tfr     b,a
                jsr     PUTNIB
                puls    a,b,pc
PUTNIB          anda    #$0F
                cmpa    #10
                blo     PUTNIB0
                adda    #'A'-10
                bra     PUTNIB1
PUTNIB0         adda    #'0'
PUTNIB1         jsr     VDGCHR
                sta     ,x+
                rts

; ---- MPIINIT: aim the Multi-Pak's SCS at the radio hardware ---------
; The slot register at $FF7F holds the CTS (ROM) slot in bits 5-4 and the
; SCS (I/O) slot in bits 1-0, each 0..3 for physical slots 1..4.
; On a bare CoCo the read returns floating-bus noise and the write goes
; nowhere, which is harmless either way.
;
; Production: copy the CTS slot into the SCS field, so $FF5E/$FF5F
; follow whichever slot this ROM is running from no matter where a disk
; controller sits.
;
; DEVMPI: leave the CTS field exactly as found, because the ROM under
; test is running from the multicart, and force only the SCS field to
; MPI_SCS_SLOT so the registers and the $FF40 window reach the radio
; cartridge.  The original value is kept for the exit path.
;
; Either way the board check in START confirms it: the ID nibble is read
; through SCS, so a wrong slot shows up as a bad or unstable ID.

MPIINIT         clr     MPIFLG
                lda     MPIREG

                if      DEVMPI
                sta     MPISAVE                 ; EXITBAS puts this back
                anda    #$30                    ; keep CTS where it already is
                ora     #MPI_SCS_SLOT-1         ; and aim SCS at the radio
                tfr     a,b
                else
                tfr     a,b
                anda    #$30
                lsra
                lsra
                lsra
                lsra                            ; CTS slot down into bits 1-0
                andb    #$30
                sta     TMP
                orb     TMP                     ; CTS slot in both fields
                endif

                stb     MPIREG
                lda     MPIREG                  ; did the write stick?
                anda    #$33
                pshs    b
                andb    #$33
                cmpa    ,s+
                bne     MPIINX
                lda     #1
                sta     MPIFLG
MPIINX          rts

; ---- EXITBAS: hand the machine back to BASIC, radio still playing ---

EXITBAS         tst     DIRTYHDR
                beq     EXB1
                jsr     STSAVEHDR
EXB1
                if      DEVMPI
; Put the slot register back the way it was found.  SCS was moved off
; whatever the machine had selected, so without this a disk controller in
; another slot stays unreachable and DOS commands fail until reset.
; Production leaves $FF7F alone so Disk BASIC sets what it needs.
                lda     MPISAVE
                sta     MPIREG
                endif
; Leave the control latch alone: the module keeps tuning without a host,
; and the sound mux stays on the cartridge.
; Restore what BASIC expects of PIA0.  VSYNC IRQ back on, because TIMER
; and the cursor need it.  The sound select bits stay as set at start.
                lda     #$34
                sta     PIA0CRA
                lda     #$3D
                sta     PIA0CRB
; Keep the cartridge FIRQ disabled.  This alone prevents re-entry.
                lda     #$3C
                sta     PIA1CRB
; Second line of defence, in case BASIC's warm start rewrites $FF23:
; point the RAM FIRQ hook at our stub, which just clears the flag.
                ldx     FIRQVEC
                lda     #$7E                    ; JMP extended
                sta     ,x+
                ldd     #FIRQSTUB
                std     ,x
; Warm start.
                lda     #$55
                sta     RSTFLG
                jmp     [RSTVEC]

; ---- messages -------------------------------------------------------

MSGNOCART       fcc     "RADIO CARTRIDGE NOT FOUND"
                fcb     0
MSGWRONGID      fcc     "WRONG RADIO BOARD - STATUS $"
                fcb     0
MSGBOOT         fcc     "STARTING TUNER"
                fcb     0

; ---- DRAWHINT: the help tip on the start-up screens ------------------
; The main screen has no room for a help hint, so H is introduced here.
; Shown on STARTING TUNER, which every boot passes through, and again on
; LOADING TUNER FIRMWARE, which clears the screen when it appears.
DRAWHINT        pshs    a,b,y
                ldy     #MSGHINT
                lda     #12
                ldb     #5
                jsr     PUTSAT
                puls    a,b,y,pc
MSGHINT         fcc     "TIP: PRESS H FOR HELP"
                fcb     0
MSGNOTUNER      fcc     "TUNER NOT RESPONDING"
                fcb     0
MSGNEEDPATCH    fcc     "TUNER NEEDS FIRMWARE PATCH"
                fcb     0
MSGTUNERFAIL    fcc     "TUNER START FAILED, CODE"
                fcb     0
MSG8K           fcc     "8K EEPROM FITTED - NO PATCH"
                fcb     0
MSG8K2          fcc     "IC1 MUST BE AN AT28C256"
                fcb     0

; ---- the rest of the ROM -------------------------------------------

                include "i2c.asm"
                include "tef.asm"
                include "kbd.asm"
                include "screen.asm"
                include "font.inc"
                include "store.asm"
                include "ui.asm"
                include "ramcode.asm"

CODEEND         equ     *

                if      CODEEND>STORE_ROM
                fatal   "code has run into the preset store at $DF00"
                endif

; ---- the default preset store at $DF00 ------------------------------
; A freshly programmed cartridge comes up with a valid store rather than
; an immediate "PRESETS RESET".  Checksums are computed by the assembler
; so they cannot drift when a default changes.

                org     STORE_ROM

DEFHDR          fcb     ST_MAGIC0,ST_MAGIC1,ST_VERSION
                fcb     0                       ; flags: US, not mono
                fdb     DEF_STATION
                fcb     DEF_VOLUME
                rept    ST_PAGELEN-8
                fcb     0
                endm
                fcb     (-(ST_MAGIC0+ST_MAGIC1+ST_VERSION+(DEF_STATION>>8)+(DEF_STATION&$FF)+DEF_VOLUME))&$FF

; Twelve empty records.  Eight spaces sum to $100, which is zero in eight
; bits, so an empty record's checksum is zero.
                rept    ST_NPRESET
                fdb     0
                fcc     "        "
                fcb     0,0,0,0,0
                fcb     0
                endm

; ---- the module firmware patch at $E000 -----------------------------
; Not distributed with this source: the blob is NXP/Catena copyrighted.
; tools/mkpatch.py writes rom/tefpatch.bin and rom/tefpatch.inc; the
; Makefile runs it and builds with TEF_PATCH=1.

                org     TEF_PATCH_BASE
                if      TEF_PATCH
                include "tefpatch.inc"
TEFPATCHBLOB    binclude "tefpatch.bin"
                if      (TEF_PATCH_SIZE+TEF_LUT_SIZE)>($FF00-TEF_PATCH_BASE)
                fatal   "the patch blob does not fit below $FF00"
                endif
                endif

; The last reachable byte.  Chip $3F00-$3FFF would appear at $FF00-$FFFF,
; the I/O page, so the image stops here.
                org     $FEFF
                fcb     $FF

                end
