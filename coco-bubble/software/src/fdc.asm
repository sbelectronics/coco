*******************************************************************
* fdc.asm - Tandy floppy controller (WD1793) driver
*
* Written from the mechanism as described in Disk BASIC Unravelled
* II and the WD1793 data sheet, rather than transcribed from DECB.
*
* The CoCo cannot poll this chip.  At 0.89 MHz there is not enough
* time between double density bytes to test status, move a byte and
* branch back, so the controller board stalls the processor
* instead: with the halt flag set in DSKREG, the FDC's DRQ line
* drives the 6809's HALT input.  The transfer loop is therefore
* only "move a byte, poke DSKREG, repeat", and the hardware
* supplies the waiting.
*
* Poking DSKREG every time round is what re-arms the halt.  It
* looks redundant and is not: without it the loop free-runs past
* the data.
*
* The loop has no exit.  It runs until the FDC finishes the sector
* and raises INTRQ, which is wired to NMI.  FDCNMI overwrites the
* return address the NMI pushed, so the RTI lands in FDCDONE rather
* than back in the loop, and FDCDONE returns to whoever called the
* transfer - the loop simply abandoned mid-iteration.  NMIFLG is
* what marks an NMI as ours to hijack.
*
* IRQ and FIRQ are masked across the transfer: a 60 Hz interrupt
* landing inside a halted loop would be fatal to the timing.
*******************************************************************

* --- DSKREG bits, $FF40, write only (DRGRAM shadows it) ----------
DSKMOTOR        equ     $08             motors on
DSKPRECMP       equ     $10             write precompensation
DSKDDEN         equ     $20             double density
DSKHALT         equ     $80             let DRQ halt the 6809

* --- WD1793 commands ---------------------------------------------
FDCRESTOR       equ     $03             restore, 30ms step, no head load
FDCSEEKC        equ     $17             seek, verify, 30ms step
FDCREAD         equ     $80             read sector
FDCWRITE        equ     $A0             write sector
FDCFORCE        equ     $D0             force interrupt, raises no INTRQ

* --- WD1793 status bits ------------------------------------------
FDCBUSY         equ     $01
FDCDRQ          equ     $02
FDCCRCID        equ     $08
FDCSEEKER       equ     $10

FDCPRECTK       equ     22              precomp from this track inwards
FDCMOTORT       equ     120             2 seconds of 60 Hz ticks
FDCRETRY        equ     5               attempts per operation

*******************************************************************
* FDCON - the floppy half of DSKCON.  Same contract as the bubble
* half: parameters in DCOPC/DCDRV/DCTRK/DSEC/DCBPT, result in DCSTA.
*
* SCS* is pointed at the controller for the whole operation and put
* back afterwards.  It must not move while a command is in flight:
* INTRQ stays asserted until the status register is read, and a
* read through the wrong slot would leave it asserted for good, so
* the following completion would raise no NMI edge at all.
*******************************************************************
FDCON           lbsr    MPIFDC
                lda     #FDCRETRY
                sta     FDCTRY
FDCON1          bsr     FDCGO
                lda     DCSTA
                beq     FDCON2
                dec     FDCTRY
                beq     FDCON2          out of attempts; report the status as is
                lbsr    FDCREST         recalibrate and go round again
                bra     FDCON1
* RDYTMR is loaded the way Disk BASIC loads it, but nothing here
* counts it down: Disk BASIC's 60 Hz interrupt handler does that and
* turns the motor off when it reaches zero, and this ROM does not
* hook the IRQ.  The motor therefore runs from the first floppy
* access until the machine is reset.
FDCON2          lda     #FDCMOTORT
                sta     RDYTMR
                lbsr    MPIMAP
                rts

*******************************************************************
* FDCGO - one attempt at the requested operation.
*******************************************************************
FDCGO           clr     DCSTA
                lbsr    FDCSTART        drive select, motor, density
                lda     DCOPC
                beq     FDCREST         0 = restore
                cmpa    #1
                beq     FDCBADOP        1 = format track, not ours to do
                cmpa    #3
                bhi     FDCBADOP
                lbra    FDCRW           2 = read, 3 = write
FDCBADOP        lda     #ST_RNF
                sta     DCSTA
                rts

