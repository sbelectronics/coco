*******************************************************************
* diag.asm - BUBDIAG, the bubble subsystem diagnostic
*
* A ladder of tests from "is anything there at all" up to a full
* surface write/verify.  Every test drives the same driver code the
* rest of the ROM uses, so a pass here means the driver works.
*
*   BUBDIAG      non-destructive tests only
*   BUBDIAG 1    + destructive write/verify of four spot sectors
*   BUBDIAG 2    + destructive write/verify of every sector
*
* Under MOCK the register-level tests have nothing to talk to (the
* mock driver replaces BBXFER and nothing below it exists), so they
* report SKIP and the transfer tests run against the RAM array.
* That is enough to exercise this file's own logic in an emulator.
*
* Everything is written for a 32 column screen: a 19 character name
* field leaves room for the widest result, "FAIL xx".
*
* Table and list pointers live in memory rather than in U across
* calls - STRINOUT and the other ROM helpers clobber registers
* freely, and several of them do.
*
* Scratch is the upper half of DBUF1, clear of the LOAD scratch at
* DBUF1+0..+31.  DBUF0 is the sector transfer buffer.
*******************************************************************

DGLEVEL         equ     DBUF1+32        0/1/2, from the command line
DGFAIL          equ     DBUF1+33        tests failed so far
DGSEC           equ     DBUF1+34        sector under test (2)
DGPAT           equ     DBUF1+36        pattern seed for that sector
DGOFF           equ     DBUF1+37        offset of the first mismatch
DGEXP           equ     DBUF1+38        the value expected there
DGGOT           equ     DBUF1+39        the value actually read
DGSKIPF         equ     DBUF1+40        test printed its own result
DGCODE          equ     DBUF1+43        failing test's detail code
DGTPTR          equ     DBUF1+44        test table pointer (2)
DGSPTR          equ     DBUF1+46        sector list pointer (2)
DGBPTR          equ     DBUF1+48        status bit table pointer (2)

E_DGREG         equ     $E7             register file read-back mismatch
E_DGDAT         equ     $E8             sector data mismatch

*******************************************************************
* BUBDIAG - the command.  The optional level argument is parsed the
* way DRVARG does it: GETCCH first, so an absent argument is not a
* syntax error.
*******************************************************************
BUBDIAG         clrb                    default level: read-only
                jsr     GETCCH
                beq     BUBDG1
                jsr     EVALEXPB
BUBDG1          stb     DGLEVEL
                clr     DGFAIL
                ldx     #DGMHDR
                lbsr    DGPS

                ldd     #DGTAB
                std     DGTPTR
DGLOOP          ldu     DGTPTR
                ldx     ,u++            test name
                beq     DGDEST
                stu     DGTPTR
                lbsr    DGPS
                ldu     DGTPTR
                ldy     ,u++            test routine
                stu     DGTPTR
                clr     DGSKIPF
                jsr     ,y
                bcs     DGBAD
                tst     DGSKIPF         did it report for itself?
                bne     DGNEXT
                ldx     #DGMOK
                lbsr    DGPS
                bra     DGNEXT
DGBAD           lbsr    DGSAY           FAIL and the code
                ldu     DGTPTR
                ldx     ,u
                cmpx    #0              a detail printer for this one?
                beq     DGNEXT
                jsr     ,x
DGNEXT          ldu     DGTPTR
                leau    2,u             step past the detail pointer
                stu     DGTPTR
                bra     DGLOOP

DGDEST          equ     *
                if      MOCK
                else
                lbsr    DGTRACE         characterise OP COMPLETE
                lbsr    DGSTAT          decode the controller status
                endc

* --- destructive passes, only when asked for ---------------------
* Anything past here overwrites the media, allocation table and
* directory included, so the cached FAT image has to be thrown away
* or the next DIR would report a filesystem that no longer exists.
* The media needs a DSKINI before it is usable again.
                lda     DGLEVEL
                beq     DGSUMM
                clr     FATBL0          the cache is now a lie
                lbsr    DGSPOT
                lda     DGLEVEL
                cmpa    #2
                blo     DGSUMM
                lbsr    DGSURF

