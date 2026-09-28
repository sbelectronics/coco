*******************************************************************
* hooks.asm - the RAM vector handlers
*
* Color BASIC calls these 25 slots at strategic points and leaves
* them as RTS.  Extended BASIC claims a few; Disk BASIC claims most
* of the rest.  A hook that must fall through to the handler that
* was there before chains through an address saved at cold start
* rather than a hardcoded Extended BASIC address, so that both ECB
* 1.0 and 1.1 work.
*
* Convention: a hook that takes over the operation drops Color
* BASIC's return address with LEAS 2,S and never comes back; a hook
* that declines simply returns.
*
* Every hook is entered with the device number in DEVNUM.  A
* positive DEVNUM is a file on this ROM; anything else belongs to the
* console, the cassette or the printer and is not ours.
*******************************************************************

*******************************************************************
* DVEC12 - the do-nothing hook (RVEC12 and RVEC16 ON ERROR GOTO)
*******************************************************************
DVEC12          rts

*******************************************************************
* DVEC0 - the OPEN command
*
* OPEN "mode",#n,"filename"[,reclen]
*******************************************************************
DVEC0           leas    2,s             OPEN is ours from here on
                jsr     LB156           evaluate the mode expression
                jsr     LB6A4           its first character
                pshs    b
                jsr     LA5A2           the device number
                tstb
                lble    LA603           not a file - Color BASIC's OPEN
                puls    a               the mode
                pshs    b,a             keep the mode and file number
                clr     DEVNUM          messages go to the screen
                jsr     SYNCOMMA
                ldx     #DATEXT         files default to a .DAT extension
                lbsr    NAMEGET
                ldd     #$01FF          data file, ASCII
                std     DFLTYP
                ldx     #SECLEN         the default record length
                jsr     GETCCH
                beq     DVEC0R
                jsr     SYNCOMMA
                jsr     LB3E6           evaluate the record length
                ldx     FPA0+2
DVEC0R          stx     DFFLEN
                lbeq    LB44A           ?FC on a zero length record
                jsr     LA5C7           nothing else may follow
                puls    a,b             mode, file number
                lbsr    OPENFCB
                rts

*******************************************************************
* OPENFCB - open a file on a given FCB.
*   Entry: ACCA = mode 'I', 'O' or 'D', ACCB = file number
*          the name is already parsed into DNAMBF/DEXTBF
*
* SAVE and LOAD reach this directly, using the system FCB.
*******************************************************************
OPENFCB         pshs    a
                lbsr    GETFCBB         X = the FCB, flags from its type
                lbne    LA61C           ?AO - it is already open
                stx     FCBTMP
                puls    a
                cmpa    #'I
                beq     OPENFI
                cmpa    #'O
                beq     OPENFO
                cmpa    #'D             direct access
                beq     OPENFD
                cmpa    #'R             Disk BASIC takes either letter
                beq     OPENFD
                jmp     LA616           ?FM - bad file mode
OPENFI          ldx     FCBTMP
                lbsr    FCBOPENI
                rts
OPENFO          ldx     FCBTMP
                lbsr    FCBOPENO
                rts
OPENFD          ldx     FCBTMP
                lbsr    FCBOPEND
                rts

*******************************************************************
* OPENSYS - open the system FCB, the one above the last user file.
*   Entry: ACCA = 'I' or 'O'
*******************************************************************
OPENSYS         ldb     FCBACT
                incb
                stb     DEVNUM
                lbra    OPENFCB

*******************************************************************
* DVEC1 - device number validity
* Entry with the number in ACCB; ours are 1..FCBACT.
*******************************************************************
DVEC1           tstb
                ble     DVEC1X
                cmpb    FCBACT
                bhi     DVEC1X          Color BASIC will reject it
                leas    2,s             it is one of ours - accept it
DVEC1X          rts

*******************************************************************
* DVEC2 - set print parameters.  The values are Disk BASIC's exactly:
* a print width of 256, so nothing wraps, but a 16 column tab field,
* so a comma in a PRINT# still pads out to the next field as it does on
* the screen.  That is why data is written with an explicit separator -
* PRINT#1,A$;",";B - rather than relying on the comma.
*******************************************************************
* PRTDEV has to be cleared here: Color BASIC reads it straight
* afterwards to decide which terminators end a string being read,
* and a file is not the cassette.
DVEC2           tst     DEVNUM
                ble     DVEC2X
                leas    2,s
                pshs    x,b,a
                clr     PRTDEV          a file, not the cassette
                lbsr    GETFCB
                ldb     FCBPOS,x        the print position
                clra                    width 256: no wrapping
                ldx     #$1000          tab field width and tab zone
                jmp     LA37C           store the print parameters
