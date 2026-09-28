*******************************************************************
* cmds.asm - BASIC command implementations
*
* Anything not implemented reports ?FC rather than pretending to
* work.  BACKUP and DOS are not built, and will not be.  The direct
* access file commands are split: FIELD, LSET, RSET and the GET/PUT
* engine live with the FCB code in fcb.asm; FILES, LOC, LOF, CVN and
* MKN$ are here.
*******************************************************************

NOTIMP          jmp     LB44A           ?FC - not implemented

BACKUP          equ     NOTIMP
DOSCMD          equ     NOTIMP


*******************************************************************
* DOSCOM - the DOS command entry, reached through $C00A.
*******************************************************************
DOSCOM          jmp     LB44A

*******************************************************************
* Small shared helpers
*******************************************************************

* PRCHR - send the character in ACCA to the current output device
PRCHR           jmp     LA282

* PRSPACE - one blank
PRSPACE         lda     #$20
                bra     PRCHR

* PRNCH - send ACCB characters from (X)
PRNCH           pshs    a,b,x
PRNCH1          lda     ,x+
                jsr     LA282
                decb
                bne     PRNCH1
                puls    a,b,x,pc

* PRDEC - print ACCB as an unsigned decimal number
PRDEC           pshs    a,b,x
                clra
                jsr     LBDCC           convert ACCD to decimal and print
                puls    a,b,x,pc

* PRCR - carriage return
PRCR            jmp     LB95C

*******************************************************************
* DRVARG - read an optional drive number argument.
* Leaves it in DCDRV.  Absent means the default drive, which DRIVE
* sets and which starts as 0.
*******************************************************************
DRVARG          lda     DEFDRV
                sta     DCDRV
                jsr     GETCCH
                beq     DRVARG9         nothing there - the default
                jsr     EVALEXPB        evaluate; result in ACCB
                bsr     DRVCHK
                stb     DCDRV
DRVARG9         rts

*******************************************************************
* DRVCHK - is ACCB a drive that exists?  Raises ?DN if not, which
* is what a Disk BASIC program expects when it asks for a drive
* that is not there.
*
* Drive 0 is always the bubble.  A floppy only exists if the cold
* start scan actually found a controller, so asking for one on a
* machine without it gives ?DN rather than a hang.
*******************************************************************
DRVCHK          tstb
                beq     DRVCHK9         0 - the bubble, always here
                if      MOCK
                bra     DRVBAD          MOCK geometry is not a floppy's
                endc
                cmpb    #FDCDRVS
                bhi     DRVBAD
                tst     MPIFDV
                beq     DRVBAD
DRVCHK9         rts
DRVBAD          ldb     #ERRDN
                jmp     LAC46           ?DN

*******************************************************************
* OPNCHK - ?AO if the file whose first granule is in V976 is open
* on any FCB.  Deleting or overwriting a file while an FCB is still
* feeding it would let its granules be handed to someone else.
*******************************************************************
OPNCHK          pshs    a,b,x
                ldb     FCBACT
                incb                    the system FCB counts too
OPNCHK1         pshs    b
                lbsr    GETFCBB         X = that FCB, flags from its type
                puls    b
                beq     OPNCHK2         closed - no conflict
                lda     FCBFGR,x
                cmpa    V976
                beq     OPNCHK9
OPNCHK2         decb
                bne     OPNCHK1
                puls    a,b,x,pc
OPNCHK9         jmp     LA61C           ?AO - file already open

*******************************************************************
* GRANOK - is every sector of granule ACCB backed by real bubble?
* Returns carry set when the granule is usable.
*******************************************************************
* The dead zone belongs to the bubble alone.  A floppy really does
* have every sector the geometry describes, so on one of those all
* granules exist and the answer is always yes.
GRANOK          pshs    a,b
                tst     DCDRV
                bne     GRANOKY         a floppy: no dead zone
                lda     #SECGRAN        the last sector in the granule
                jsr     GRAN2TS
                lda     DCTRK
                ldb     #SECTRK
                mul
                pshs    d
                ldb     DSEC
                decb
                clra
                addd    ,s++
                cmpd    #PHYSSEC        carry set if inside the bubble
                puls    a,b,pc
GRANOKY         orcc    #$01            carry set: the granule exists
                puls    a,b,pc

*******************************************************************
* GRANCNT - count the granules in the chain starting at ACCA.
* Returns the count in ACCB.  Used by DIR.
*******************************************************************
GRANCNT         pshs    a,x,u
                lbsr    FATPTR2
                clrb
GRANC1          incb
                cmpb    #GRANMX         more granules than exist can only
                bhi     GRANC9          mean the chain loops - stop
                cmpa    #GRANMX
                bhs     GRANC9          a damaged chain - stop counting
                pshs    b
                lda     a,u
                tfr     a,b
                cmpb    #$C0
                puls    b
                bhs     GRANC9
                bra     GRANC1
GRANC9          puls    a,x,u,pc

*******************************************************************
* DSKINI - prepare the bubble for use.
*
* There is nothing to format on a bubble, so this writes a fresh
* allocation table and empties the directory.  Granules that reach
* past the end of the physical bubble are marked $FE, which is
* neither free nor a legal link, so they can never be allocated.
*******************************************************************
* The drive number is not optional, as it is for DIR and UNLOAD.
* Disk BASIC requires it here, and this command destroys whatever was
* on the drive it lands on.
DSKINI          jsr     GETCCH
                lbeq    LA61F           ?DN - name the drive
                jsr     DRVARG
                jsr     LA5C7           error if anything else on the line
                lbsr    CLOSEALL        as Disk BASIC does: files still
*                                       open refer to the old contents
                clr     FATBL0          and nothing may span the format
                lbsr    FATPTR2         U = granule data
                clra
