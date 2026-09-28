*******************************************************************
* mpi.asm - Multi-Pak detection and slot routing
*
* One ROM for every arrangement.  At cold start the ROM works out whether
* an MPI is present at all, and if so which slot holds the bubble
* and which holds a floppy controller, then route SCS* per device.
*
* The MPI switches only SCS*, CTS* and CART*; everything else is in
* parallel to all four slots (26-3024 service manual, p.30).  CTS*
* and SCS* are independent nibbles of the slot latch, so the ROM can
* keep serving from its own slot while SCS* points anywhere else.
* Bits 0-1 select the SCS* slot, bits 4-5 the CTS* slot; bits 2, 3,
* 6 and 7 do nothing.
*
* The conventional arrangement, as in the CoCo SDC guide and the
* Burke & Burke cards: floppy controller in slot 4, second
* controller in slot 3, and the MPI switch left on whichever slot
* holds the ROM so that it boots.  SDC-DOS scans the same way.
*
* Probes never touch $FF40 ($DSKREG on a floppy controller - drive
* select, motor, halt enable) until the slot has already been shown
* not to hold one, and never touch $FF5F (the multicart's "swap me
* now" signal).
*******************************************************************

*******************************************************************
* MPIDET - find out what is where.  Called once from DOSINI before
* anything else touches the bubble.
*******************************************************************
MPIDET          pshs    a,b,x,y,u
                clr     MPIPRES
                clr     MPIBBV
                clr     MPIFDV

* --- is there an MPI at all? -------------------------------------
* Bits 2, 3, 6 and 7 have no hardware function but read back, which
* the manual offers expressly so software can check the latch.  Two
* complementary patterns are needed: with one pattern a floating bus
* that simply returns the last value driven would answer correctly
* by accident.  The intervening ROM read is what makes even that
* case fail, by putting a different value on the bus in between.
                lda     MPISLOT         before the first write this reads
                anda    #MPISLOTB       the front panel switch, which is
                sta     MPIBASE         the routing to preserve
                ora     #MPISCRAT       pattern A: all four scratch bits
                sta     MPISLOT
                lda     DOSBAS          drive something else onto the bus
                lda     MPISLOT
                anda    #MPISCRAT
                cmpa    #MPISCRAT
                bne     MPINONE
                lda     MPIBASE         pattern B: all four clear
                sta     MPISLOT
                lda     DOSBAS
                lda     MPISLOT
                anda    #MPISCRAT
                bne     MPINONE
                inc     MPIPRES

* --- walk the slots, starting at the switch's slot -----------------
* Upward to slot 4 only: that is where the conventions put a
* controller, and it is the direction SDC-DOS scans.
*
* MPICUR carries the whole latch byte for the slot under test - the
* CTS* nibble exactly as found, plus this slot in the SCS* bits.
* Getting that combination wrong drops CTS* to slot 1 and the ROM
* vanishes mid-instruction, so it is built once and kept in memory
* rather than juggled in a register across the probe calls.
                ldb     MPIBASE
                andb    #$03            start at the switch's slot
MPISCAN         lda     MPIBASE
                anda    #$F0            keep the CTS* nibble, drop SCS*
                pshs    b
                ora     ,s+             OR in the slot under test
                sta     MPICUR
                sta     MPISLOT
                lbsr    MPIPFDC
                bcc     MPISFDC
                lbsr    MPIPBUB
                bcs     MPISNXT
                tst     MPIBBV          first one found wins
                bne     MPISNXT
                lda     MPICUR
                ora     #MPIFOUND
                sta     MPIBBV
                bra     MPISNXT
MPISFDC         tst     MPIFDV
                bne     MPISNXT
                lda     MPICUR
                ora     #MPIFOUND
                sta     MPIFDV
MPISNXT         incb
                cmpb    #4
                blo     MPISCAN
                bra     MPIDONE

* --- no Multi-Pak: probe the cartridge port as it stands -----------
* Whatever is plugged in has SCS* to itself, so there is nothing to
* route - but which device it is still matters.  The found marker
* goes in without any slot bits; MPIMAP and MPIFDC write the latch
* only when there is a latch, so the value is never used as one.
MPINONE         lbsr    MPIPFDC
                bcs     MPINONE1
                lda     #MPIFOUND
                sta     MPIFDV
                bra     MPIDONE
MPINONE1        lbsr    MPIPBUB
                bcs     MPIDONE
                lda     #MPIFOUND
                sta     MPIBBV

MPIDONE         lbsr    MPIMAP          leave SCS* on the bubble
                puls    a,b,x,y,u,pc

*******************************************************************
* MPIMAP - point SCS* at the bubble.  A no-op when there is no MPI,
* or when the scan found no bubble in one (in which case the bubble
* is presumably in the ROM's own slot and the switch already selects it).
*******************************************************************
* If the scan found no bubble, put the latch back exactly as the
* front panel switch had it - otherwise SCS* would be left pointing
* at whichever slot the scan happened to finish on.
MPIMAP          pshs    a
                tst     MPIPRES
                beq     MPIMAP9         no MPI: nothing to route
                lda     MPIBBV
                bne     MPIMAP1
                lda     MPIBASE
MPIMAP1         sta     MPISLOT
MPIMAP9         puls    a,pc

*******************************************************************
* MPIFDC - point SCS* at the floppy controller.  Same contract.
* The caller must leave the controller quiesced - command complete,
* INTRQ cleared - before switching away again, or its next
* completion produces no NMI edge and the operation after it hangs.
*******************************************************************
MPIFDC          pshs    a
                tst     MPIPRES
                beq     MPIFDC9         no MPI: the controller is simply
                lda     MPIFDV          there, nothing to route
                beq     MPIFDC9
                sta     MPISLOT
MPIFDC9         puls    a,pc

*******************************************************************
* MPIPFDC - is there a floppy controller in the selected slot?
*
* Round-trip two patterns through the sector register.  It is
* read/write and completely inert until a command consumes it, and
* the bubble does not decode that address, so this is safe to run
* against any slot.  A CoCo SDC answers here too: it presents the
* same WD177x register interface and is driven the same way.
*
* Carry clear = found.
*******************************************************************
MPIPFDC         lda     #$A5
                bsr     MPIRT
                bcs     MPIPF9
                lda     #$5A
                bsr     MPIRT
MPIPF9          rts

MPIRT           sta     FDCSECR
                pshs    a
                lda     DOSBAS          intervening bus cycle
                lda     FDCSECR
                cmpa    ,s+
                beq     MPIRT9
                orcc    #$01
                rts
MPIRT9          andcc   #$FE
                rts

*******************************************************************
* MPIPBUB - is there a 7220 in the selected slot?
*
* First an ABORT through $FF41 alone, watching for a completion.
* $FF41 is not decoded by a floppy controller, so this disturbs
* nothing if the slot holds one - and by the time this runs the
* FDC probe has already said it does not.
*
* A status that happens to read $40 off a floating bus would pass
* that on its own, so confirm with a register round-trip: set the
* address register LSB, read it back.  Table 3 of the datasheet
* makes $0E one of only three readable registers.
*
* Carry clear = found.
*******************************************************************
MPIPBUB         lda     #BBC_ABORT
                sta     BBSTAT
                ldy     #$1000
MPIPB1          lda     BBSTAT
                lbsr    BBDONE
                beq     MPIPB2
                leay    -1,y
                bne     MPIPB1
                orcc    #$01
                rts
MPIPB2          lda     #$A5
                bsr     MPIPBR
                bcs     MPIPB9
                lda     #$5A
                bsr     MPIPBR
MPIPB9          rts

MPIPBR          pshs    a
                lda     #$0E            RAC = address register LSB
                sta     BBSTAT
                puls    a
                sta     BBDATA
                pshs    a
                lda     #$0E            the write bumped RAC - put it back
                sta     BBSTAT
                lda     BBDATA
                cmpa    ,s+
                beq     MPIPBR9
                orcc    #$01
                rts
MPIPBR9         andcc   #$FE
                rts

*******************************************************************
* MPIMSG - report what was found, as part of the boot banner.
*
*   MPI: BUB 3 FDC 4
*   MPI: BUB 3
*   NO MPI
*******************************************************************
MPIMSG          pshs    a
                tst     MPIPRES
                bne     MPIM1
                ldx     #MSGNOMPI-1
                jsr     STRINOUT
                puls    a,pc
MPIM1           ldx     #MSGMPI-1
                jsr     STRINOUT
                lda     MPIBBV
                beq     MPIM2
                ldx     #MSGMBUB-1
                jsr     STRINOUT
                lda     MPIBBV
                bsr     MPISLN
MPIM2           lda     MPIFDV
                beq     MPIM9
                ldx     #MSGMFDC-1
                jsr     STRINOUT
                lda     MPIFDV
                bsr     MPISLN
MPIM9           jsr     LB95C
                puls    a,pc

* print the SCS* slot of a latch value, as a 1-based slot number
MPISLN          anda    #$03
                inca
                adda    #'0
                jmp     LA282

MSGNOMPI        fcc     'NO MPI'
                fcb     $0D,$00
MSGMPI          fcc     'MPI:'
                fcb     $00
MSGMBUB         fcc     ' BUB '
                fcb     $00
MSGMFDC         fcc     ' FDC '
                fcb     $00