DVEC2X          rts

*******************************************************************
* DVEC3 - console out.  Send the byte in ACCA to the file.
*******************************************************************
* A file open for INPUT swallows the byte and writes nothing.  BASIC
* echoes every character it reads, so during an ASCII load - where
* DEVNUM points at the file being read - the echo arrives here.
* Writing it would advance the same buffer pointer the read had just
* advanced: two bytes consumed for every one delivered, and once the
* pointer wrapped, the buffer flushed back over the file.
*
* The print position is kept here too, as Disk BASIC does it: a CR
* restarts it and control codes do not advance it.  DVEC2 hands it
* to Color BASIC as the column for PRINT# comma zones.
DVEC3           tst     DEVNUM
                ble     DVEC3X
                leas    2,s
                pshs    b,x
                lbsr    GETFCB
                ldb     FCBTYP,x
                cmpb    #INPFIL
                beq     DVEC3D          reading, not writing: drop it
                cmpa    #$0D
                bne     DVEC3P
                clr     FCBPOS,x
                bra     DVEC3W
DVEC3P          cmpa    #$20
                blo     DVEC3W          a control code, no column change
                inc     FCBPOS,x
DVEC3W          cmpb    #RANFIL         ACCB still holds the file type
                beq     DVEC3R
                lbsr    FCBPUTB
DVEC3D          puls    b,x
DVEC3X          rts

* A direct file takes printed bytes into the record it is holding, at
* the position PRINT# has reached within it; the record reaches the
* media when PUT is used.  Running off the end of the record is ?ER -
* there is nowhere for the byte to go, and dropping it would lose data
* without saying so.
DVEC3R          pshs    a               the byte
                ldd     FCBPUT,x
                addd    #1
                cmpd    FCBRLN,x
                bhi     DVEC3ER
                std     FCBPUT,x
                ldx     FCBBUF,x        X, not U: the sequential path
                leax    d,x             leaves U alone and so must this
                puls    a
                sta     -1,x
                puls    b,x
                rts
DVEC3ER         leas    1,s             the byte, which has nowhere to go
                puls    b,x
                ldb     #ERRER
                jmp     LAC46           ?ER - past the end of the record

*******************************************************************
* DVEC4 - console in.  Fetch the next byte of the file into ACCA
* and raise CINBFL at the end of the file.
*******************************************************************
DVEC4           tst     DEVNUM
                ble     DVEC4X
                leas    2,s
                pshs    b,x
                clr     CINBFL
                lbsr    GETFCB
                ldb     FCBTYP,x
                cmpb    #RANFIL
                beq     DVEC4R
                lbsr    FCBGETB
                bcc     DVEC4A
                com     CINBFL          nothing left
DVEC4A          puls    b,x
DVEC4X          rts

* A direct file hands out the bytes of the record it is holding, one at
* a time.  Reaching the end of the record is the end of the input, the
* same as a sequential file reaching the end of its data, rather than
* an error: INPUT# is expected to stop there.
DVEC4R          ldd     FCBGET,x
                cmpd    FCBRLN,x
                bhs     DVEC4E
                addd    #1
                std     FCBGET,x
                ldx     FCBBUF,x        X is restored on the way out; U
                leax    d,x             would not be
                lda     -1,x
                bra     DVEC4A
DVEC4E          com     CINBFL
                bra     DVEC4A

*******************************************************************
* DVEC5 / DVEC6 - INPUT and PRINT device checks: the file has to be
* open in a mode that allows what is being asked of it.
*******************************************************************
DVEC5           lda     #INPFIL
                bra     DVEC56
DVEC6           lda     #OUTFIL
DVEC56          tst     DEVNUM
                ble     DVEC56X
                pshs    a,b,x
                lbsr    GETFCB
                lbeq    DVEC56NO        not open at all
                ldb     FCBTYP,x
                cmpb    #RANFIL
                beq     DVEC56Y         a random file allows both
                cmpb    ,s
                bne     DVEC56BAD
DVEC56Y         puls    a,b,x
                leas    2,s
DVEC56X         rts
DVEC56BAD       puls    a,b,x
                jmp     LA616           ?FM - bad file mode
DVEC56NO        puls    a,b,x
                jmp     LA3FB           ?NO - file not open

