*******************************************************************
* driver.asm - Intel 7220 bubble memory controller driver
*
* Polled I/O, no interrupts, no DMA.  The command sequences are a
* port of the author's earlier Z80 driver for this same subsystem,
* itself derived from Craig Andrews' SBC-85 routines.
*
* The 7110 delivers a byte every ~80us; the transfer loops below
* run ~29 cycles (~32us) per byte at 0.895MHz, roughly 2.5x
* headroom, and the 7220's 40 byte FIFO adds 3.2ms of slack.
* Interrupts are masked for the duration of a transfer by DSKCON.
*
* Interface to the layer above:
*   BBLBA   - logical sector number to transfer
*   BBXFER  - X = buffer, B = 0 read / 1 write; carry set on error
*   DOSINI  - bring the hardware up; sets BBNRDY/BBLERR on failure
*
* This is the only file that touches the hardware.
*******************************************************************

* --- driver RAM lives in the direct page bytes DECB documents as
* --- spare ($00F3-$00FF), so the DECB variable map is untouched.
* --- None of these can be assumed zero at power-on: cold start
* --- initialises every one of them (SAVEVEC fills the chain
* --- vectors, DOSINI sets BBNRDY/BBLERR, INIT clears CLOSING).
XV8SAV          equ     $F3             chained RVEC8  target (2)
XV17SAV         equ     $F5             chained RVEC17 target (2)
BBLBA           equ     $F7             logical sector number (2)
BBCNT           equ     $F9             byte counter (2)
BBNRDY          equ     $FB             non-zero = bubble unusable
BBLERR          equ     $FC             last hardware failure code
XV18SAV         equ     $FD             chained RVEC18 target (2)
CLOSING         equ     $FF             guards against re-entering CLOSEALL

* DBUF1 is the spare sector buffer, and DSKCON uses it to read a
* sector back when VERIFY is on.  So nothing that has to survive a
* write may live here - the ROM's own state is in the block below
* the FCBs instead (equates.inc).
*
* LOADM's offset is the exception and is safe: LOADM only reads.
LMOFF           equ     DBUF1+4         LOADM load offset (2)
* DBUF1+32..+49 belongs to the diagnostic, which is safe here for a
* different reason: BUBDIAG drives BBXFER directly and never goes
* through DSKCON, so a verify read-back cannot land on it.

* --- failure codes, one per site ---------------------------------
E_ABORT1        equ     $E1             abort: no BUSY
E_ABORT2        equ     $E2             abort: no completion
E_FFRST1        equ     $E3             FIFO reset: no BUSY
E_FFRST2        equ     $E4             FIFO reset: no completion
E_INIT1         equ     $E5             init: no BUSY
E_INIT2         equ     $E6             init: no completion
E_XFER1         equ     $EA             transfer: command not accepted
E_XFER2         equ     $EB             transfer: controller went idle
E_XFER3         equ     $EC             transfer: FIFO timeout

*******************************************************************
* BBDONE - Z set if ACCA is a plain completion, FIFO READY ignored;
* ACCA preserved.  This is BBOKST without the $42 case: the command
* sequences want a bare OP COMPLETE, because a corrected parity
* status only means anything for a data transfer.
*
* FIFO READY is masked because it is not an error.  The datasheet
* defines it, when BUSY is inactive, as "the FIFO input and output
* registers are all empty" - exactly the state an abort or a FIFO
* reset leaves behind, so an idle controller reports $41 and a
* compare for a bare $40 would reject a clean completion.  PULS does
* not disturb CC, so the compare's Z flag survives restoring the
* caller's status byte.
*
* Outside the MOCK conditional because mpi.asm uses it to recognise
* a 7220 while probing, and that probe touches real hardware even
* in a MOCK build.
*******************************************************************
BBDONE          pshs    a
                anda    #$FF-BBS_FIFO
                cmpa    #BBS_OPC
                puls    a,pc

                if      MOCK
*******************************************************************
* MOCK build - a RAM array stands in for the bubble so that every
* layer above this one can be exercised in an emulator.
*******************************************************************
* The mock stands in for the 7220 only.  MPIDET still has to run:
* a MOCK image may be looking at a real floppy controller, and the
* slot state it fills in is what DSKCON dispatches on.  Leaving it
* as power-on garbage sends floppy requests to an imaginary slot.
DOSINI          lbsr    MPIDET
                clr     BBNRDY
                clr     BBLERR
                rts

BBXFER          pshs    a,b,x,y
                ldd     BBLBA
                tfr     b,a             sector number * 256
                clrb
                addd    #MOCKBUF
                tfr     d,y
                ldb     1,s             the pushed direction flag
                tstb
                bne     BBXFW
                clrb                    256 bytes
BBXFR1          lda     ,y+
                sta     ,x+
                decb
                bne     BBXFR1
                bra     BBXFD
BBXFW           clrb
BBXFW1          lda     ,x+
                sta     ,y+
                decb
                bne     BBXFW1
BBXFD           andcc   #$FE            success
                puls    a,b,x,y,pc

                else
*******************************************************************
* Real hardware
*******************************************************************