DSKINI1         pshs    a
                tfr     a,b
                bsr     GRANOK
                puls    a
                ldb     #$FF            free
                bcs     DSKINI2
                ldb     #DEADGR         past the end of the bubble
DSKINI2         stb     a,u
                inca
                cmpa    #GRANMX
                blo     DSKINI1
                lbsr    FATWR

* an empty directory is a run of never-used entries
                ldx     #DBUF0
                lda     #$FF
                clrb
DSKINI3         sta     ,x+
                decb
                bne     DSKINI3
                ldb     #DIRSEC1
DSKINI4         pshs    b
                lda     #DIRTRK
                ldx     #DBUF0
                jsr     DWRITE
                puls    b
                incb
                cmpb    #DIRSECN
                bls     DSKINI4
                rts

*******************************************************************
* FREE(n) - the number of free granules.
*******************************************************************
FREE            jsr     LB70E           evaluate the drive number
                lbsr    DRVCHK
                stb     DCDRV
                lbsr    FATVAL
                lbsr    FATPTR2
                clra                    granule number
                clrb                    free count
FREE1           pshs    b
                ldb     a,u
                cmpb    #$FF
                puls    b
                bne     FREE2
                incb
FREE2           inca
                cmpa    #GRANMX
                blo     FREE1
                clra
                jmp     GIVABF          return it as a number

*******************************************************************
* DIR - list the directory.
*
* One line per live file: name, extension, type, format and the
* number of granules it occupies, then a count of free granules.
*******************************************************************
DIR             jsr     DRVARG
                lbsr    FATVAL
                jsr     PRCR
                ldb     #DIRSEC1
DIR1            pshs    b
                lda     #DIRTRK
                ldx     #DBUF0
                jsr     DREAD
                ldu     #DBUF0
DIR2            lda     ,u
                beq     DIR5            killed - skip it
                cmpa    #$FF
                beq     DIR9            never used - the directory ends here
                bsr     DIRLINE
DIR5            leau    DIRLEN,u
                cmpu    #DBUF0+SECLEN
                bne     DIR2
                puls    b
                incb
                cmpb    #DIRSECN
                bls     DIR1
                bra     DIRFR
DIR9            puls    b
DIRFR           jsr     PRCR
                lbsr    FATPTR2
                clra
                clrb
DIRFR1          pshs    b
                ldb     a,u
                cmpb    #$FF
                puls    b
                bne     DIRFR2
                incb
DIRFR2          inca
                cmpa    #GRANMX
                blo     DIRFR1
                jsr     PRDEC
                ldx     #MSGFREE-1
                jsr     STRINOUT
                jmp     PRCR

*******************************************************************
* DIRLINE - print one directory entry.  U points at it.
*******************************************************************
DIRLINE         pshs    a,b,x,u
                leax    ,u
                ldb     #8              the name
                jsr     PRNCH
                jsr     PRSPACE
                leax    DIREXT,u
                ldb     #3              the extension
                jsr     PRNCH
                jsr     PRSPACE
                lda     DIRTYP,u        file type digit
                adda    #'0
                jsr     PRCHR
                jsr     PRSPACE
                lda     DIRASC,u        format
                beq     DIRLB
                lda     #'A             ASCII
                bra     DIRLB2
DIRLB           lda     #'B             binary or crunched
DIRLB2          jsr     PRCHR
                jsr     PRSPACE
                lda     DIRGRN,u        how much space it uses
                lbsr    GRANCNT
                jsr     PRDEC
                jsr     PRCR
                puls    a,b,x,u,pc

MSGFREE         fcc     ' GRANULES FREE'
                fcb     $00

*******************************************************************
* KILL - delete a file.
*
* The directory entry is marked dead first and written back before
* the granules are released, so an interruption leaves orphaned
* granules (recoverable) rather than a live directory entry
* pointing at granules that have been handed to something else.
*******************************************************************
KILL            lbsr    NAMEDEF         parse the file name
                jsr     LA5C7           error if anything else on the line
                lbsr    FATVAL
                lbsr    DIRSRCH
                lbsr    DIRFOUND        ?NE if it is not there
                lbsr    OPNCHK          ?AO if it is open right now
                ldx     V974            the directory entry
                clr     DIRNAM,x        mark it killed
                lda     DIRGRN,x        remember where its data was
                pshs    a
                ldb     V973
                lbsr    DIRWRB          write the directory back
                puls    a
                lbsr    GRANFREE        release the chain
                lbsr    FATWR
                rts

*******************************************************************
* RENAME old TO new
*
* The line is parsed twice: once to validate both names and confirm
* the new one is free, and again to do the work, because parsing
* the second name overwrites the buffer the first one used.
*******************************************************************
RENAME          ldx     CHARAD          where the arguments begin
                pshs    x
                bsr     RENNAME         the existing name
                bsr     RENTO           'TO' and the new name
                bsr     RENFREE         the new name must not exist
                puls    x
                stx     CHARAD          rewind and do it for real
                bsr     RENNAME
                lbsr    DIRSRCH
                lbsr    DIRFOUND        ?NE if the old name is not there
                bsr     RENTO
                ldx     #DNAMBF
                ldu     V974            the directory entry to rename
                ldb     #11             name and extension
                jsr     LA59A
                ldb     V973
                lbsr    DIRWRB
                rts

RENTO           ldb     #$A5            the 'TO' token
                jsr     SYNCHK
RENNAME         lbsr    NAMEDEF
                rts

RENFREE         lbsr    DIRSRCH
                tst     V973
                beq     RENFRE9
                ldb     #ERRAE
                jmp     LAC46           ?AE - file already exists
RENFRE9         rts