* --- summary -----------------------------------------------------
DGSUMM          jsr     LB95C
                lda     DGFAIL
                bne     DGSUM1
                ldx     #DGMPASS
                lbra    DGPS
DGSUM1          clra
                ldb     DGFAIL
                lbsr    DGDEC
                ldx     #DGMFCNT
                lbra    DGPS

*******************************************************************
* The non-destructive ladder: name, routine, detail printer.
* A zero name ends the table; a zero detail printer means the
* one-line PASS/FAIL says all there is to say.
*******************************************************************
DGTAB           fdb     DGN1,DGT1,DGD1
                fdb     DGN2,DGT2,DGD2
                fdb     DGN3,DGT3,DGD3
                fdb     DGN4,DGT4,DGD45
                fdb     DGN5,DGT5,DGD45
                fdb     DGN6,DGT6,0
                fdb     DGN7,DGT7,DGD7
                fdb     0

                if      MOCK
*******************************************************************
* MOCK: there is no controller to interrogate, so say so rather
* than pretend.  A skip is not a failure and is not counted.
*******************************************************************
DGT1            equ     DGSKIP
DGT2            equ     DGSKIP
DGT3            equ     DGSKIP
DGT4            equ     DGSKIP
DGT5            equ     DGSKIP
DGT6            equ     DGSKIP
DGD1            equ     0
DGD2            equ     0
DGD3            equ     0
DGD45           equ     0

DGSKIP          ldx     #DGMSKIP
                lbsr    DGPS
                inc     DGSKIPF         this test printed its own result
                andcc   #$FE
                rts

                else
*******************************************************************
* T1 - BUS PRESENCE.  An all-ones read means nothing is driving the
* bus at all: no board, no power, or dead address decode.  The
* status value itself is reported by the status block further down.
*******************************************************************
DGT1            lda     BBSTAT
                sta     DGCODE
                cmpa    #$FF
                beq     DGT1BAD
                andcc   #$FE
                rts
DGT1BAD         orcc    #$01
                rts

DGD1            ldx     #DGMD1
                lbra    DGPS

*******************************************************************
* DGD45 - detail for the abort and FIFO reset tests.
*
* Both fail with "no completion", which only says the wait loop
* never saw a completion - not what it did see.  Read the status now
* and print it: $42 is a corrected parity error (which BBOKST
* accepts but the wait loops do not), $50 a latched timing error,
* $00 a completion that had already gone.
*******************************************************************
DGD45           ldx     #DGMDST
                lbsr    DGPS
                lda     BBSTAT
                lbsr    PRHEXA
                jmp     LB95C

*******************************************************************
* T2 - DATA BUS.  Walk $00/$FF/$AA/$55 through the address register
* LSB and accumulate every bit that failed to read back.  A nonzero
* mask names the broken data lines, which is the first thing worth
* knowing on a fresh board.  AR LSB is safe scratch - T6 reloads it.
*******************************************************************
DGT2            clr     DGEXP           stuck-bit accumulator
                ldu     #DGPATS
* The bound is tested after the pattern is used, not before: the
* post-increment has already stepped past the last entry by then,
* so testing first would silently drop $55 and with it the check
* for shorts between adjacent data lines.
DGT2A           ldb     ,u+
                pshs    u,b
                lda     #$0E            RAC = address register LSB
                sta     BBSTAT
                stb     BBDATA
                lda     #$0E            the write bumped RAC - put it back
                sta     BBSTAT
                lda     BBDATA
                puls    b
                pshs    b
                eora    ,s+
                ora     DGEXP
                sta     DGEXP
                puls    u
                cmpu    #DGPATE
                bls     DGT2A
                lda     DGEXP
                beq     DGT2OK
                orcc    #$01
                rts
DGT2OK          andcc   #$FE
                rts

DGPATS          fcb     $00,$FF,$AA,$55
DGPATE          equ     *-1

DGD2            ldx     #DGMD2
                lbsr    DGPS
                lda     DGEXP
                lbsr    PRHEXA
                jmp     LB95C

