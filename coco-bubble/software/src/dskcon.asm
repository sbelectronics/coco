*******************************************************************
* dskcon.asm - the sector layer
*
* DSKCON is the published machine language entry point, reached
* through the pointer at $C004 with its parameter block in the
* direct page at $EA-$F0, exactly as Disk BASIC does it.  Every
* layer above this one is device independent.
*
*   DCOPC   0 = restore, 1 = format track, 2 = read, 3 = write
*   DCDRV   drive number: 0 = bubble, 1-3 = floppies
*   DCTRK   track  0..TRKMAX-1
*   DSEC    sector 1..SECTRK
*   DCBPT   buffer address
*   DCSTA   0 = success, otherwise a WD1793-shaped status byte
*
* The virtual geometry is larger than the bubble: sectors at or
* above PHYSSEC are the "dead zone".  Reads there return zeros so
* that whole-disk walkers complete; writes there fail, because
* silently discarding a write is how data disappears.
*******************************************************************

* DCSTA bits, chosen to mirror the WD1793 status ML utilities expect

DSKRETRY        equ     3               attempts per sector

*******************************************************************
* DSKCON
*******************************************************************
DSKCON          pshs    a,b,x,y,u,cc
* Only a write gets a verify attempt counter, and setting it only for
* a write is what keeps the read-back below from resetting the count
* of the write that asked for it.
                lda     DCOPC
                cmpa    #3
                bne     DSKCDISP
                lda     #DSKRETRY
                sta     VFYCTR
DSKCDISP        clr     DCSTA

* --- which device? ------------------------------------------------
* Drive 0 is the bubble, so everything that worked before this
* existed still does.  Drives 1 upward are floppies, reached only
* when the cold start scan actually found a controller.
*
* A MOCK image refuses floppies, and the reason is geometry rather
* than hardware.  MOCK shrinks the disk to 3 tracks of 12 sectors so
* the pseudo-bubble fits in RAM, but those constants are global - a
* real floppy needs 35/18/9/17, and driving one with MOCK's numbers
* writes the directory in the wrong place.  The full size cannot be
* mocked either: 630 sectors is 161K in a 64K machine.  So cross
* device work is exercised on the hardware build, where the geometry
* is right for both.
                lda     DCDRV
                beq     DSKCBUB
                if      MOCK
                lda     #ST_NOTRDY
                sta     DCSTA
                lbra    DSKCEXIT
                endc
                cmpa    #FDCDRVS
                bhi     DSKCNDRV
                tst     MPIFDV          was a controller found?
                beq     DSKCNDRV
                lbsr    FDCON
                lbra    DSKCVFY
DSKCNDRV        lda     #ST_NOTRDY
                sta     DCSTA
                lbra    DSKCEXIT

* --- the bubble has to be alive -----------------------------------
DSKCBUB         tst     BBNRDY
                beq     DSKC2
                lda     #ST_NOTRDY
                sta     DCSTA
                lbra    DSKCEXIT

* --- which operation? ---------------------------------------------
* Restore is a no-op that succeeds: there is no head to move.
* Formatting a track is not something this ROM does, on either
* device, so it fails rather than reporting a success it did not
* earn - a machine language utility that formats through the
* published vector has to be able to tell.
DSKC2           lda     DCOPC
                beq     DSKCOK          0 = restore
                cmpa    #1
                lbeq    DSKCRNF         1 = format track
                cmpa    #3
                lbhi    DSKCRNF         an op code that does not exist here

* --- validate the address and turn it into a sector number --------
                lda     DCTRK
                cmpa    #TRKMAX
                bhs     DSKCRNF
                lda     DSEC
                beq     DSKCRNF         sectors are numbered from 1
                cmpa    #SECTRK
                bhi     DSKCRNF

                lda     DCTRK
                ldb     #SECTRK
                mul                     D = track * sectors per track
                pshs    d
                ldb     DSEC
                decb
                clra
                addd    ,s++
                std     BBLBA

* --- dead zone ----------------------------------------------------
                cmpd    #PHYSSEC
                blo     DSKC3
                lda     DCOPC
                cmpa    #3
                beq     DSKCWP          a write to the dead zone fails
                ldx     DCBPT           a read returns a sector of zeros
                clra
                ldb     #0              256 iterations
DSKCDZ          sta     ,x+
                decb
                bne     DSKCDZ
                bra     DSKCOK

* --- the transfer itself ------------------------------------------
* Interrupts are masked so that the 60Hz BASIC interrupt cannot
* intrude on the polled FIFO loop, and the CPU is forced to normal
* speed because a double speed poke halves the bus strobes to below
* what the 7220 will accept.
DSKC3           lda     #DSKRETRY
                sta     ATTCTR