*******************************************************************
* DRIVE - select the default drive.
*******************************************************************
DRIVE           jsr     EVALEXPB
                lbsr    DRVCHK
                stb     DEFDRV
                stb     DCDRV
                rts

*******************************************************************
* VERIFY ON / VERIFY OFF
*******************************************************************
VERIFY          clrb                    OFF
                cmpa    #$AA            the OFF token
                beq     VERIFY1
                comb                    ON
                cmpa    #$88            the ON token
                lbne    LB277           ?SN
VERIFY1         stb     DVERFL
                jmp     GETNCH

*******************************************************************
* UNLOAD - close everything and forget the cached allocation table.
*******************************************************************
UNLOAD          jsr     DRVARG
                lbsr    DKCLOS
                rts

*******************************************************************
* SAVE "name" [,A]
*
* A crunched program is written as a three byte preamble - $FF then
* the two byte length - followed by the program image, which is the
* format Disk BASIC uses.  With ,A the program is listed into the
* file instead, and comes back as plain text.
*******************************************************************
SAVE            cmpa    #'M
                lbeq    SAVEM
                lbsr    SAVNAME
                ldx     ZERO            two zero bytes
                stx     DFLTYP          type 0 (BASIC), crunched
                jsr     GETCCH
                beq     SAVEBIN
                jsr     SYNCOMMA
                ldb     #'A
                jsr     SYNCHK
                jsr     LA5C7           nothing else may follow
                com     DASCFL          mark it as ASCII
                lbsr    SAVEOPEN
                clra                    Z set - list the whole program
                jmp     LIST            LIST writes through console out

SAVEBIN         jsr     LA5C7
                bsr     SAVEOPEN
                lda     #$FF            the crunched-program marker
                lbsr    PUTFB
                ldd     VARTAB
                subd    TXTTAB
                lbsr    PUTFB           length, high byte first
                tfr     b,a
                lbsr    PUTFB
                ldx     TXTTAB
SAVEB1          lda     ,x+
                bsr     PUTFB
                cmpx    VARTAB
                bne     SAVEB1
                jmp     LA42D           close the file

*******************************************************************
* SAVEM "name",start,end,exec
*
* A machine language file is one or more blocks, each introduced by
* a five byte preamble ($00, length, load address), and closed by a
* postamble ($FF, 0000, execution address).  This writes a single
* block, which is what SAVEM has always produced.
*******************************************************************
SAVEM           jsr     GETNCH          step past the M
                bsr     MLNAME
                jsr     L836C           start address, onto the stack
                jsr     L836C           end address
                cmpx    2,s
                lbcs    LB44A           ?FC if it ends before it starts
                jsr     L836C           execution address
                jsr     LA5C7
                ldd     #$0200          machine language, not ASCII
                std     DFLTYP
                lbsr    SAVEOPEN
                clra                    a data block follows
                bsr     MLPUT1
                ldd     2,s             end
                subd    4,s             minus start
                addd    #1              the last byte counts too
                tfr     d,y
                bsr     MLPUT2          the block length
                ldd     4,s
                bsr     MLPUT2          the load address
                ldx     4,s
SAVEM1          lda     ,x+
                lbsr    PUTFB
                leay    -1,y
                bne     SAVEM1
                lda     #$FF            the postamble
                bsr     MLPUT1
                clra
                clrb
                bsr     MLPUT2
                puls    a,b,x,y         exec address into ACCD
                bsr     MLPUT2
                jmp     LA42D           close the file

* MLPUT2 writes ACCD, MLPUT1 writes ACCA and swaps the halves
MLPUT2          bsr     MLPUT1
MLPUT1          lbsr    PUTFB
                exg     a,b
                rts

* MLNAME - a file name defaulting to the BIN extension
MLNAME          ldx     #BINEXT
                lbra    NAMEGET

* SAVNAME - a file name with BAS as the default extension
SAVNAME         ldx     #BASEXT
                lbra    NAMEGET

* SAVEOPEN / LOADOPEN - the system FCB, output or input
SAVEOPEN        lda     #'O
                lbra    OPENSYS
LOADOPEN        lda     #'I
                lbra    OPENSYS

* SETSYSDEV - point DEVNUM back at the system FCB
SETSYSDEV       pshs    b
                ldb     FCBACT
                incb
                stb     DEVNUM
                puls    b,pc

* PUTFB - write ACCA to the file that DEVNUM selects
PUTFB           jmp     LA282           console out, redirected by DVEC3

* GETFB - read the next byte of the file into ACCA.
* LA176, not LA171: LA171 masks off bit 7, which would corrupt
* every byte of a crunched program (starting with its $FF marker).
GETFB           jmp     LA176           console in, redirected by DVEC4

*******************************************************************
* LOAD "name" [,R]
*******************************************************************
LOAD            cmpa    #'M
                lbeq    LOADM
                clra                    do not run afterwards
                clrb                    not a merge
                bra     LOADGO

*******************************************************************
* MERGE "name" - read an ASCII listing into the program already in
* memory, rather than replacing it.
*******************************************************************
MERGE           clra                    do not run afterwards
                ldb     #$FF            merging

LOADGO          sta     DRUNFL
                stb     DMRGFL
                bsr     SAVNAME
                jsr     GETCCH
                beq     LOAD1
                jsr     SYNCOMMA
                ldb     #'R
                jsr     SYNCHK
                lda     #3              Disk BASIC's flag: bit 1 = run
                sta     DRUNFL          it, bit 0 = close all files first
LOAD1           jsr     LA5C7
                bsr     LOADOPEN        this also reads the file's type
                tst     DASCFL
                lbne    LOADASC
* --- a crunched program ---
                lda     DFLTYP          it has to be a BASIC program,
                ora     DMRGFL          and MERGE only works on ASCII
                lbne    LA616           ?FM - bad file mode
                jsr     LAD19           NEW
