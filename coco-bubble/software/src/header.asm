*******************************************************************
* header.asm - ROM header, cold start, warm start, banner
*
* Color BASIC's reset code looks for 'DK' at $C000 and jumps to
* $C002.  That is Disk BASIC's autostart mechanism and it is ours.
* The four vectors that follow are the sanctioned entry points for
* machine language software.
*******************************************************************

                org     $C000

DOSBAS          fcc     'DK'            cold start signature
                bra     INIT            $C002 - Color BASIC jumps here

DCNVEC          fdb     DSKCON          $C004 - sector I/O routine
DSKVAR          fdb     DCOPC           $C006 - DSKCON variable block
DSINIT          fdb     DOSINI          $C008 - re-initialise the hardware
DOSVEC          fdb     DOSCOM          $C00A - DOS command handler

*******************************************************************
* INIT - cold start
*
* Mirrors DECB's LC00C step for step so that the resulting machine
* state is one that DECB-era software recognises.
*******************************************************************
INIT
* --- 1. clear the file system RAM ---------------------------------
* Up to DFLBUF, which leaves the record buffers and the FCBs alone
* but does take in the ROM's own private block just below it.
                ldx     #DBUF0
INIT1           clr     ,x+
                cmpx    #DFLBUF
                bne     INIT1

* --- 2. install the command interpretation stub -------------------
                ldx     #CMDSTUB        ROM image of the stub
                ldu     #CMDTBL_DSK     RAM home of the stub
                ldb     #10
                jsr     LA59A           move B bytes from X to U
* Initialise the user table so an unknown token errors cleanly and
* the table chain terminates.
                ldd     #LB277          syntax error
                std     3,u             user command jump vector
                std     8,u             user secondary jump vector
                clr     ,u              0 reserved words (table absent)
                clr     5,u             0 secondary functions

* --- 3. interpose on Extended BASIC's handlers --------------------
* PMODE has to account for the RAM claimed here, DLOAD has to close
* files first, and POS() has to understand random files.
                ldd     #DXCVEC
                std     COMVEC+13
                ldd     #DXIVEC
                std     COMVEC+18

* --- 4. install the RAM hooks -------------------------------------
* Three of the hooks fall through to whatever was installed before
* this ROM, so those targets are recorded first.  Recording them
* rather than hardcoding Extended BASIC addresses keeps both ECB
* 1.0 and 1.1 working.  RVEC15 cannot be done this way - see DVEC15.
                pshs    x
                ldx     #RVEC8
                ldy     #XV8SAV
                lbsr    SAVEVEC
                ldx     #RVEC17
                ldy     #XV17SAV
                lbsr    SAVEVEC
                ldx     #RVEC18
                ldy     #XV18SAV
                lbsr    SAVEVEC
                if      MOCK
                else
* Hook the NMI so the floppy driver can escape its transfer loop.
* Harmless when no controller is present: FDCNMI checks NMIFLG and
* returns untouched unless a transfer set it.
                lda     #$7E            JMP opcode
                sta     NMIVEC
                ldx     #FDCNMI
                stx     NMIVEC+1
                clr     NMIFLG
                clr     RDYTMR
                clr     DRGRAM
                endc
* Point the IRQ at Extended BASIC's handler, which is what counts
* TIMER.  Color BASIC's own handler does not, and its is what the
* vector still holds at this point: a cartridge reached through the DK
* hook runs instead of Extended BASIC's cold start, not after it.
*
* Disk BASIC installs a handler of its own that counts the floppy motor
* down and then enters this one at $8955, part way in - past the point
* where it has already cleared the interrupt flag for itself.  There is
* no motor timer in this ROM, so the vector goes to $894C, the entry
* Extended BASIC uses when nothing has displaced it, and Extended BASIC
* clears the flag as it always would.
                lda     #$7E            JMP opcode
                sta     IRQVEC
                ldx     #L894C
                stx     IRQVEC+1
* And the interrupt itself has to be switched on at the PIA.  Reaching
* this ROM through the DK hook means Extended BASIC's cold start never
* runs, and that is what would otherwise enable it: a machine booted
* this way comes up with $FF03 bit 0 clear, where a Disk BASIC machine
* has it set.  Only that one bit differs, so it is set rather than the
* whole register written, leaving the CB2 sound mux bits alone.
*
* Without this TIMER reads zero forever and every program that times
* anything quietly stops working.
                lda     PIA0CRB
                ora     #$01            VSYNC, 60 times a second
                sta     PIA0CRB
                clr     CLOSING         power-on RAM is not zero: without
*                                       this a stray byte here would
*                                       silently disable every file close
                puls    x
* X still points just past CMDSTUB, at the vector image table.
* RVEC19 onward are left exactly as found, as Disk BASIC 1.1 leaves
* them, except RVEC22 below.
                ldu     #RVEC0
INIT2           lda     #$7E            JMP opcode
                sta     ,u+
                ldd     ,x++
                std     ,u++
                cmpx    #VECEND
                bne     INIT2

* RVEC22 is the CLS hook, and it is Disk BASIC's way in to GET and PUT
* on a direct file: Extended BASIC's GET/PUT parser shares a code path
* with CLS, so the hook fires part way through it.  It is installed
* separately because it is not adjacent to the others - RVEC19 to
* RVEC21 have to be left as found.
                lda     #$7E            JMP opcode
                sta     RVEC22
                ldx     #DVEC22
                stx     RVEC22+1