*******************************************************************
* DOSINI - initialise the controller.  Reached through $C008.
* Sets BBNRDY and BBLERR if the bubble cannot be brought up, so
* that later commands fail with ?IO instead of hanging.
*
* MPIDET runs first and is self-configuring: it works out whether
* there is a Multi-Pak at all and, if so, which slot holds the
* bubble, then leaves SCS* pointing at it.  With no MPI it does
* nothing and the cartridge port answers directly.  One ROM covers
* every arrangement - see mpi.asm.
*
* A reset returns the MPI to its front panel switch, and DOSINI
* runs again afterwards, so the routing is self-healing.
*******************************************************************
DOSINI          pshs    a,b,x,y
                lbsr    MPIDET          before anything touches $FF40
                lbsr    BBHWINI
                lbsr    BBOKST
                beq     DOSINOK
                lbsr    BBHWINI         one retry - init often fails once
                lbsr    BBOKST
                beq     DOSINOK
                sta     BBLERR
                lda     #$FF
                sta     BBNRDY
                puls    a,b,x,y,pc
DOSINOK         clr     BBNRDY
                clr     BBLERR
                puls    a,b,x,y,pc

*******************************************************************
* BBOKST - is the status in ACCA a success?  $40 = complete,
* $42 = complete with a corrected parity error.
* Returns Z set on success; ACCA preserved.
*
* FIFO READY (bit 0) is masked off first, for the reason given at
* BBDONE.  PULS does not disturb CC, so the Z flag set by the
* compare survives the restore of the caller's status byte.
*******************************************************************
BBOKST          pshs    a
                anda    #$FF-BBS_FIFO
                cmpa    #BBS_OPC
                beq     BBOKST1
                cmpa    #BBS_OPC+2
BBOKST1         puls    a,pc

*******************************************************************
* BBHWINI - the low level initialisation sequence.
* Returns ACCA = final controller status.
*******************************************************************
BBHWINI         bsr     BBIPARM         load the parametric registers
                lbsr    BBABORT         abort anything in progress
                lbsr    BBDONE          plain completion - $42 applies
                bne     BBHWIN9         only to data commands
                lbsr    BBIPARM         reload them after the abort
                lda     #BBC_INIT
                lbsr    BBSEND
                bcc     BBHWIN1
                lda     #E_INIT1
                rts
BBHWIN1         ldy     #$FFFF
BBHWIN2         lda     BBSTAT
                lbsr    BBDONE
                beq     BBHWIN9
                tfr     a,b
                andb    #$30            both timing error bits set?
                cmpb    #$30
                beq     BBHWIN3
                leay    -1,y
                bne     BBHWIN2
                lda     #E_INIT2
                rts
BBHWIN3         lda     BBSTAT          read the final status
BBHWIN9         rts

*******************************************************************
* BBIPARM - parametric registers with the initialisation defaults:
* one page block length, address zero.
*******************************************************************
BBIPARM         lda     #BBR_BLRLSB
                sta     BBSTAT
                lda     #1
                sta     BBDATA
                lda     #BBV_BLRMSB
                sta     BBDATA
                lda     #BBV_ENABLE
                sta     BBDATA
                clra
                sta     BBDATA          address LSB
                sta     BBDATA          address MSB
                rts

*******************************************************************
* BBPARM - parametric registers for a sector transfer:
* BBPPS pages, bubble address = BBLBA * BBPPS.
*   Entry: ACCA = enable register value - BBV_ENABLE for a write,
*          BBV_ENABRD (with MFBTR) for a read.  See config.inc.
*
* The register address counter is loaded with $0B and increments
* after each data-port write, so the five writes land on BLR LSB,
* BLR MSB, enable, AR LSB, AR MSB; the counter then rests on the
* FIFO, which is exactly where the data transfer wants it.
*******************************************************************
BBPARM          pshs    a               the enable value for this op
                lda     #BBR_BLRLSB
                sta     BBSTAT
                lda     #BBPPS
                sta     BBDATA
                lda     #BBV_BLRMSB
                sta     BBDATA
                puls    a
                sta     BBDATA          enable
                ldd     BBLBA
                lslb                    x2
                rola
                lslb                    x4 = pages per sector
                rola
                stb     BBDATA          address LSB
                sta     BBDATA          address MSB
                lda     BBSTAT
                rts

*******************************************************************
* BBSEND - write the command in ACCA and wait for the 7220 to
* acknowledge it.  Returns carry set on timeout.
*
* Acknowledgement is BUSY going high - but a quick command (abort,
* FIFO reset) can finish before this processor's first status read
* ever sees BUSY, so OP COMPLETE also counts: the status bits are
* cleared when a command is issued, which means an OP COMPLETE seen
* here belongs to this command, not a previous one.
*******************************************************************
BBSEND          sta     BBSTAT
                ldy     #$FFFF
BBSEND1         lda     BBSTAT
                bmi     BBSEND2         bit 7 = BUSY: accepted
                bita    #BBS_OPC
                bne     BBSEND2         already finished
                leay    -1,y
                bne     BBSEND1
                orcc    #$01
                rts