* LAD19 resets the stack pointer, so from here on nothing may RTS:
* every exit below is a JMP, which is how Disk BASIC does it too.
                com     DLODFL          an error from here on leaves half
*                                       a program - the error driver sees
*                                       this flag and does a NEW
                bsr     SETSYSDEV       and it forgets the device number
                bsr     GETFB           the $FF marker
                cmpa    #$FF
                lbne    LOADBAD
                bsr     GETFB
                tfr     a,b
                pshs    b               length, high byte first
                bsr     GETFB
                tfr     a,b
                puls    a               ACCD = the program length
                addd    TXTTAB
                jsr     LAC37           ?OM unless it fits below the stack
* Read to the end of the file rather than trusting the length: the
* length has already served its purpose above, and stopping on the
* real end of file is what keeps a truncated file from running on
* into whatever follows it in memory.
                ldx     TXTTAB
LOADB1          bsr     GETFB
                ldb     CINBFL
                bne     LOADB2
                sta     ,x+
                bra     LOADB1
LOADB2          stx     VARTAB
                clr     DLODFL          the program in memory is whole

*******************************************************************
* LOADFIN - finish a load, however the program got here.
*
* Also the end of an ASCII load, which arrives through the line
* input termination hook when the file runs out.
*******************************************************************
LOADFIN         jsr     LA42D           close the file
                jsr     LAD21           part of NEW - erase the variables
                jsr     LACEF           relocate the line pointers
* The run flag must be consumed here: leaving it set would make the
* next thing that comes through this path - a later SAVE ",A", for
* instance - spuriously run the program.
                lda     DRUNFL
                clr     DRUNFL
                lsra                    bit 0 - close all files first
                bcc     LOADFIN2        (LOAD ,R does, RUN "file" does not)
                pshs    a
                jsr     LA426           close all files
                puls    a
LOADFIN2        lsra                    bit 1 - run the program
                lbcs    LAD9E
                jmp     LAC73           back to the direct mode prompt

LOADBAD         jsr     LA42D
                ldb     #ERRFD
                jmp     LAC46           ?FD - bad file data

*******************************************************************
* LOADASC - an ASCII listing.
*
* Rather than parsing the text ourselves, hand it to BASIC: with
* the file selected as the input device, BASIC's direct mode loop
* reads, crunches and stores each line exactly as if it had been
* typed.  When the file runs dry Color BASIC calls the line input
* termination hook, and DVEC13 finishes the load.
*******************************************************************
LOADASC         tst     DMRGFL
                bne     LOADASC1
                jsr     LAD19           NEW - this also resets the stack
LOADASC1        lbsr    SETSYSDEV       NEW forgets the device number
                jmp     LAC7C           BASIC's direct loop does the rest
*******************************************************************
* LOADM "name" [,offset]
*
* Reads block after block until the postamble, which also supplies
* the address EXEC will use.  Each byte is read back after being
* stored, so loading into ROM or absent RAM is reported rather than
* silently doing nothing.
*******************************************************************
LOADM           jsr     GETNCH          step past the M
                lbsr    MLNAME
                lbsr    LOADOPEN
                ldd     DFLTYP
                subd    #$0200          machine language, not ASCII?
                lbne    LA616           ?FM
                ldx     ZERO            no offset by default
                jsr     GETCCH
                beq     LOADM1
                jsr     SYNCOMMA
                jsr     LB73D           evaluate the offset
LOADM1          stx     LMOFF
                jsr     LA5C7
* The flag is read directly rather than through MLGET1, which swaps
* the halves of ACCD on its way out and would leave it in ACCB.
LOADM2          lbsr    GETFCE          the block flag
                pshs    a
                bsr     MLGET2
                tfr     d,y             the block length
                bsr     MLGET2
                addd    LMOFF
                std     EXECJP          where EXEC will go
                tfr     d,x
                lda     ,s+
                lbne    LA42D           the postamble - close and finish
LOADM3          lbsr    GETFB
                ldb     CINBFL
                beq     LOADM4
                ldb     #ERRIE
                jmp     LAC46           ?IE - the file ended mid block
LOADM4          sta     ,x
                cmpa    ,x+             did it actually store?
                beq     LOADM5
                ldb     #ERRIO
                jmp     LAC46           ?IO - not writable memory
LOADM5          leay    -1,y
                bne     LOADM3
                bra     LOADM2

* MLGET2 reads two bytes into ACCD, high byte first
MLGET2          bsr     MLGET1
MLGET1          lbsr    GETFCE          reading past the end here is ?IE
                exg     a,b
                rts

*******************************************************************
* WRITE #n, item, item ...
*
* PRINT# with punctuation: strings go out in quotes, items are
* separated by commas and the line ends with a carriage return, so
* that INPUT# reads back exactly what was written.
*******************************************************************
WRITE           lbeq    LB958           nothing to write - just a CR
                bsr     WRITELS
                clr     DEVNUM
                rts

WRITELS         cmpa    #'#
                bne     WRITE2          no device given - the current one
                jsr     LA5A5           set and check the device number
                jsr     LA406           it has to be open for output
                jsr     GETCCH
                lbeq    LB958
WRITE1          jsr     SYNCOMMA
WRITE2          jsr     LB156           evaluate the item
                lda     VALTYP
                bne     WRITESTR
                jsr     LBDD9           a number goes out as its digits
                jsr     LB516
                jsr     LB99F
WRITESEP        jsr     GETCCH
                lbeq    LB958           the list ended - close with a CR
                lda     #',
                jsr     LA35F           set the print parameters
                tst     PRTDEV
                beq     WRITESEP2
                lda     #$0D            the cassette separates with a CR