* --- 5. relocate the USR vector table -----------------------------
* Disk BASIC moves USR vectors out of $013E (which the user command
* table now occupies) to DUSRVC, and points them all at ?FC.
                ldx     #DUSRVC
                stx     USRADR
                ldu     #LB44A          'illegal function call'
                ldb     #10
INIT3           stu     ,x++
                decb
                bne     INIT3

* --- 6. file system variables -------------------------------------
                lda     #19             FAT write-back trigger
                sta     WFATVL
                clr     FATBL0          no active files on any FAT
                clr     FATBL1
                clr     FATBL2
                clr     FATBL3
                lda     #$FF
                sta     DVERFL          VERIFY ON by default, as DECB
                ldx     #DFLBUF
                stx     RNBFAD          start of free random buffer area
                leax    SECLEN,x        reserve one record buffer
                stx     FCBADR          FCBs start here
                leax    1,x
                stx     FCBV1           FCB 1
                clr     FCBTYP,x        closed
                leax    FCBLEN,x
                stx     FCBV1+2         FCB 2
                clr     FCBTYP,x        closed
                leax    FCBLEN,x
                stx     FCBV1+4         system FCB
                clr     FCBTYP,x        closed
                lda     #2
                sta     FCBACT          2 user FCBs (FILES can change it)
                leax    FCBLEN,x        one past the end of the system FCB

* --- 7. hand the rest of RAM to BASIC -----------------------------
                tfr     x,d
                tstb                    on a 256 byte boundary?
                beq     INIT4
                inca                    no - round up
INIT4           bita    #$01            on an even (graphics page) boundary?
                beq     INIT5
                inca                    no - round up
INIT5           tfr     a,b
                addb    #$18            room for 4 graphics pages
                stb     TXTTAB          new start of BASIC
                if      MOCK
* The MOCK build keeps its pseudo-disk in RAM, so pull the top of
* memory down below it before BASIC lays out its string space.
* ACCA still holds the graphics start page for L96EC below and has
* to survive this.
                pshs    a
                ldd     #MOCKBUF
                std     TOPRAM
                puls    a
                endc
                jsr     L96EC           init ECB variables and do a NEW
*                                       (entered with ACCA = the page
*                                       where the graphics pages begin)
                lda     BEGGRP
                adda    #$06            one graphics page = 6 x 256
                sta     ENDGRP

* --- 8. bring up the bubble ---------------------------------------
                jsr     [DSINIT]

* --- 9. announce ourselves ----------------------------------------
                andcc   #$AF            interrupts back on
                ldx     #BANNER-1       STRINOUT skips the first byte
                jsr     STRINOUT
                jsr     BLDMSG          which image is in the socket
                jsr     MPIMSG          where the bubble and any FDC are
                jsr     BBSTMSG         "128K BUBBLE READY" or an error

* --- 10. arm the warm start and return to BASIC -------------------
                ldx     #DKWMST
                stx     RSTVEC
                jmp     LA0E2

*******************************************************************
* DKWMST - warm start (reset button)
*
* The NOP is the warm start indicator Color BASIC checks for.
*******************************************************************
DKWMST          nop
                jsr     DOSINI          re-initialise the bubble
                jsr     DKCLOS          close files, reset flags
                jmp     XBWMST          Extended BASIC's warm start

*******************************************************************
* DKCLOS - close all files and reset the file system variables.
* Called on warm start and by UNLOAD.  Clobbers A, B, X.
*******************************************************************
DKCLOS          jsr     CLOSEALL        close every open FCB
                clr     DRESFL
                clr     DLODFL
                clr     DMRGFL
                clr     DRUNFL
                clr     DEFDRV
                clr     FATBL0          invalidate the FAT image
                rts

*******************************************************************
* BBSTMSG - print the bubble status line.
*******************************************************************
BBSTMSG         lda     BBNRDY
                bne     BBSTBAD
                ldx     #MSGREADY-1
                jmp     STRINOUT
BBSTBAD         ldx     #MSGNRDY-1
                jsr     STRINOUT
                lda     BBLERR          the site-specific failure code
                jsr     PRHEXA
                jmp     LB95C           CR

*******************************************************************
* BLDMSG - print the build stamp.
*
* BUILDID is a checksum of the sources, handed in by the Makefile,
* so two machines showing the same four digits are running the same
* image.  A ROM assembled without the Makefile shows 0000.
*******************************************************************
BLDMSG          ldx     #MSGBUILD-1
                jsr     STRINOUT
                lda     #(BUILDID/256)&255
                jsr     PRHEXA
                lda     #BUILDID&255
                jsr     PRHEXA
                jmp     LB95C           CR

*******************************************************************
* PRHEXA - print ACCA as two hex digits.
*******************************************************************
PRHEXA          pshs    a
                lsra
                lsra
                lsra
                lsra
                bsr     PRHEXD
                puls    a
                anda    #$0F
PRHEXD          cmpa    #10
                blo     PRHEXD1
                adda    #7
PRHEXD1         adda    #'0
                jmp     LA282           send character to console out

*******************************************************************
* Messages
*******************************************************************
BANNER          fcb     $0D
                fcc     'BUBBLE EXTENDED COLOR BASIC 1.0'
                fcb     $0D
                fcc     'INTEL 7110 BUBBLE CARTRIDGE'
                fcb     $0D
                fcc     'BY SCOTT BAKER'
                fcb     $0D,$00

MSGBUILD        fcc     'BUILD '
                fcb     $00

MSGREADY        fcc     '128K BUBBLE READY'
                fcb     $0D,$0D,$00

MSGNRDY         fcc     'BUBBLE NOT READY E='
                fcb     $00