*******************************************************************
* T3 - REGISTER FILE.  Write all five parametric registers, set the
* address counter back, read all five.  This proves the counter
* auto-increments on reads as well as writes, which every command
* sequence in the driver depends on.
*******************************************************************
* Only three of the six registers can be read back.  Datasheet
* Table 3, "Address Assignments for the User-Accessible Registers":
*
*   $0A  UR      utility           read or write
*   $0B  BLR LSB block length LSB  WRITE ONLY
*   $0C  BLR MSB block length MSB  WRITE ONLY
*   $0D  ER      enable            WRITE ONLY
*   $0E  AR LSB  address LSB       read or write
*   $0F  AR MSB  address MSB       read or write
*
* A write-only register reads back $FF, so a read-back that starts
* at $0B fails against a perfectly good part.
DGT3            ldu     #DGREGS
DGT3R           lda     ,u              RAC address
                sta     BBSTAT
                ldb     1,u             value to write
                stb     BBDATA
                sta     BBSTAT          re-select: the write bumped RAC
                lda     BBDATA          read it back
                sta     DGGOT
                anda    2,u             only the bits that are ours
                pshs    a
                ldb     1,u
                andb    2,u
                cmpb    ,s+
                bne     DGT3BAD
                leau    3,u
                cmpu    #DGREGE
                bls     DGT3R
                lbsr    DGPARK
                andcc   #$FE
                rts

*******************************************************************
* DGPARK - leave the register address counter on a harmless
* register rather than on the FIFO.
*
* Five accesses from $0B walk the counter to $10, which is the FIFO
* - driver.asm relies on exactly that for transfers.  A register
* test must not leave it there: a stray read then pulls from an
* empty FIFO, which returns $FF and latches a timing error, and the
* next command's status comes back $50 instead of $40.
*******************************************************************
DGPARK          pshs    a
                lda     #$0E            address register LSB
                sta     BBSTAT
                puls    a,pc
DGT3BAD         leas    1,s             drop the pushed masked value
                ldb     1,u
                stb     DGEXP
                lda     ,u
                sta     DGOFF           report the RAC address itself
                lbsr    DGPARK
                lda     #E_DGREG
                orcc    #$01
                rts

* RAC address, value to write, mask of the bits that are ours.
* Different patterns so a crossed address line shows up as a wrong
* value rather than as a coincidence.
*
* The address register MSB is not a plain byte: the datasheet splits
* it into a bubble memory module select field and the page address.
* On the part, the module select bits read back with bit 7 set
* whatever was written ($12 reads back $92) while the page bits
* round-trip exactly, so only the page bits are tested.
DGREGS          fcb     $0A,$A5,$FF     utility register
                fcb     $0E,$34,$FF     address register LSB
                fcb     $0F,$12,$0F     address MSB, page bits only
DGREGE          equ     *-3

DGD3            ldx     #DGMD3
                lbsr    DGPS
                lda     DGOFF
                lbsr    PRHEXA
                lbra    DGDEG

*******************************************************************
* T4 - ABORT.  The first real command handshake: BUSY must rise and
* OP COMPLETE must follow.
*******************************************************************
* Use the driver's own completion test rather than an exact $40:
* FIFO READY rides along on an idle controller and is not an error.
DGT4            lbsr    BBABORT
                lbsr    BBDONE
                beq     DGT4OK
                orcc    #$01
                rts
DGT4OK          andcc   #$FE
                rts

*******************************************************************
* T5 - FIFO RESET.  Must return exactly $40 - the $42 parity status
* applies only to data commands and has no business here.
*******************************************************************
DGT5            lbsr    BBFFRST
                lbsr    BBDONE
                beq     DGT5OK
                orcc    #$01
                rts
DGT5OK          andcc   #$FE
                rts

*******************************************************************
* T6 - CONTROLLER INIT.  The whole sequence DOSINI runs.  If this
* passes, the bubble and its FSA are alive.
*******************************************************************
DGT6            lbsr    BBHWINI
                pshs    a
                lbsr    BBOKST
                puls    a
                beq     DGT6OK
                sta     BBLERR          leave BASIC knowing it is dead,
                lda     #$FF            so later commands give ?IO rather
                sta     BBNRDY          than pretending all is well
                lda     BBLERR
                orcc    #$01
                rts