WRITESEP2       bsr     WRITECH
                bra     WRITE1

WRITESTR        bsr     WRITEQ          a string goes out in quotes, which
                jsr     LB99F           is what lets INPUT# read back one
                bsr     WRITEQ          containing commas or spaces
                bra     WRITESEP

WRITEQ          jsr     LA35F
                lda     #'"
WRITECH         jmp     LA282

*******************************************************************
* CVN(s$)  - the five bytes a record holds, as a number.
* MKN$(n)  - a number, as the five bytes a record holds.
*
* A record buffer holds bytes, so a number goes into one in the same
* packed form BASIC keeps in a variable: an exponent and four bytes of
* mantissa.  These two are the way to put a number in a record and get
* it back without going through its decimal spelling, which would both
* cost more room and lose the low digits.
*
* More than five bytes is accepted and the first five used, as Disk
* BASIC does; fewer is ?FC, because there is no number there.
*******************************************************************
CVN             jsr     LB654           the string: X = bytes, ACCB = length
                cmpb    #5
                lbcs    LB44A           ?FC - too short to be a number
                clr     VALTYP          what comes back is a number
                jmp     LBC14           unpack it into FPA0

MKNS            jsr     LB143           ?TM if it is a string already
                ldb     #5
                jsr     LB50F           five bytes of string space
                jsr     LBC35           pack FPA0 into them
                jmp     LB69B           and hand back the descriptor

*******************************************************************
*******************************************************************
* FILES - how many files may be open at once, and how much room the
* record buffers get.
*
*   FILES n            n file control blocks, 0 to 15
*   FILES n,m          and m bytes of record buffer space
*   FILES ,m           just the buffer space
*
* Both default to what is already in force, so either may be left out.
*
* The FCBs and the record buffers sit below BASIC's program, so
* changing their size moves everything above them: the graphics pages,
* the program text, and the variables.  The program is carried to its
* new home rather than lost, which is why this is worth the arithmetic
* - a FILES in the middle of a program listing must not erase it.
*
* This ROM's own private block is below DFLBUF, not above it, so none
* of this disturbs it.  That is what it is down there for.
*
* The frame while the sums are done:
*   0,s       the number of FCBs, then the new graphics RAM page
*   1,s 2,s   the record buffer space, then the start of the FCBs
*******************************************************************
FILES           jsr     L95AC           put the display back to a known page
                ldd     FCBADR
                subd    #DFLBUF
                pshs    b,a             the buffer space in force now
                ldb     FCBACT
                pshs    b               and the FCB count in force now
                jsr     GETCCH
                cmpa    #',
                beq     FILES1          no count given, only a size
                jsr     EVALEXPB
                cmpb    #15
                lbhi    LB44A           ?FC - fifteen files is the limit
                stb     ,s
                jsr     GETCCH
                beq     FILES2          no size given
FILES1          jsr     SYNCOMMA
                jsr     LB3E6           the record buffer space
                ldd     FPA0+2
                std     1,s
FILES2          lbsr    CLOSEALL        every file closes: they are about
*                                       to be somewhere else
* With nothing open, the record buffer area is empty by definition.
* Saying so here means a buffer that was somehow never given back
* cannot outlive the layout it belonged to.
                ldx     #DFLBUF
                stx     RNBFAD
* --- where do the FCBs end? --------------------------------------
                ldb     ,s
                pshs    b               a counter to walk them with
                ldd     #DFLBUF
                addd    2,s
                lbcs    FILESOM
                std     2,s             the FCBs start here
* One more FCB than asked for: the system one, which COPY and MERGE
* use and which a program cannot reach.
FILES3          addd    #FCBLEN
                lbcs    FILESOM
                dec     ,s
                bpl     FILES3
* --- round up to a page the graphics can start on ----------------
* The counter has done its work and its slot now takes the page, which
* is what the three values are pulled off as further down.
                tstb                    already on a 256 byte boundary?
                beq     FILES4
                inca
                lbeq    FILESOM
FILES4          bita    #$01            and on an even one?
                beq     FILES5
                inca
                lbeq    FILESOM
FILES5          sta     ,s              the new graphics RAM page
* --- will BASIC still fit? ---------------------------------------
* The program and everything above it shift by the difference between
* the old graphics page and the new one.  Only the high byte matters:
* all of this is counted in whole pages.
                ldd     VARTAB
                suba    GRPRAM
                adda    ,s
                lbcs    FILESOM
                tfr     d,x             the new start of variables
                inca                    a page of headroom, because the
                lbeq    FILESOM         arithmetic only tracks pages
                cmpd    FRETOP
                lbhs    FILESOM         it would reach into string space
                deca
                subd    VARTAB
                addd    TXTTAB
                tfr     d,y             the new start of the program
* --- move the graphics pages with it -----------------------------
                lda     ,s
                suba    GRPRAM
                tfr     a,b
                adda    BEGGRP
                sta     BEGGRP
                addb    ENDGRP
                stb     ENDGRP
                puls    a,b,u           page, FCB count, start of the FCBs
                sta     GRPRAM
                stb     FCBACT
                stu     FCBADR
* --- carry BASIC's program to its new home -----------------------
* Running inside a program means the interpreter's own input pointer
* is somewhere in the text being moved, so it has to move too.
                lda     CURLIN
                inca
                beq     FILES6          direct mode: nothing is executing
                tfr     y,d
                subd    TXTTAB
                addd    CHARAD
                std     CHARAD
FILES6          ldu     VARTAB
                stx     VARTAB
                cmpu    VARTAB
                bhi     FILES8          the program is coming down
* going up: copy backwards, so the two runs cannot overlap wrongly
FILES7          lda     ,-u
                sta     ,-x
                cmpu    TXTTAB
                bne     FILES7
                sty     TXTTAB
                clr     -1,y            the byte before the program is zero
                bra     FILES9
