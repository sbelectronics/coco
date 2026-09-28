*******************************************************************
* fs.asm - granule allocation table and directory
*
* The on-media format is Disk BASIC's, resized by config.inc:
*
*   FAT     one byte per granule on the directory track, sector 2.
*           $FF        free
*           $00-$3F    the number of the next granule in the chain
*           $C0+n      last granule, n sectors of it used
*           $FE        dead zone - past the end of the physical
*                      bubble, permanently unavailable (ours)
*
*   dir     32 byte entries in sectors 3..11 of the directory track.
*           byte 0 = $00 killed, $FF never used, anything else is
*           the first character of a live filename.
*
* A RAM image of the FAT is kept at FATBL0 so that allocation does
* not need media access.  It is written back when enough granules
* have been taken (WFATVL) and whenever a file is closed.
*******************************************************************

*******************************************************************
* FATPTR - point X at the FAT image for the current drive.
* Preserves everything else.
*******************************************************************
FATPTR          pshs    a,b
                lda     DCDRV
                ldb     #FATLEN
                mul
                ldx     #FATBL0
                leax    d,x
                puls    a,b,pc

*******************************************************************
* FATVAL - make sure the FAT image is valid.
*
* While any file is open the RAM image is the authority (it may
* hold allocations not yet written back), so it is only re-read
* from the media when nothing is using it.
*******************************************************************
FATVAL          pshs    a,b,x,u
                bsr     FATPTR
                tst     FAT0,x          any active files?
                bne     FATVAL9         yes - do not disturb the image
                lda     #DIRTRK
                ldb     #2              the FAT lives in sector 2
                ldx     #DBUF0
                jsr     DREAD
                ldx     #DBUF0
                bsr     FATPTR2
                ldb     #GRANMX
                jsr     LA59A           move B bytes from X to U
FATVAL9         puls    a,b,x,u,pc

* FATPTR2 - point U at the granule data of the FAT image
FATPTR2         pshs    x
                bsr     FATPTR
                leau    FATCON,x
                puls    x,pc

*******************************************************************
* FATWR - write the FAT image back to the media.
*******************************************************************
FATWR           pshs    a,b,x,u
                bsr     FATPTR
                clr     FAT1,x          the image and the media agree again
                leax    FATCON,x
                ldu     #DBUF0
                ldb     #GRANMX
                jsr     LA59A
* the rest of the sector is not part of the FAT; zero it
                clra
FATWR1          sta     ,u+
                cmpu    #DBUF0+SECLEN
                bne     FATWR1
                lda     #DIRTRK
                ldb     #2
                ldx     #DBUF0
                jsr     DWRITE
                puls    a,b,x,u,pc

*******************************************************************
* FATBUMP - note that a granule has been taken, and write the FAT
* back once enough of them have accumulated.
*******************************************************************
FATBUMP         pshs    a,b,x,u
                bsr     FATPTR
                inc     FAT1,x
                lda     FAT1,x
                cmpa    WFATVL
                blo     FATBUMP9
                bsr     FATWR
FATBUMP9        puls    a,b,x,u,pc

*******************************************************************
* GRANALLOC - allocate the first free granule.
*
* Dead zone granules are marked $FE rather than $FF, so a search
* for a free entry passes over them without needing a bounds test.
*
* Returns the granule number in ACCA; raises ?DF if the bubble is
* full.  The entry is left as the caller found it: the caller marks
* it (usually $C0+n) once it knows how much of it is in use.
*******************************************************************
GRANALLOC       pshs    b,x,u
                bsr     FATPTR2         U = granule data
                clra
GRANA1          ldb     a,u
                cmpb    #$FF
                beq     GRANA9
                inca
                cmpa    #GRANMX
                blo     GRANA1
                ldb     #ERRDF
                jmp     LAC46           ?DF - disk full
GRANA9          puls    b,x,u,pc

*******************************************************************
* GRANFREE - release the granule chain that starts at ACCA.
*
* Walks the chain marking each entry free.  A link that is not a
* legal granule number stops the walk and raises ?FS, so a damaged
* chain cannot run away through the whole table.
*******************************************************************
GRANFREE        pshs    a,b,x,u
                lbsr    FATPTR2         U = granule data
GRANF1          cmpa    #GRANMX
                bhs     GRANFBAD
                ldb     a,u             the link out of this granule
                cmpb    #DEADGR         a corrupt chain must never free
                beq     GRANFBAD        a dead zone granule
                pshs    b
                ldb     #$FF
                stb     a,u             this one is free again
                puls    b
                cmpb    #$C0
                bhs     GRANF9          a terminator - the chain ends
                tfr     b,a
                bra     GRANF1
GRANF9          puls    a,b,x,u,pc
GRANFBAD        ldb     #ERRFS
                jmp     LAC46           ?FS - bad file structure