DGT6OK          clr     BBNRDY          the bubble is usable again
                clr     BBLERR
                andcc   #$FE
                rts

*******************************************************************
* DGTRACE - not a test but a measurement.  Issue a bare ABORT and
* print the first four status reads back to back.
*
* On the part this cartridge uses the trace reads 80 40 40 40.
*
* BUSY on the first read, then OP COMPLETE which *persists* across
* three further reads.  This matters, because the datasheet says the
* bit "is reset whenever the status register is interrogated by the
* host" - on this part it plainly is not.  Since BBSEND accepts OP
* COMPLETE as command acknowledgement, a bit that cleared on read
* would have left every caller's wait loop polling for a completion
* that no longer existed.  It does not clear, so those loops are
* safe and the driver's polling pattern is sound.
*
* Keep this as a regression check: if a future part does clear the
* bit on read, the trace turns into 80 40 00 00 and the abort, FIFO
* reset and init wait loops all have to move to polling BUSY the way
* BBXFER already does.
*******************************************************************
DGTRACE         ldx     #DGMTRC
                lbsr    DGPS
                lda     #BBC_ABORT
                sta     BBSTAT
                ldb     #4
DGTR1           lda     BBSTAT
                pshs    b
                lbsr    PRHEXA
                lda     #$20            space
                jsr     LA282
                puls    b
                decb
                bne     DGTR1
                jmp     LB95C

*******************************************************************
* DGSTAT - not a pass/fail test but a report: print the status byte
* and name every bit that is set, so a half-working controller
* explains itself in English instead of in hex.
*******************************************************************
DGSTAT          ldx     #DGMSTAT
                lbsr    DGPS
                lda     BBSTAT
                sta     DGCODE
                lbsr    PRHEXA
                jsr     LB95C
                ldd     #DGSBTAB
                std     DGBPTR
DGST1           ldu     DGBPTR
                lda     ,u+
                beq     DGST9
                ldx     ,u++
                stu     DGBPTR
                anda    DGCODE
                beq     DGST1
                lbsr    DGPS
                bra     DGST1
DGST9           rts

* mask, name.  OP COMPLETE and FIFO READY are normal, not faults.
DGSBTAB         fcb     $80
                fdb     DGSB7
                fcb     $20
                fdb     DGSB5
                fcb     $10
                fdb     DGSB4
                fcb     $08
                fdb     DGSB3
                fcb     $04
                fdb     DGSB2
                fcb     $02
                fdb     DGSB1
                fcb     $00

                endc

*******************************************************************
* T7 - SECTOR READ.  Read the spot sectors through BBXFER.  Wholly
* non-destructive: whatever is on the media stays there.
*******************************************************************
DGT7            ldd     #DGSPTAB
                std     DGSPTR
DGT7A           ldu     DGSPTR
                ldd     ,u++
                stu     DGSPTR
                cmpd    #$FFFF
                beq     DGT7OK
                std     DGSEC
                std     BBLBA
                ldx     #DBUF0
                clrb                    read
                lbsr    BBXFER
                bcs     DGT7BAD
                bra     DGT7A
DGT7OK          andcc   #$FE
                rts
DGT7BAD         lda     BBLERR
                orcc    #$01
                rts

DGD7            lbra    DGDSEC

* first, second, middle and last physical sector
DGSPTAB         fdb     0
                fdb     1
                fdb     PHYSSEC/2
                fdb     PHYSSEC-1
                fdb     $FFFF

*******************************************************************
* T8 - destructive spot test.  Write all four sectors, THEN verify
* all four.  Writing and checking one at a time would miss an
* address line stuck so that two sectors alias onto the same
* storage; doing every write before any read will not.
*******************************************************************
DGSPOT          ldx     #DGN8
                lbsr    DGPS
                ldd     #DGSPTAB
                std     DGSPTR