*******************************************************************
* DVEC7 - close every file.  Also called directly on warm start.
*******************************************************************
DVEC7           lbsr    CLOSEALL
                rts

*******************************************************************
* DVEC8 - close one file.
*******************************************************************
DVEC8           tst     DEVNUM
                ble     DVEC8X
                leas    2,s
                pshs    b,x
                lbsr    GETFCB
                lbsr    FCBCLOSE
                puls    b,x
                clr     DEVNUM
                rts
DVEC8X          jmp     [XV8SAV]        the displaced handler

*******************************************************************
* DVEC10 - INPUT# reads one item out of the file.
*
* Color BASIC's INPUT expects a line already sitting in LINBUF, so
* this collects one item there and then redirects the return into
* the middle of BASIC's INPUT, which parses it.  An item ends at a
* comma (or a space too, for a numeric variable), at a carriage
* return, or at the end of the file; a quoted string swallows
* separators until its closing quote.
*******************************************************************
DVEC10          tst     DEVNUM
                ble     DVEC10X
                ldx     #LB069          come back inside BASIC's INPUT
                stx     ,s
                ldx     #LINBUF+1
                ldb     #',             the item separator
                stb     CHARAC
                lda     VALTYP
                bne     DVEC10A         a string variable
                ldb     #$20            for numbers a space ends it too
* The first character is fetched with the erroring reader: asking a
* file that is already finished for another item is ?IE, whereas
* running out part way through one just ends the item.
DVEC10A         bsr     GETFCE
                cmpa    #$20
                beq     DVEC10A         ignore leading blanks
                cmpa    #'"
                bne     DVEC10C
                cmpb    #',
                bne     DVEC10C
                tfr     a,b             a quoted string: the quote is
                stb     CHARAC          now what terminates it
                bra     DVEC10S
DVEC10C         cmpb    #'"
                beq     DVEC10D
                cmpa    #$0D
                bne     DVEC10D
                cmpx    #LINBUF+1
                beq     DVEC10CR
                lda     -1,x            a CR after a LF is real data
                cmpa    #$0A
                bne     DVEC10CR
                lda     #$0D
DVEC10D         tsta
                beq     DVEC10N         nulls are ignored
                cmpa    CHARAC
                beq     DVEC10E
                pshs    b
                cmpa    ,s+
                beq     DVEC10E
DVEC10S         sta     ,x+
                cmpx    #LINBUF+LBUFMX
                bne     DVEC10N
                bsr     GETFC           the buffer is full
                bne     DVEC10Z
                bra     DVEC10F
DVEC10N         bsr     GETFC
                beq     DVEC10C
DVEC10Z         clr     ,x              terminate the item
                ldx     #LINBUF
DVEC10X         rts

* the item ended - swallow the separator sequence that follows it
DVEC10E         cmpa    #'"
                beq     DVEC10G
                cmpa    #$20
                bne     DVEC10Z
DVEC10G         bsr     GETFC
                bne     DVEC10Z
                cmpa    #$20
                beq     DVEC10G
                cmpa    #',
                beq     DVEC10Z
DVEC10F         cmpa    #$0D
                bne     DVEC10H
DVEC10CR        bsr     GETFC
                bne     DVEC10Z
                cmpa    #$0A            CR LF counts as one terminator
                beq     DVEC10Z
DVEC10H         bsr     UNGETFC         it belongs to the next item
                bra     DVEC10Z

*******************************************************************
* GETFC - read one character of the file into ACCA.
* Returns Z set when a character was actually available.
*******************************************************************
GETFC           jsr     LA176           console in, redirected by DVEC4
                tst     CINBFL
                rts

*******************************************************************
* GETFCE - the same, but reading past the end of the file is ?IE.
*******************************************************************
GETFCE          bsr     GETFC
                beq     GETFCE9
                ldb     #ERRIE
                jmp     LAC46           ?IE - input past end of file
GETFCE9         rts

*******************************************************************
* UNGETFC - put the character in ACCA back for the next read.
* One character of pushback is all the item parser ever needs.
*******************************************************************
UNGETFC         pshs    a,b,x
                lbsr    GETFCB
                sta     FCBCDT,x
                ldb     #$FF
                stb     FCBCFL,x
                puls    a,b,x,pc

*******************************************************************
* DVEC11 / DVEC13 - break check and line input termination; neither
* applies while the "console" is a file.
*******************************************************************
DVEC11          tst     DEVNUM
                ble     DVEC11X
                leas    2,s
DVEC11X         rts