BBSEND2         andcc   #$FE
                rts

*******************************************************************
* BBABORT - abort; the controller wants it twice.
* Returns ACCA = status.
*******************************************************************
BBABORT         bsr     BBABRT1
                lbsr    BBOKST
                bne     BBABRT9
BBABRT1         lda     #BBC_ABORT
                lbsr    BBSEND
                bcc     BBABRT2
                lda     #E_ABORT1
                rts
BBABRT2         ldy     #$FFFF
BBABRT3         lda     BBSTAT
                lbsr    BBDONE          FIFO READY masked - see BBOKST
                beq     BBABRT9
                leay    -1,y
                bne     BBABRT3
                lda     #E_ABORT2
BBABRT9         rts

*******************************************************************
* BBFFRST - reset the FIFO.  This one must return exactly $40:
* the parity status $42 only applies to data commands.
*******************************************************************
BBFFRST         lda     #BBC_FIFORST
                lbsr    BBSEND
                bcc     BBFFR1
                lda     #E_FFRST1
                rts
BBFFR1          ldy     #$FFFF
BBFFR2          lda     BBSTAT
                lbsr    BBDONE
                beq     BBFFR9
                leay    -1,y
                bne     BBFFR2
                lda     #E_FFRST2
BBFFR9          rts

*******************************************************************
* BBXFER - transfer one sector.
*   Entry: BBLBA = sector, X = buffer, B = 0 read / 1 write
*   Exit:  carry clear on success; carry set with BBLERR set on
*          failure.  X is preserved.
*******************************************************************
BBXFER          pshs    b,x
                lbsr    BBABORT
                lbsr    BBFFRST
                lbsr    BBDONE
                beq     BBXF1
                sta     BBLERR
                bra     BBXF8
BBXF1           ldb     ,s              direction picks the enable value:
                lda     #BBV_ENABRD     reads need MFBTR or the FIFO
                tstb                    outruns this processor
                beq     BBXF1E
                lda     #BBV_ENABLE     writes must not have it
BBXF1E          lbsr    BBPARM
                ldd     #SECLEN
                std     BBCNT
                ldb     ,s              direction
                ldx     1,s             buffer
                tstb
                bne     BBXFPUT
                lbsr    BBGET
                bra     BBXF2
BBXFPUT         lbsr    BBPUT
BBXF2           bcs     BBXF8
* wait for the controller to leave BUSY, then check the final status
                ldy     #$FFFF
BBXF3           lda     BBSTAT
                bpl     BBXF4
                leay    -1,y
                bne     BBXF3
BBXF4           lda     BBSTAT
                lbsr    BBOKST
                bne     BBXF7
                puls    b,x
                andcc   #$FE
                rts
BBXF7           sta     BBLERR
BBXF8           puls    b,x
                orcc    #$01
                rts

*******************************************************************
* BBGET - read BBCNT bytes from the FIFO into (X).
*
* The inner loop is the timing critical path: status, test FIFO
* bit, read, store, count, branch = ~29 cycles per byte.
*******************************************************************
BBGET           lda     #BBC_RDDATA
                lbsr    BBSEND
                bcc     BBGET1
                lda     #E_XFER1
                bra     BBGETER
BBGET1          ldy     BBCNT
                ldu     #$FFFF
BBGETP          lda     BBSTAT
                lsra                    bit 0 -> carry = FIFO ready
                bcs     BBGETB
                lda     BBSTAT
                lsla                    bit 7 -> carry = BUSY
                bcc     BBGETE2         idle with bytes outstanding
                leau    -1,u
                cmpu    #0
                beq     BBGETE3
                bra     BBGETP
BBGETB          lda     BBDATA
                sta     ,x+
                ldu     #$FFFF          restart the timeout
                leay    -1,y
                bne     BBGETP
                andcc   #$FE
                rts
BBGETE2         lda     #E_XFER2
                bra     BBGETER
BBGETE3         lda     #E_XFER3
BBGETER         sta     BBLERR
                orcc    #$01
                rts

*******************************************************************
* BBPUT - write BBCNT bytes from (X) into the FIFO.
*******************************************************************
BBPUT           lda     #BBC_WRDATA
                lbsr    BBSEND
                bcc     BBPUT1
                lda     #E_XFER1
                bra     BBPUTER
BBPUT1          ldy     BBCNT
                ldu     #$FFFF
BBPUTP          lda     BBSTAT
                lsra                    bit 0 -> carry = room in FIFO
                bcs     BBPUTB
                lda     BBSTAT
                lsla
                bcc     BBPUTE2
                leau    -1,u
                cmpu    #0
                beq     BBPUTE3
                bra     BBPUTP
BBPUTB          lda     ,x+
                sta     BBDATA
                ldu     #$FFFF
                leay    -1,y
                bne     BBPUTP
                andcc   #$FE
                rts
BBPUTE2         lda     #E_XFER2
                bra     BBPUTER
BBPUTE3         lda     #E_XFER3
BBPUTER         sta     BBLERR
                orcc    #$01
                rts

                endc