DGSPW           ldu     DGSPTR
                ldd     ,u++
                stu     DGSPTR
                cmpd    #$FFFF
                beq     DGSPVS
                std     DGSEC
                lbsr    DGWSEC
                bcs     DGSPBAD
                bra     DGSPW
DGSPVS          ldd     #DGSPTAB
                std     DGSPTR
DGSPV           ldu     DGSPTR
                ldd     ,u++
                stu     DGSPTR
                cmpd    #$FFFF
                beq     DGSPOK
                std     DGSEC
                lbsr    DGVSEC
                bcs     DGSPBAD
                bra     DGSPV
DGSPOK          ldx     #DGMOK
                lbra    DGPS
DGSPBAD         lbra    DGDFAIL

*******************************************************************
* T9 - every sector, same two-pass discipline, with a dot every 32
* sectors so a long run visibly progresses.
*******************************************************************
DGSURF          ldx     #DGMSURF
                lbsr    DGPS
                ldd     #0
                std     DGSEC
DGSFW           lbsr    DGWSEC
                bcs     DGSFBAD
                lbsr    DGDOT
                ldd     DGSEC
                addd    #1
                std     DGSEC
                cmpd    #PHYSSEC
                blo     DGSFW
                ldx     #DGMVFY
                lbsr    DGPS
                ldd     #0
                std     DGSEC
DGSFV           lbsr    DGVSEC
                bcs     DGSFBAD
                lbsr    DGDOT
                ldd     DGSEC
                addd    #1
                std     DGSEC
                cmpd    #PHYSSEC
                blo     DGSFV
                ldx     #DGMSOK
                lbsr    DGPS
                ldd     #PHYSSEC
                lbsr    DGDEC
                ldx     #DGMSECT
                lbra    DGPS
DGSFBAD         lbra    DGDFAIL

* a dot every 32 sectors
DGDOT           ldd     DGSEC
                andb    #$1F
                bne     DGDOT9
                lda     #'.
                jmp     LA282
DGDOT9          rts

*******************************************************************
* DGWSEC / DGVSEC - write, and read-back-and-compare, the sector
* numbered in DGSEC.  Carry set on failure with ACCA = the code.
* LDA does not affect carry, so the error code loads safely after
* the branch that tested it.
*******************************************************************
DGWSEC          lbsr    DGFILL
                ldd     DGSEC
                std     BBLBA
                ldx     #DBUF0
                ldb     #1              write
                lbsr    BBXFER
                bcc     DGWS9
                lda     BBLERR
DGWS9           rts

DGVSEC          ldd     DGSEC
                std     BBLBA
                ldx     #DBUF0
                clrb                    read
                lbsr    BBXFER
                bcc     DGVS1
                lda     BBLERR
                rts
DGVS1           lbsr    DGCHK
                bcc     DGVS9
                lda     #E_DGDAT
DGVS9           rts

*******************************************************************
* The test pattern.  Bytes 0 and 1 carry the sector number and the
* rest is seeded from it, so a sector read back from the wrong place
* is caught by its contents rather than by luck.
*
* Accumulator-offset indexing is signed on a 6809, so the fill and
* compare loops walk a pointer instead of indexing off a base.
*******************************************************************
DGSEED          ldd     DGSEC
                pshs    a
                eorb    ,s+             seed = LSB EOR MSB
                stb     DGPAT
                rts

DGFILL          lbsr    DGSEED
                ldx     #DBUF0
                ldd     DGSEC
                sta     ,x+             byte 0 = sector MSB
                stb     ,x+             byte 1 = sector LSB
                lda     #2
DGFILL1         tfr     a,b
                eorb    DGPAT
                eorb    #$5A
                stb     ,x+
                inca
                bne     DGFILL1         index 2..255, then wraps to 0
                rts

DGCHK           lbsr    DGSEED
                ldx     #DBUF0
                ldd     DGSEC
                cmpa    ,x
                bne     DGCHKA
                leax    1,x
                cmpb    ,x
                bne     DGCHKB
                leax    1,x
                lda     #2