DSKC4           lda     DCOPC
                suba    #2              0 = read, 1 = write
                tfr     a,b
                ldx     DCBPT
* Both SAM rate bits have to come off: $FFD6 clears R0 (the common
* POKE 65495 address-dependent speed-up) and $FFD8 clears R1 (the
* full double-speed poke).  They are write-only, so the previous
* state cannot be read back and the machine is left at normal speed
* afterwards - same policy as the disk systems of the era.
                sta     $FFD6           clear SAM R0
                sta     $FFD8           clear SAM R1
                orcc    #$50            mask IRQ and FIRQ
                jsr     BBXFER
                andcc   #$AF            interrupts back on
                bcc     DSKCOK
                dec     ATTCTR
                bne     DSKC4
                lda     #ST_CRC         out of retries
                sta     DCSTA
                bra     DSKCEXIT

DSKCRNF         lda     #ST_RNF
                sta     DCSTA
                bra     DSKCEXIT

DSKCWP          lda     #ST_WP
                sta     DCSTA
                bra     DSKCEXIT

DSKCOK          clr     DCSTA

*******************************************************************
* DSKCVFY - with VERIFY ON, read a sector back after writing it and
* compare.  Every write the ROM makes comes through here, which is
* why the check lives at this level rather than in the file system.
*
* The read-back goes into DBUF1, the spare sector buffer, which is
* where Disk BASIC puts it too.  Nothing that has to survive a write
* is kept there - the ROM's own state lives below the FCBs instead.
*
* A mismatch retries the whole write, as Disk BASIC does, and gives
* up with ?VF after DSKRETRY attempts.
*******************************************************************
DSKCVFY         lda     DCOPC
                cmpa    #3              only a write is verified
                bne     DSKCEXIT
                tst     DCSTA           and only one that succeeded
                bne     DSKCEXIT
                tst     DVERFL
                beq     DSKCEXIT
                ldx     DCBPT           the buffer just written
                pshs    x
                lda     #2
                sta     DCOPC           read the same sector back
                ldx     #DBUF1
                stx     DCBPT
                jsr     DSKCON
                lda     #3
                sta     DCOPC
                puls    x
                stx     DCBPT
                tst     DCSTA
                bne     DSKCEXIT        the read back failed; report that
                ldu     #DBUF1
                clrb                    256 bytes
DSKCV1          lda     ,x+
                cmpa    ,u+
                bne     DSKCV2
                decb
                bne     DSKCV1
                bra     DSKCEXIT        it matched
DSKCV2          dec     VFYCTR
                lbne    DSKCDISP        write it again
                lda     #ST_VFY
                sta     DCSTA

DSKCEXIT        puls    a,b,x,y,u,cc,pc

*******************************************************************
* Helpers used throughout the file system
*******************************************************************

*******************************************************************
* DREAD / DWRITE - read or write one sector.
*   Entry: A = track, B = sector, X = buffer, DCDRV = drive
*   Exit:  DCSTA set; raises ?IO on failure (does not return)
*
* DCDRV is the caller's to set and is deliberately left alone here.
* These are what the FAT and directory code go through, so forcing
* drive 0 would quietly read the bubble's filesystem while the
* caller believed it was looking at a floppy.
*******************************************************************
DREAD           pshs    a,b,x
                lda     #2
                bra     DRW1
DWRITE          pshs    a,b,x
                lda     #3
DRW1            sta     DCOPC
                lda     ,s
                sta     DCTRK
                lda     1,s
                sta     DSEC
                ldx     2,s
                stx     DCBPT
                jsr     DSKCON
                puls    a,b,x
                tst     DCSTA
                bne     DSKERR
                rts

*******************************************************************
* DSKERR - turn a DCSTA into a BASIC error.
*******************************************************************
DSKERR          lda     DCSTA
                bita    #ST_WP
                bne     DSKERWP
                bita    #ST_VFY
                bne     DSKERVF
                ldb     #ERRIO
                jmp     LAC46
DSKERWP         ldb     #ERRWP
                jmp     LAC46
DSKERVF         ldb     #ERRVF
                jmp     LAC46

*******************************************************************
* SECPTR - point X at the sector buffer DBUF0 and set up a read of
* directory track sector B.  Used by the directory routines.
*   Entry: B = sector number on the directory track
*******************************************************************
SECPTR          lda     #DIRTRK
                ldx     #DBUF0
                jmp     DREAD