*******************************************************************
* RECOFF - where in the file does a record start?
*
*   Entry: X = FCB, with FCBREC and FCBRLN set
*   Exit:  X = sector index, counted from the start of the file
*          ACCB = byte offset within that sector
*          A, Y and U preserved; raises ?BR if the record starts
*          past sector 65535
*
* The offset is (FCBREC-1) * FCBRLN, and both are 16 bit, so the
* product needs 32.  Splitting it into a sector and an offset is
* then free, because SECLEN is 256: the low byte of the product is
* the offset within the sector and everything above it is the sector
* number.  The top byte has to be zero, which is what bounds a file
* to 65536 sectors - far past what either device holds, but it is
* the arithmetic that would break first, so it is checked here
* rather than assumed.
*
* MUL is the only multiply the 6809 has, so the product is built
* from the four byte-sized partial products, added in from the least
* significant end with the carries rippled up by hand.  The 32 bit
* accumulator and both operands live in a frame on the stack:
*
*   0,s 1,s 2,s 3,s   the product, most significant byte first
*   4,s 5,s           FCBREC-1, high byte first
*   6,s 7,s           FCBRLN
*******************************************************************
RECOFF          pshs    a,y,u
                ldd     FCBRLN,x
                pshs    b,a
                ldd     FCBREC,x
                subd    #1              records are numbered from 1
                pshs    b,a
                ldd     #0
                pshs    b,a
                pshs    b,a
* low byte times low byte, into bytes 2 and 3
                lda     5,s
                ldb     7,s
                mul
                addd    2,s
                std     2,s
                bcc     RECOFF1
                ldd     ,s              ripple the carry up
                addd    #1
                std     ,s
* the two middle partial products, into bytes 1 and 2
RECOFF1         lda     4,s
                ldb     7,s
                mul
                addd    1,s
                std     1,s
                bcc     RECOFF2
                inc     ,s
RECOFF2         lda     5,s
                ldb     6,s
                mul
                addd    1,s
                std     1,s
                bcc     RECOFF3
                inc     ,s
* high byte times high byte, into bytes 0 and 1.  No carry can come
* out of this one: the largest 16 by 16 product still fits in 32.
RECOFF3         lda     4,s
                ldb     6,s
                mul
                addd    ,s
                std     ,s
                tst     ,s
                bne     RECOFFBR
                ldx     1,s             the sector holding the record
                ldb     3,s             and where in it the record starts
                leas    8,s
                puls    a,y,u,pc
RECOFFBR        leas    8,s
                puls    a,y,u
                ldb     #ERRBR
                jmp     LAC46           ?BR - bad record

*******************************************************************
* GRAN2TS - convert a granule and a sector within it into the
* track and sector numbers DSKCON wants.
*
*   Entry: ACCB = granule number, ACCA = sector in granule (1..SECGRAN)
*   Exit:  DCTRK and DSEC set
*
* Granules are laid out GRANTRK to a track, skipping the directory
* track entirely.
*******************************************************************
GRAN2TS         pshs    a,b
                clra                    A counts tracks
GRAN2T1         cmpb    #GRANTRK
                blo     GRAN2T2
                subb    #GRANTRK
                inca
                bra     GRAN2T1
GRAN2T2         cmpa    #DIRTRK
                blo     GRAN2T3
                inca                    step over the directory track
GRAN2T3         sta     DCTRK
                lda     #SECGRAN
                mul                     D = granule-in-track * SECGRAN
                addb    ,s              plus the sector within the granule
                stb     DSEC
                puls    a,b,pc

*******************************************************************
* DIRSRCH - scan the directory for the name in DNAMBF/DEXTBF.
*
*   V973 = sector holding the match, 0 if not found
*   V974 = address of the matching entry inside DBUF0
*   V976 = first granule of the matching file
*   V977 = sector holding the first reusable entry, 0 if none
*   V978 = address of that entry
*
* A byte 0 of $FF means this entry and every entry after it has
* never been used, so the scan can stop there.
*******************************************************************
* Results come back in the V9xx variables, so the index registers
* are preserved: callers hold things like an FCB pointer in X.
DIRSRCH         pshs    x,y,u
                lbsr    DIRSRCH0
                puls    x,y,u,pc

DIRSRCH0        clr     V973
                clr     V977
                ldb     #DIRSEC1
DIRS1           pshs    b
                lda     #DIRTRK
                ldx     #DBUF0
                jsr     DREAD
                puls    b
                ldu     #DBUF0
DIRS2           stu     V974
                lda     ,u
                bne     DIRS3
                bsr     DIRFREE         killed - a candidate for reuse
                bra     DIRS5
DIRS3           coma
                bne     DIRS5           a live entry - compare the name
                bsr     DIRFREE         never used - and the scan ends here
                rts
* compare the 11 byte name and extension
DIRS5           leay    ,u
                ldx     #DNAMBF