* DVEC13 is reached when the direct mode loop finds console in
* empty, which during an ASCII load means the file has ended.
DVEC13          tst     DEVNUM
                lbgt    LOADFIN         finish the load
                rts

*******************************************************************
* DVEC14 - EOF(n)
* Returns 0 while data remains and -1 once the file is exhausted.
*******************************************************************
* The device number has not been parsed yet when this hook runs, so
* EOF does that itself and then hands ACCB (zero = data remains) to
* Color BASIC, which turns it into 0 or -1.
DVEC14          leas    2,s
                lda     DEVNUM
                pshs    a
                jsr     LA5AE           strip the device number
                jsr     LA3ED           it has to be open for input
                tst     DEVNUM
                lble    LA5DA           not one of ours
                lbsr    GETFCB
                ldb     FCBTYP,x
                cmpb    #RANFIL
                lbeq    LA616           ?FM - not on a random file
                ldb     FCBDFL,x        non-zero once exhausted
                jmp     LA5E4           back into Color BASIC's EOF

*******************************************************************
* DVEC18 - RUN "file"
*
* RUN followed by a quoted string loads the file and runs it; RUN
* with anything else belongs to whoever owned this hook before.
* The abandoned return address on the stack does not matter: every
* load path passes through LAD19, which resets the stack pointer.
*******************************************************************
DVEC18          cmpa    #'"
                beq     DVEC18F
                jmp     [XV18SAV]       plain RUN - the old handler
DVEC18F         lda     #2              run it, but do not close files
                clrb                    (and it is not a merge)
                lbra    LOADGO

*******************************************************************
* DVEC15 - expression evaluation.
*
* Extended BASIC's handler is what parses the &H, &O and &B
* constants, so this hook must reach it or those vanish from the
* language with no error to say so.
*
* The address is hardcoded rather than chained, which is what Disk
* BASIC does, and here it is the only thing that works: Extended
* BASIC does not install this slot until L96EC runs, and cold start
* calls L96EC after the hooks are in.  Saving the slot's contents
* beforehand captures the RTS Color BASIC left there.
*
* Before that, the LET modifier below gets a look in, as it does in
* Disk BASIC: an assignment whose value came out of a record buffer
* has to be copied into string space first.
*******************************************************************
* The hook fires for every expression BASIC evaluates, so the test for
* "this one is the right hand side of a LET" has to be cheap and exact.
* It is made from the two return addresses already on the stack: LET
* calls expression evaluation at a known place, through a known
* intermediate.  Anything else drops straight through to Extended
* BASIC, which is the common case by a wide margin.
DVEC15          lda     4,s             the stacked precedence flag
                bne     DVEC15X         mid-expression, not a fresh one
                ldx     5,s
                cmpx    #LAF9A          called from LET?
                bne     DVEC15X
                ldx     2,s
                cmpx    #LB166          through expression evaluation?
                bne     DVEC15X
                ldx     #LETMOD         come back to us when it returns
                stx     5,s
DVEC15X         jmp     XVEC15

*******************************************************************
* LETMOD - the LET modifier.
*
* A FIELDed string's bytes live in a record buffer, not in string
* space.  Assigning one to another variable would hand that variable a
* pointer into the record, so it would change underneath when the next
* GET arrived and would dangle when the file was closed.  Copying the
* value into string space here is what makes the assignment mean what
* it says.
*
* Only the value being assigned is examined.  Assigning *to* a FIELDed
* variable is a different thing and is not caught here: it repoints the
* variable out of the record, which is why LSET and RSET exist.
*******************************************************************
LETMOD          puls    a               the variable type
                rora                    carry set for a string
                jsr     LB148           ?TM unless the two agree
                lbeq    LBC33           a number: pack FPA0 into the variable
                ldx     FPA0+2          the string descriptor
                ldd     2,x             where its bytes are
                cmpd    #DFLBUF
                blo     LETMOD9
                subd    FCBADR
                lbcs    LAFB1           inside a record buffer: copy it out
LETMOD9         jmp     LAFA4           on into BASIC's own LET

*******************************************************************
* DVEC17 - the error driver
*
* Reached for every BASIC error.  Ours are numbered 27 and up;
* anything below that belongs to the displaced handler.  The
* return address stays on the stack across the test so that the
* chained handler sees the stack it expects.
*******************************************************************
DVEC17          puls    y               return address into Y
                jsr     LAD33           reset the CONT flag etc.