*******************************************************************
* FDCSTART - build this operation's DSKREG image and program it.
* Returns the image in ACCA.
*
* Precompensation goes on for the inner tracks.  Where it should
* start is a property of the drive; 22 is the number Tandy chose
* for theirs and every CoCo disk is written expecting it.
*******************************************************************
FDCSTART        ldx     #FDCSELT
                ldb     DCDRV
                decb                    drive 1 is the first floppy
                lda     DRGRAM
                anda    #DSKMOTOR       keep only the motor state
                ora     b,x             drive select for this drive
                ora     #DSKDDEN
                ldb     DCTRK
                cmpb    #FDCPRECTK
                blo     FDCST1
                ora     #DSKPRECMP
FDCST1          tfr     a,b             B remembers the old motor bit
                ora     #DSKMOTOR
                sta     DRGRAM
                sta     DSKREG
                bitb    #DSKMOTOR
                bne     FDCST9          already spinning
                pshs    a
                jsr     LA7D1           two helpings of Color BASIC's
                jsr     LA7D1           delay: motors take a moment
                puls    a
FDCST9          rts

* drive select bits, by drive number.  Drive 3 is bit 6, not bit 3:
* bit 3 is the motor enable.
FDCSELT         fcb     $01,$02,$04,$40

*******************************************************************
* FDCREST - restore the head to track zero.
*******************************************************************
FDCREST         ldx     #DR0TRK
                ldb     DCDRV
                decb                    drive 1 is the first floppy
                clr     b,x             forget the assumed head position
                lda     #FDCRESTOR
                sta     FDCCMD
                lbsr    FDCSETL
                lbsr    FDCWAIT
                bne     FDCRS9          never came back
                lbsr    FDCDLY
                anda    #FDCSEEKER
                sta     DCSTA
FDCRS9          rts

*******************************************************************
* FDCRW - seek if the head is elsewhere, then transfer one sector.
*******************************************************************
FDCRW           ldx     #DR0TRK
                ldb     DCDRV
                decb                    drive 1 is the first floppy
                abx
                ldb     ,x              where the head is believed to be
                stb     FDCTRK
                cmpb    DCTRK
                beq     FDCRW2
                lda     DCTRK
                sta     FDCDATA         destination track
                sta     ,x              believe it until told otherwise
                lda     #FDCSEEKC
                sta     FDCCMD
                lbsr    FDCSETL
                lbsr    FDCWAIT
                bne     FDCRW9
                lbsr    FDCDLY
                anda    #FDCSEEKER+FDCCRCID
                beq     FDCRW2
                sta     DCSTA
FDCRW9          rts

* --- head in position; set the transfer up ------------------------
FDCRW2          lda     DSEC
                sta     FDCSECR
                ldx     #FDCDONE
                stx     DNMIVC          where the NMI is to land
                lda     FDCCMD          a status read clears a stale INTRQ
                ldb     #FDCREAD
                lda     DCOPC
                cmpa    #3
                bne     FDCRW3
                ldb     #FDCWRITE
FDCRW3          ldx     DCBPT
* Set the flag outright rather than complementing it.  COM only
* arms the escape if the flag happened to be clear, so one stray
* NMI leaving it set would silently disarm the next transfer - and
* a transfer that cannot escape never ends.
                lda     #$FF
                sta     NMIFLG          this NMI will be ours
                lda     DRGRAM
                ora     #DSKHALT        from here on, DRQ stalls the CPU
                ldy     #0              DRQ wait timeout
                ldu     #FDCCMD
                orcc    #$50            no IRQ or FIRQ inside a transfer
                stb     FDCCMD          go
                lbsr    FDCSETL
                cmpb    #FDCREAD
                beq     FDCRD

* --- write: wait for the first DRQ, then feed the chip ------------
* The loop is meant to be left by the NMI, not by falling out of the
* bottom.  The counter covers the case where the NMI never comes:
* without it a missed NMI leaves the machine spinning with
* interrupts masked and no break check.  A sector is 256 bytes, so
* twice that is slack rather than a limit.
                ldb     #FDCDRQ
FDCWR1          bitb    ,u
                bne     FDCWR1G
                leay    -1,y
                bne     FDCWR1
                bra     FDCFAIL