DGCHK2          tfr     a,b
                eorb    DGPAT
                eorb    #$5A
                cmpb    ,x
                bne     DGCHKC
                leax    1,x
                inca
                bne     DGCHK2
                andcc   #$FE
                rts
DGCHKA          sta     DGEXP
                clr     DGOFF
                bra     DGCHKG
DGCHKB          stb     DGEXP
                lda     #1
                sta     DGOFF
                bra     DGCHKG
DGCHKC          stb     DGEXP
                sta     DGOFF
DGCHKG          lda     ,x
                sta     DGGOT
                orcc    #$01
                rts

*******************************************************************
* Result reporting.
*******************************************************************
* DGSAY - ACCA = code.  Print FAIL, the code, and what the code
* means in English.  A bare hex number is not a diagnostic.
*
* The value is either one of the driver's own $E1-$EC site codes or,
* from the command tests, a 7220 status byte that was not the $40 we
* wanted - so anything not in the table is reported as exactly that.
DGSAY           sta     DGCODE
                inc     DGFAIL
                ldx     #DGMFAIL
                lbsr    DGPS
                lda     DGCODE
                lbsr    PRHEXA
                jsr     LB95C
                ldd     #DGECTAB
                std     DGBPTR
DGSAY1          ldu     DGBPTR
                lda     ,u+
                beq     DGSAY8          not a known code
                ldx     ,u++
                stu     DGBPTR
                cmpa    DGCODE
                bne     DGSAY1
                lbra    DGPS
DGSAY8          ldx     #DGEUNK
                lbra    DGPS

* code, meaning.  These are the driver's failure sites from
* driver.asm plus this file's own two.
DGECTAB         fcb     E_ABORT1
                fdb     DGE1
                fcb     E_ABORT2
                fdb     DGE2
                fcb     E_FFRST1
                fdb     DGE3
                fcb     E_FFRST2
                fdb     DGE4
                fcb     E_INIT1
                fdb     DGE5
                fcb     E_INIT2
                fdb     DGE6
                fcb     E_DGREG
                fdb     DGE7
                fcb     E_DGDAT
                fdb     DGE8
                fcb     E_XFER1
                fdb     DGEA
                fcb     E_XFER2
                fdb     DGEB
                fcb     E_XFER3
                fdb     DGEC
                fcb     $00

DGE1            fcc     '  ABORT: NO BUSY'
                fcb     $0D,$00
DGE2            fcc     '  ABORT: NO COMPLETION'
                fcb     $0D,$00
DGE3            fcc     '  FIFO RESET: NO BUSY'
                fcb     $0D,$00
DGE4            fcc     '  FIFO RESET: NO COMPLETE'
                fcb     $0D,$00
DGE5            fcc     '  INIT: NO BUSY'
                fcb     $0D,$00
DGE6            fcc     '  INIT: NO COMPLETION'
                fcb     $0D,$00
DGE7            fcc     '  REGISTER READBACK BAD'
                fcb     $0D,$00
DGE8            fcc     '  DATA MISMATCH'
                fcb     $0D,$00
DGEA            fcc     '  XFER: CMD NOT ACCEPTED'
                fcb     $0D,$00
DGEB            fcc     '  XFER: CONTROLLER IDLE'
                fcb     $0D,$00
DGEC            fcc     '  XFER: FIFO TIMEOUT'
                fcb     $0D,$00
DGEUNK          fcc     '  UNEXPECTED 7220 STATUS'
                fcb     $0D,$00

* DGDFAIL - as DGSAY, then whichever detail suits the code.
DGDFAIL         lbsr    DGSAY
                lda     DGCODE
                cmpa    #E_DGDAT
                beq     DGDDATA
                lbra    DGDSEC

* which sector was being worked on
DGDSEC          ldx     #DGMDSEC
                lbsr    DGPS
                ldd     DGSEC
                lbsr    DGDEC
                jmp     LB95C

* sector, offset, expected, got
DGDDATA         ldx     #DGMDSEC
                lbsr    DGPS
                ldd     DGSEC
                lbsr    DGDEC
                ldx     #DGMDOFF
                lbsr    DGPS
                lda     DGOFF
                lbsr    PRHEXA