* coming down: copy forwards, for the same reason
FILES8          ldu     TXTTAB
                sty     TXTTAB
                clr     -1,y
FILES8A         lda     ,u+
                sta     ,y+
                cmpy    VARTAB
                bne     FILES8A
* --- and lay the FCBs out again ----------------------------------
FILES9          ldu     #FCBV1
                ldx     FCBADR
                clrb
FILES10         stx     ,u++
                clr     FCBTYP,x        every one of them is closed
                leax    FCBLEN,x
                incb
                cmpb    FCBACT
                bls     FILES10         the system FCB is counted too
                jmp     L96CB           put the line numbers right

FILESOM         jmp     LAC44           ?OM - out of memory

*******************************************************************
* LOC(n) - how far into the file the reader is.
* LOF(n) - how long the file is.
*
* For a sequential file both count sectors, which is what Disk BASIC
* reports.  For a direct file LOC is the record the file is at - the
* one after the last GET or PUT - and LOF is the number of records in
* the file.
*******************************************************************
LOC             bsr     LOCFCB
                ldd     FCBREC,x
LOCRET          jmp     GIVABF          return ACCD as a number

LOF             bsr     LOCFCB
                lda     FCBDRV,x
                sta     DCDRV
                lda     FCBFGR,x
                bsr     GRANSEC         D = sectors in the chain
                pshs    a
                lda     FCBTYP,x
                cmpa    #RANFIL
                puls    a
                bne     LOCRET          sequential: the count of sectors
* A direct file's length is its record count: the bytes in the full
* sectors plus those used in the last one, over the record length.
* The byte count can pass 65535, so this is done in floating point,
* as Disk BASIC does it (LCD83).
                pshs    x
                subd    #0
                beq     LOF1            no sectors at all
                subd    #1              the last sector is only partly used
LOF1            bsr     LOFD2FP
                ldb     FP0EXP
                beq     LOF2            zero stays zero
                addb    #8              times 256 bytes a sector
                stb     FP0EXP
LOF2            jsr     LBC5F           FPA1 = bytes in the full sectors
                ldx     ,s
                ldd     FCBLST,x
                bsr     LOFD2FP
                clr     RESSGN
                lda     FP1EXP
                ldb     FP0EXP
                jsr     LB9C5           FPA0 = FPA1 + FPA0, the total
                jsr     LBC5F           into FPA1
                puls    x
                ldd     FCBRLN,x
                bsr     LOFD2FP
                clr     RESSGN
                lda     FP1EXP
                ldb     FP0EXP
                jsr     LBB91           FPA0 = FPA1 / FPA0
                jmp     LBCEE           and whole records only

* LOFD2FP - ACCD, unsigned, into FPA0.  GIVABF takes a signed value,
* which the byte counts here can exceed.
LOFD2FP         std     FPA0+2
                jmp     L880E

* LOCFCB - the argument names a file; leave X pointing at its FCB.
*
* LA5AE turns the argument into the current device number, and that
* MUST be put back the way it was: LOC appears inside expressions,
* and without the restore everything after "PRINT LOC(1)" in the
* statement is quietly printed into the file instead of the screen.
LOCFCB          lda     DEVNUM
                pshs    a
                jsr     LB143           ?TM if it is a string
                jsr     LA5AE           the argument selects the file
                tst     DEVNUM
                lble    LB44A           ?FC - not a file, as Disk BASIC
                lbsr    GETFCB
                puls    a
                sta     DEVNUM          the statement's own device back
                tst     FCBTYP,x
                lbeq    LA3FB           ?NO - not open
                rts

*******************************************************************
* GRANSEC - how many sectors the granule chain starting at ACCA
* occupies.  Returns the count in ACCD.
*******************************************************************
GRANSEC         pshs    x,u
                lbsr    FATPTR2
                ldx     #0
GRANS1          cmpa    #GRANMX
                bhs     GRANS9          a damaged chain - stop here
                lda     a,u             the entry is also the next granule
                cmpa    #$C0
                bhs     GRANS3          unless it is the terminator
                pshs    a
                clra                    a full granule
                ldb     #SECGRAN
                leax    d,x
                puls    a
                cmpx    #(TRKMAX-1)*SECTRK  more sectors than the whole
                bhi     GRANS9          bubble holds - the chain loops
                bra     GRANS1
GRANS3          anda    #$3F            sectors used in the last granule
                tfr     a,b
                clra
                leax    d,x
GRANS9          tfr     x,d
                puls    x,u,pc

*******************************************************************
* COPY "source" TO "destination"
*
* One sector at a time through DBUF0, so no free memory is needed
* and the size of the file does not matter.  Disk BASIC buffers as
* many whole sectors as fit between the arrays and the stack and
* reports ?OM for anything larger; this has no such limit.
*
* The destination's directory entry is written last.  An error part
* way through - a full disk, a bad sector - therefore leaves the
* granules it had taken marked only in the RAM image of the
* allocation table, which the next FATVAL discards, and no directory
* entry pointing at them.
*
* Every FAT, directory and media access sets DCDRV first.  The two
* files can be on different drives, and the cursors alternate
* between them for every sector.
*******************************************************************
COPY            lbsr    NAMEDEF         the source name
                lda     DCDRV           and the drive it named
                sta     COPYDR1
                ldx     #DNAMBF
                ldu     #COPYNM
                ldb     #11
                jsr     LA59A           put it somewhere safe
                ldb     #$A5            the 'TO' token
                jsr     SYNCHK
                lbsr    NAMEDEF         the destination name
                lda     DCDRV
                sta     COPYDR2
                jsr     LA5C7
                ldx     #DNAMBF
                ldu     #COPYNM2
                ldb     #11
                jsr     LA59A