FDCWR1G         ldy     #2*SECLEN
FDCWR2          ldb     ,x+
                stb     FDCDATA
                sta     DSKREG          re-arm the halt
                leay    -1,y
                bne     FDCWR2
                bra     FDCLOST

* --- read: the same shape, the other way round --------------------
FDCRD           ldb     #FDCDRQ
FDCRD1          bitb    ,u
                bne     FDCRD1G
                leay    -1,y
                bne     FDCRD1
                bra     FDCFAIL
FDCRD1G         ldy     #2*SECLEN
FDCRD2          ldb     FDCDATA
                stb     ,x+
                sta     DSKREG          re-arm the halt
                leay    -1,y
                bne     FDCRD2

* --- the transfer ran on and the NMI never came -------------------
FDCLOST         clr     NMIFLG
                andcc   #$AF
                lda     #ST_CRC         report it as bad data, which it is
                sta     DCSTA
                bsr     FDCUNHALT
                rts

* --- the chip never asked for the first byte ----------------------
FDCFAIL         clr     NMIFLG
                andcc   #$AF
                bra     FDCABRT

*******************************************************************
* FDCUNHALT - take the halt enable back out of DSKREG.
*
* DRQ is wired to the 6809's HALT line and DSKREG bit 7 is what
* lets it through.  The transfer loops set that bit in the value
* they write, so the latch is left armed when a transfer ends;
* DRGRAM, which never has the bit, puts it back.  Left armed, any
* later DRQ halts the processor outright, and a halted 6809 takes no
* interrupt, no NMI and no break - only a reset.
*******************************************************************
FDCUNHALT       pshs    a
                lda     DRGRAM
                sta     DSKREG
                puls    a,pc

*******************************************************************
* FDCDONE - entered by RTI out of FDCNMI, the transfer loop
* abandoned.  Its RTS returns to whoever called FDCRW.
*
* The status bits worth keeping are the ones that say the data is
* suspect: write protect, write fault, record not found, CRC error,
* lost data.  BUSY and DRQ are transient, not faults.
*******************************************************************
FDCDONE         andcc   #$AF
                lbsr    FDCUNHALT       before anything else can raise DRQ
                lda     FDCCMD
                anda    #$7C
                sta     DCSTA
                rts

*******************************************************************
* FDCNMI - NMI service, reached through Color BASIC's RAM vector.
*
* NMI stacks CC,A,B,DP,X,Y,U,PC: twelve bytes, PC at offset 10.
* Rewriting that slot and returning sends the RTI wherever DNMIVC
* points instead of back to the interrupted instruction, which is
* the whole escape mechanism.  With the flag clear the NMI is not
* the transfer's and the frame is left alone.
*******************************************************************
FDCNMI          lda     NMIFLG
                beq     FDCNMI9
                ldx     DNMIVC
                stx     10,s
                clr     NMIFLG
FDCNMI9         rti

*******************************************************************
* FDCABRT - force the chip idle and report "not ready".
*******************************************************************
FDCABRT         lbsr    FDCUNHALT
                lda     #FDCFORCE
                sta     FDCCMD
                lbsr    FDCSETL
                lda     FDCCMD          clear INTRQ before leaving
                lda     #ST_NOTRDY
                sta     DCSTA
                rts

*******************************************************************
* FDCWAIT - spin until BUSY clears.
*   Exit: Z set, ACCA = final status, if it did.
*         Z clear if it never did, chip forced idle on the way out.
*******************************************************************
FDCWAIT         pshs    x
                ldx     #0
FDCW1           leax    -1,x
                beq     FDCW2
                lda     FDCCMD
                bita    #FDCBUSY
                bne     FDCW1
                puls    x
                orcc    #$04            Z set: it came back
                rts
FDCW2           puls    x
                lbsr    FDCABRT
                andcc   #$FB            Z clear: it did not
                rts

*******************************************************************
* FDCSETL - the 1793 needs a moment after a command write before
* its status means anything.  Two EXG A,A burn the cycles without
* disturbing a register.
*******************************************************************
FDCSETL         exg     a,a
                exg     a,a
                rts

*******************************************************************
* FDCDLY - the longer settle wanted after a seek or a restore.
*******************************************************************
FDCDLY          pshs    a,x
                ldx     #8750
FDCD1           leax    -1,x
                bne     FDCD1
                puls    a,x,pc