* An error part way through a crunched LOAD leaves half a program
* in memory with stale pointers, so throw it away, exactly as Disk
* BASIC's DLODFL mechanism does.  This must happen before anything
* is pushed: LAD19 resets the stack pointer.
                lda     DLODFL
                beq     DVEC17B
                clr     DLODFL
                jsr     LAD19           NEW
DVEC17B         pshs    y,b             return address and error number
* If the error was raised while CLOSEALL was already running -
* a failing write during a close, say - closing again would just
* raise the same error and loop.  Skip the close but always clear
* the guard, so the file system recovers for the next command.
                tst     CLOSING
                bne     DVEC17A
                jsr     DKCLOS          close files, reset the file system flags
DVEC17A         clr     CLOSING
                puls    b               error number back
                cmpb    #ERRLOWEST
                blo     DVEC17X         not ours - pass it on
                leas    2,s             purge the return address
                jsr     LA7E9           turn off the cassette motor
                jsr     LA974           disable the analog multiplexer
                clr     DEVNUM          report on the screen
                jsr     LB95C           CR
                jsr     LB9AF           '?'
                ldx     #ERRTAB-ERRLOWEST
                jmp     LAC60           Color BASIC's error handler
DVEC17X         jmp     [XV17SAV]

*******************************************************************
* ERRTAB - the error messages, indexed by (code*2 - ERRLOWEST)
*******************************************************************
* Extended BASIC's two extra codes are carried here as well.  ECB
* raises them through its own path with its own table, but anything
* that raises them the ordinary way arrives here, and Color BASIC's
* table stops at 24.
ERRTAB          fcc     'UF'            25 undefined function call
                fcc     'NE'            26 file not found
                fcc     'BR'            27 bad record
                fcc     'DF'            28 disk full
                fcc     'OB'            29 out of buffer space
                fcc     'WP'            30 write protected
                fcc     'FN'            31 bad file name
                fcc     'FS'            32 bad file structure
                fcc     'AE'            33 file already exists
                fcc     'FO'            34 field overflow
                fcc     'SE'            35 set to non-fielded string
                fcc     'VF'            36 verification error
                fcc     'ER'            37 write or input past end of record

*******************************************************************
* DVEC22 - GET and PUT on a direct access file.
*
* This is the CLS hook.  Extended BASIC's GET/PUT parser shares a code
* path with CLS, so the hook fires part way through parsing a GET or a
* PUT.  It is ours only when the caller is that parser and the next
* character is '#'; CLS itself, and graphics GET/PUT, return from here
* and carry on exactly as before.
*
* Entry is a JSR, so the stacked return address identifies the caller.
* Both Extended BASIC 1.0 and 1.1 are recognised, as elsewhere.
*******************************************************************
DVEC22          pshs    x,cc
                ldx     3,s             who called
                cmpx    #ECBGETPUT11
                beq     DVEC22A
                cmpx    #ECBGETPUT10
                bne     DVEC22X
DVEC22A         cmpa    #'#             a file, not a graphics array
                bne     DVEC22X
                leas    5,s             CC, X and the return address
                lbra    GETPUT          this statement is ours now
DVEC22X         puls    cc,x,pc

*******************************************************************
* GETFCB  - point X at the FCB for the file number in DEVNUM.
* GETFCBB - the same, with the file number already in ACCB.
*
* Returns X = FCB pointer with the condition codes set from the
* file type (Z set means the file is closed).  ACCB is preserved:
* PULS does not disturb CC, so the flags from the LDB survive.
*******************************************************************
GETFCB          pshs    b
                ldb     DEVNUM
                bra     GETFCB1
GETFCBB         pshs    b
GETFCB1         aslb                    two bytes per pointer
                ldx     #FCBV1-2
                ldx     b,x
                ldb     FCBTYP,x        sets the flags
                puls    b,pc

*******************************************************************
* SAVEVEC - remember the handler currently installed in a RAM
* vector so that the replacement can chain to it.
*   Entry: X = the RVEC slot, Y = where to save the address
*
* If nothing was installed (Color BASIC leaves an RTS), the address
* of an RTS is recorded, which makes chaining equivalent to declining.
*******************************************************************
* If the slot already points into this cartridge then an earlier
* initialisation put it there, and capturing it would make the hook
* chain to itself - an error inside the error handler, forever.
SAVEVEC         lda     ,x
                cmpa    #$7E            was it a JMP?
                bne     SAVEVEC0
                ldd     1,x
                cmpd    #$C000
                blo     SAVEVEC1        a genuine earlier handler
SAVEVEC0        ldd     #DVEC12         nothing there - chain to an RTS
SAVEVEC1        std     ,y
                rts