* A file may not be copied onto itself.  The destination is cleared
* before any data is read, so the two being one file would destroy
* it rather than copy it.
                lda     COPYDR1
                cmpa    COPYDR2
                bne     COPYNS1
                ldx     #COPYNM
                ldu     #COPYNM2
                ldb     #11
COPYNS2         lda     ,x+
                cmpa    ,u+
                bne     COPYNS1
                decb
                bne     COPYNS2
                ldb     #ERRAE
                jmp     LAC46           ?AE - source and destination are one

* --- find the source --------------------------------------------
COPYNS1         ldx     #COPYNM
                ldu     #DNAMBF
                ldb     #11
                jsr     LA59A
                lda     COPYDR1
                sta     DCDRV
                lbsr    FATVAL
                lbsr    DIRSRCH
                lbsr    DIRFOUND        ?NE if it is not there
                ldu     V974            its directory entry
                lda     DIRGRN,u
                sta     COPYSGR
                lda     DIRTYP,u        the copy keeps the original's
                sta     COPYTYP         type, format and tail length
                lda     DIRASC,u
                sta     COPYASC
                ldd     DIRLST,u
                std     COPYLST

* --- clear the way at the destination ---------------------------
                ldx     #COPYNM2
                ldu     #DNAMBF
                ldb     #11
                jsr     LA59A
                lda     COPYDR2
                sta     DCDRV
                lbsr    FATVAL
                lbsr    DIRSRCH
                tst     V973
                beq     COPYDC          nothing of that name to remove
                lbsr    OPNCHK          ?AO if it is open right now
                ldx     V974
                clr     DIRNAM,x        mark it killed, as KILL does, and
                lda     DIRGRN,x        write that before the granules go
                pshs    a               back to the pool
                ldb     V973
                lbsr    DIRWRB
                puls    a
                lbsr    GRANFREE
                lbsr    FATWR
                lbsr    DIRSRCH         find the entry just freed
COPYDC          tst     V977            is there an entry to write at all?
                lbeq    COPYDFUL

* --- the destination's first granule ---------------------------
                lda     COPYDR2
                sta     DCDRV
                lbsr    GRANALLOC       ?DF if the disk is full
                sta     COPYDGR
                sta     COPYDCUR
                lbsr    FATPTR2         (preserves ACCA)
                ldb     #$C0
                stb     a,u             out of the free pool at once
                lda     #1
                sta     COPYDSEC

* --- the data --------------------------------------------------
                lda     COPYSGR
                sta     COPYSCUR
                lbsr    COPYSGET
COPYC1          lda     #1
                sta     COPYSSEC
COPYC2          lda     COPYSSEC
                cmpa    COPYSNS
                bhi     COPYC4          this granule is used up
                lda     COPYDSEC        room at the destination?
                cmpa    #SECGRAN
                bls     COPYC3
                lbsr    COPYDNEW        no - take another granule
COPYC3          lda     COPYDR1
                sta     DCDRV
                lda     COPYSSEC
                ldb     COPYSCUR
                lbsr    GRAN2TS         sets DCTRK and DSEC
                lda     DCTRK
                ldb     DSEC
                ldx     #DBUF0
                jsr     DREAD
                lda     COPYDR2
                sta     DCDRV
                lda     COPYDSEC
                ldb     COPYDCUR
                lbsr    GRAN2TS
                lda     DCTRK
                ldb     DSEC
                ldx     #DBUF0
                jsr     DWRITE
                lbsr    COPYDMRK        the terminator follows the writes
                inc     COPYDSEC
                inc     COPYSSEC
                bra     COPYC2
COPYC4          tst     COPYSLF
                bne     COPYC9          that was the last granule
                lda     COPYDR1
                sta     DCDRV
                lbsr    FATPTR2
                lda     COPYSCUR
                lda     a,u             follow the source's chain
                sta     COPYSCUR
                lbsr    COPYSGET
                bra     COPYC1

* --- the destination's directory entry -------------------------
COPYC9          ldx     #COPYNM2
                ldu     #DNAMBF
                ldb     #11
                jsr     LA59A
                lda     COPYDR2
                sta     DCDRV
                lbsr    DIRSRCH         the data copy reused DBUF0, so
                lda     V977            find the free entry again
                lbeq    COPYDFUL
                sta     V973
                ldb     V977
                lda     #DIRTRK
                ldx     #DBUF0
                jsr     DREAD
                ldu     V978
                stu     V974
                leax    ,u
                ldb     #DIRLEN
                clra
COPYE1          sta     ,x+
                decb
                bne     COPYE1
                ldx     #DNAMBF
                ldb     #11
                jsr     LA59A           name and extension
                ldu     V974
                lda     COPYTYP
                sta     DIRTYP,u
                lda     COPYASC
                sta     DIRASC,u
                lda     COPYDGR
                sta     DIRGRN,u
                ldd     COPYLST
                std     DIRLST,u
                ldb     V973
                lbsr    DIRWRB
                lbsr    FATWR           commit the destination's table
                rts

COPYDFUL        ldb     #ERRDF
                jmp     LAC46           ?DF - no free directory entry

*******************************************************************
* COPYSGET - read the source's allocation entry for the granule in
* COPYSCUR: how many of its sectors hold file data, and whether it
* is the last one.  A chain that leaves the table, runs into the
* dead zone, or claims more sectors than a granule holds is ?FS
* rather than something to follow.
*******************************************************************
COPYSGET        pshs    a,b,u
                lda     COPYDR1
                sta     DCDRV
                lbsr    FATPTR2
                lda     COPYSCUR
                cmpa    #GRANMX
                bhs     COPYSBAD
                lda     a,u
                cmpa    #DEADGR
                beq     COPYSBAD
                cmpa    #$C0
                blo     COPYSG1
                anda    #$3F            a terminator: the sector count
                beq     COPYSBAD
                cmpa    #SECGRAN
                bhi     COPYSBAD
                sta     COPYSNS
                lda     #$FF
                sta     COPYSLF
                puls    a,b,u,pc