DGDEG           ldx     #DGMEXP
                lbsr    DGPS
                lda     DGEXP
                lbsr    PRHEXA
                ldx     #DGMGOT
                lbsr    DGPS
                lda     DGGOT
                lbsr    PRHEXA
                jmp     LB95C

*******************************************************************
* DGPS - print the null-terminated string at X.  STRINOUT begins by
* skipping a byte, so back up one first.
*******************************************************************
DGPS            leax    -1,x
                jmp     STRINOUT

*******************************************************************
* DGDEC - print D in decimal.  Color BASIC already has the routine;
* it just does not preserve the registers.
*******************************************************************
DGDEC           pshs    a,b,x,y,u
                jsr     LBDCC
                puls    a,b,x,y,u,pc

*******************************************************************
* Messages.  Names are 19 characters including the trailing space,
* which leaves room for the widest result on a 32 column screen.
*******************************************************************
DGMHDR          fcb     $0D
                fcc     'BUBBLE DIAGNOSTIC'
                fcb     $0D,$00

DGN1            fcc     'T1 BUS PRESENCE .. '
                fcb     $00
DGN2            fcc     'T2 DATA BUS ...... '
                fcb     $00
DGN3            fcc     'T3 REGISTER FILE . '
                fcb     $00
DGN4            fcc     'T4 ABORT CMD ..... '
                fcb     $00
DGN5            fcc     'T5 FIFO RESET .... '
                fcb     $00
DGN6            fcc     'T6 CONTROLLER INIT '
                fcb     $00
DGN7            fcc     'T7 SECTOR READ ... '
                fcb     $00
DGN8            fcc     'T8 WRITE/VERIFY .. '
                fcb     $00

DGMOK           fcc     'OK'
                fcb     $0D,$00
DGMSKIP         fcc     'SKIP'
                fcb     $0D,$00
DGMFAIL         fcc     'FAIL '
                fcb     $00
DGMPASS         fcc     'ALL TESTS PASSED'
                fcb     $0D,$00
DGMFCNT         fcc     ' TEST(S) FAILED'
                fcb     $0D,$00

DGMD1           fcc     '  NOTHING DRIVING THE BUS:'
                fcb     $0D
                fcc     '  NO BOARD, POWER OR DECODE'
                fcb     $0D,$00
DGMD2           fcc     '  STUCK DATA BITS '
                fcb     $00
DGMD3           fcc     '  RAC '
                fcb     $00
DGMEXP          fcb     $0D
                fcc     '  WANT '
                fcb     $00
DGMGOT          fcc     ' GOT '
                fcb     $00
DGMDSEC         fcc     '  SECTOR '
                fcb     $00
DGMDOFF         fcc     ' OFFSET '
                fcb     $00

DGMSURF         fcc     'T9 FULL SURFACE'
                fcb     $0D
                fcc     '  WRITE '
                fcb     $00
DGMVFY          fcb     $0D
                fcc     '  VERIFY '
                fcb     $00
DGMSOK          fcb     $0D
                fcc     '  OK, '
                fcb     $00
DGMSECT         fcc     ' SECTORS'
                fcb     $0D,$00

                if      MOCK
                else
DGMDST          fcc     '  STATUS NOW '
                fcb     $00
DGMTRC          fcc     'ABORT TRACE '
                fcb     $00
DGMSTAT         fcc     'STATUS '
                fcb     $00
DGSB7           fcc     '  BUSY STUCK HIGH'
                fcb     $0D,$00
DGSB5           fcc     '  OP FAIL'
                fcb     $0D,$00
DGSB4           fcc     '  TIMING ERROR'
                fcb     $0D,$00
DGSB3           fcc     '  CORRECTABLE ERROR'
                fcb     $0D,$00
DGSB2           fcc     '  UNCORRECTABLE ERROR'
                fcb     $0D,$00
DGSB1           fcc     '  PARITY ERROR'
                fcb     $0D,$00
                endc