DIRS6           lda     ,x+
                cmpa    ,u+
                bne     DIRS7           mismatch - next entry
                cmpx    #DNAMBF+11
                bne     DIRS6
                stb     V973            found it
                lda     DIRGRN,y
                sta     V976
                rts
DIRS7           leau    DIRLEN,y        step to the next entry
                cmpu    #DBUF0+SECLEN
                bne     DIRS2
                incb
                cmpb    #DIRSECN
                bls     DIRS1
                rts

*******************************************************************
* DIRFREE - remember the first entry that can be reused.
*   Entry: ACCB = sector, U = entry address
*******************************************************************
DIRFREE         tst     V977
                bne     DIRFREE9        one is already recorded
                stb     V977
                stu     V978
DIRFREE9        rts

*******************************************************************
* DIRFOUND - raise ?NE unless the last DIRSRCH found the file.
*******************************************************************
DIRFOUND        tst     V973
                bne     DIRFND9
                ldb     #ERRNE
                jmp     LAC46
DIRFND9         rts

*******************************************************************
* DIRWRB - write DBUF0 back to the directory sector in ACCB.
*******************************************************************
DIRWRB          pshs    a,b,x
                lda     #DIRTRK
                ldx     #DBUF0
                jsr     DWRITE
                puls    a,b,x,pc

*******************************************************************
* NAMEGET - parse a file name from the BASIC line into DNAMBF,
* DEXTBF and DCDRV.
*
* Accepts NAME, NAME.EXT, NAME/EXT and a trailing :drive, which is
* the syntax every Disk BASIC program uses.  The name is padded
* with blanks; the default extension is supplied by the caller in X.
*******************************************************************
NAMEDEF         ldx     #DEFEXT         blank default extension
NAMEGET         clr     ,-s             drive-seen flag
                lda     DEFDRV
                sta     DCDRV
                ldu     #DNAMBF
                ldd     #$2008          8 blanks
NAMEG1          sta     ,u+
                decb
                bne     NAMEG1
                ldb     #3              then the default extension
                jsr     LA59A
                jsr     L8748           evaluate a string expression
                leau    ,x              U = the string, B = its length
                cmpb    #2
                blo     NAMEG2
                lda     1,u
                cmpa    #':
                bne     NAMEG2
                lda     ,u
                cmpa    #'0
                blo     NAMEG2
                cmpa    #'3
                bhi     NAMEG2
                bsr     NAMEDRV         a leading n: drive prefix
NAMEG2          ldx     #DNAMBF
                incb                    compensate for the DECB below
NAMEG3          decb
                bne     NAMEG5
                leas    1,s             done - drop the flag
NAMEG4          cmpx    #DNAMBF         was anything stored?
                bne     NAMEG9
NAMEBAD         ldb     #ERRFN
                jmp     LAC46           ?FN - bad file name
NAMEG9          rts
NAMEG5          lda     ,u+
                cmpa    #'.
                beq     NAMEEXT
                cmpa    #'/
                beq     NAMEEXT
                cmpa    #':
                beq     NAMEG6
                cmpx    #DEXTBF         name buffer full?
                beq     NAMEBAD
                bsr     NAMEPUT
                bra     NAMEG3
NAMEG6          bsr     NAMEG4          must have a name before the drive
                bsr     NAMEDRV
                tstb                    nothing may follow the drive
                bne     NAMEBAD
                puls    a,pc

*******************************************************************
* NAMEDRV - take the n: drive prefix or :n suffix at U.
*******************************************************************
NAMEDRV         com     2,s             the drive may only appear once
                beq     NAMEBAD
                lda     ,u++
                subb    #2
                suba    #'0
                blo     NAMEBAD
                cmpa    #3
                bhi     NAMEBAD
                sta     DCDRV
                rts

*******************************************************************
* NAMEEXT - collect the extension after a . or /
*******************************************************************
NAMEEXT         bsr     NAMEG4          must have a name first
                ldx     #DFLTYP
                lda     #$20            blank
NAMEE1          sta     ,-x             blank fill the extension
                cmpx    #DEXTBF
                bne     NAMEE1
NAMEE2          decb
                beq     NAMEE9
                lda     ,u+
                cmpa    #':
                beq     NAMEG6
                cmpx    #DFLTYP         extension buffer full?
                beq     NAMEBAD
                bsr     NAMEPUT
                bra     NAMEE2
NAMEE9          leas    1,s
                rts

*******************************************************************
* NAMEPUT - store one character.
*
* Stored exactly as given.  Disk BASIC never folds case, so a name
* typed in lower case is a different file from the same name in
* upper case - here as there.
*******************************************************************
NAMEPUT         sta     ,x+
                rts

DEFEXT          fcc     '   '           the default (blank) extension
BASEXT          fcc     'BAS'
DATEXT          fcc     'DAT'
BINEXT          fcc     'BIN'