COPYSG1         cmpa    #GRANMX         a link, and it must be a real one
                bhs     COPYSBAD
                lda     #SECGRAN        a full granule
                sta     COPYSNS
                clr     COPYSLF
                puls    a,b,u,pc
COPYSBAD        ldb     #ERRFS
                jmp     LAC46           ?FS - bad file structure

*******************************************************************
* COPYDNEW - the destination granule is full; take another and link
* the chain on.  As in FCBROOM the new granule leaves the free pool
* at once, so nothing else can be handed the same one.
*******************************************************************
COPYDNEW        pshs    a,b,u
                lda     COPYDR2
                sta     DCDRV
                lbsr    GRANALLOC       ?DF if the disk is full
                lbsr    FATPTR2
                ldb     COPYDCUR
                sta     b,u             link the old granule to the new
                sta     COPYDCUR
                ldb     #$C0
                stb     a,u
                lda     #1
                sta     COPYDSEC
                puls    a,b,u,pc

*******************************************************************
* COPYDMRK - keep the destination's terminator in step with what has
* actually been written, so an interrupted copy leaves a chain that
* describes the sectors it really wrote.
*******************************************************************
COPYDMRK        pshs    a,b,u
                lda     COPYDR2
                sta     DCDRV
                lbsr    FATPTR2
                lda     COPYDSEC
                ora     #$C0
                ldb     COPYDCUR
                sta     b,u
                puls    a,b,u,pc
*******************************************************************
* DSKI$ drive,track,sector,A$,B$
*
* Hands a whole sector to BASIC as two 128 character strings,
* because a BASIC string cannot hold 256.  This and DSKO$ are the
* only places the virtual geometry is visible to a program, and
* they are what every sector editor of the period was built on.
*******************************************************************
DSKI            bsr     DSKTS           drive, track and sector
                bsr     DSKSVAR          the first string variable
                pshs    x
                bsr     DSKSVAR          the second
                pshs    x
                ldb     #2              read
                lbsr    DSKSEC
                ldu     #DBUF0+128      the top half
                puls    x
                bsr     DSKPUT
                ldu     #DBUF0          the bottom half
                puls    x

* DSKPUT - copy 128 bytes at U into a new string for the descriptor X
DSKPUT          pshs    u,x
                ldb     #128
                jsr     LB50F           reserve string space
                leau    ,x
                puls    x
                stb     ,x              descriptor length
                stu     2,x             and address
                puls    x
DSKMOVE         jmp     LA59A           move ACCB bytes from X to U

* DSKSVAR - a comma, then a string variable; ?TM if it is numeric
DSKSVAR          jsr     SYNCOMMA
                ldx     #LB357          evaluate a variable
                bsr     DSKEVAL
DSKTM           jmp     LB146           ?TM if numeric

*******************************************************************
* DSKTS - evaluate the drive, track and sector arguments.
*******************************************************************
DSKTS           jsr     EVALEXPB
                cmpb    #3
                bhi     DSKFC
                pshs    b
                jsr     LB738           comma, then the track
                cmpb    #TRKMAX-1
                bhi     DSKFC1
                pshs    b
                jsr     LB738           comma, then the sector
                stb     DSEC
                decb
                cmpb    #SECTRK-1
                bhi     DSKFC2
                puls    a,b
                sta     DCTRK
                stb     DCDRV
                rts
DSKFC2          leas    1,s
DSKFC1          leas    1,s
DSKFC           jmp     LB44A           ?FC - out of range

*******************************************************************
* DSKEXPR / DSKEVAL - evaluate an argument without losing the
* drive, track and sector already parsed into the DSKCON block.
*******************************************************************
DSKEXPR         jsr     SYNCOMMA
                ldx     #LB156          evaluate an expression
DSKEVAL         ldb     DCDRV
                ldu     DCTRK           DCTRK and DSEC together
                pshs    u,b
                jsr     ,x
                puls    b,u
                stb     DCDRV
                stu     DCTRK
                rts

*******************************************************************
* DSKO$ drive,track,sector,A$,B$
*******************************************************************
DSKO            bsr     DSKTS
                bsr     DSKEXPR         the first string
                bsr     DSKTM
                ldx     FPA0+2
                pshs    x
                bsr     DSKEXPR         the second
                jsr     LB654           its length and address
                pshs    x,b
                clrb                    a whole sector of zeros first, so
                ldx     #DBUF0          short strings pad rather than
DSKO1           clr     ,x+             leaving whatever was there before
                decb
                bne     DSKO1
                puls    b,x
                ldu     #DBUF0+128
                bsr     DSKCLIP
                puls    x
                jsr     LB659           length and address of the first
                ldu     #DBUF0
                bsr     DSKCLIP
                ldb     #3              write
                bra     DSKSEC

* DSKCLIP - move min(B,128) bytes from X to U.  Disk BASIC moves
* the whole string and a long one runs off the end of the buffer;
* only the first 128 characters mean anything, so stop there.
DSKCLIP         cmpb    #128
                lbls    DSKMOVE
                ldb     #128
                lbra    DSKMOVE

*******************************************************************
* DSKSEC - read or write DBUF0 with the op code in ACCB.
*******************************************************************
DSKSEC          ldx     #DBUF0
                stx     DCBPT
                stb     DCOPC
                jsr     DSKCON
                tst     DCSTA
                lbne    DSKERR
                rts
