*******************************************************************
* fcb.asm - the file engine: sequential and direct access
*
* A file control block is 25 control bytes followed by a 256 byte
* sector buffer.  A sequential file reads by filling the buffer a
* sector at a time and handing out bytes, and writes by accumulating
* bytes and flushing whole sectors, allocating granules as it goes.
* A direct file moves fixed length records between the media and a
* record buffer outside the FCB, in any order.
*
* Fields common to both:
*   FCBTYP  $10 input, $20 output, $40 direct, 0 closed
*   FCBDRV  the drive the file is on
*   FCBFGR  first granule       FCBCGR  current granule
*   FCBSEC  sector within the granule, 1..SECGRAN
*   FCBPOS  print position within the line
*   FCBDIR  directory entry number
*   FCBLST  bytes used in the last sector (2 bytes, 0..256)
*
* Sequential only:
*   FCBCPT  offset of the next byte in the buffer
*   FCBREC  whole sectors read or written so far (2 bytes)
*   FCBFTYP FCBFASC  type and format for the directory on close
*   FCBCFL  FCBCDT   one character of pushback
*   FCBDFL  $FF once the end of the file has been reached
*   FCBLFT  bytes still unread in the buffer
*
* Direct only - some of these share offsets with the sequential set:
*   FCBREC  the record the file is at, numbered from 1
*   FCBRLN  record length, over FCBFTYP/FCBFASC
*   FCBBUF  address of the record buffer
*   FCBGET  FCBPUT   how far INPUT# and PRINT# have reached within
*                    the record; FCBPUT is over FCBDFL
*******************************************************************

*******************************************************************
* FCBDRVSEL - point DCDRV at the drive this FCB belongs to.
*   Entry: X = FCB.  Preserves everything but DCDRV.
*
* The FAT image, the directory and the media are all selected by
* DCDRV, which is left wherever the last transfer put it.  With two
* files open on different drives that is the wrong drive half the
* time, and allocating from the wrong drive's FAT hands a file a
* granule that belongs to another disk.  So every routine that
* reaches the FAT, the directory or the media on an FCB's behalf
* selects the FCB's drive first.
*******************************************************************
FCBDRVSEL       pshs    a
                lda     FCBDRV,x
                sta     DCDRV
                puls    a,pc

*******************************************************************
* FCBSECRW - read or write the FCB's current sector.
*   Entry: X = FCB, ACCA = 2 read / 3 write
*******************************************************************
FCBSECRW        pshs    a,b,x,u
                pshs    a
                lda     FCBSEC,x
                ldb     FCBCGR,x
                lbsr    GRAN2TS         sets DCTRK and DSEC
                leau    FCBCON,x        the FCB's own sector buffer
                stu     DCBPT
                lda     FCBDRV,x
                sta     DCDRV
                puls    a
* Every disk write the file system makes passes through here, so a
* write through a handle opened for input is refused here as well as
* in FCBPUTB.  A write that got this far would go over the user's
* file; ?FM is the better failure.
                cmpa    #3              a write?
                bne     FCBSECRW1
                ldb     FCBTYP,x
                cmpb    #INPFIL
                beq     FCBSECRWBAD
FCBSECRW1       sta     DCOPC
                jsr     DSKCON
                tst     DCSTA
                lbne    DSKERR
                puls    a,b,x,u,pc
FCBSECRWBAD     puls    a,b,x,u
                jmp     LA616           ?FM - bad file mode

*******************************************************************
* FCBLAST - how many sectors of the current granule belong to the
* file?  Returns ACCA = 0 if this granule is not the last one,
* otherwise the number of used sectors from the FAT terminator.
*   Entry: X = FCB
*******************************************************************
FCBLAST         pshs    b,u
                lbsr    FCBDRVSEL
                lbsr    FATPTR2
                lda     FCBCGR,x
                lda     a,u             the FAT entry for this granule
                cmpa    #$C0
                blo     FCBLAST0        it is a link - not the last
                anda    #$3F            the used sector count
                bra     FCBLAST9
FCBLAST0        clra
FCBLAST9        puls    b,u,pc

*******************************************************************
* FCBNEXTR - step to the next sector when reading.
* Returns carry set at the end of the file.
*   Entry: X = FCB
*******************************************************************
FCBNEXTR        pshs    a,b,u
                bsr     FCBLAST
                tsta
                beq     FCBNR1          not the last granule
                cmpa    FCBSEC,x        was that the final sector?
                bhi     FCBNR1
                orcc    #$01            end of file
                puls    a,b,u,pc
FCBNR1          inc     FCBSEC,x
                lda     FCBSEC,x
                cmpa    #SECGRAN
                bls     FCBNR9          still inside this granule
* follow the chain into the next granule
                lbsr    FATPTR2
                lda     FCBCGR,x
                lda     a,u
                cmpa    #GRANMX
                bhs     FCBNRBAD        not a legal link
                sta     FCBCGR,x
                lda     #1
                sta     FCBSEC,x
FCBNR9          andcc   #$FE
                puls    a,b,u,pc
FCBNRBAD        ldb     #ERRFS
                jmp     LAC46           ?FS - bad file structure

*******************************************************************
* FCBFILL - fill the buffer with the next sector of an input file.
* Sets FCBDFL when there is nothing left to read.
*   Entry: X = FCB
*******************************************************************
FCBFILL         pshs    a,b,u
                lda     #2              read
                bsr     FCBSECRW
                clr     FCBCPT,x
* how much of this sector is real data?  Every sector is full
* except the file's last one, whose length the directory recorded.
                bsr     FCBLAST
                tsta
                beq     FCBFILL1        not the last granule - a full sector
                cmpa    FCBSEC,x
                bne     FCBFILL1        not the last sector - full
                ldd     FCBLST,x        the tail length
                tsta                    256 means a full sector
                bne     FCBFILL1
                stb     FCBLFT,x
                tstb
                bne     FCBFILL9
                lda     #$FF            a zero length tail is the end
                sta     FCBDFL,x
                bra     FCBFILL9
FCBFILL1        clr     FCBLFT,x        256 bytes (counted as 0 = wrap)
FCBFILL9        ldd     FCBREC,x
                addd    #1
                std     FCBREC,x
                puls    a,b,u,pc

*******************************************************************
* FCBATEOF - is the file exhausted?  Returns carry set at the end.
* Non destructive: the position is put back afterwards, so EOF()
* can be asked without consuming anything.
*   Entry: X = FCB
*******************************************************************
* The buffer is refilled as part of consuming its last byte, so the
* end of the file is always already known by the time it matters.
FCBATEOF        tst     FCBDFL,x
                bne     FCBEOFY
                andcc   #$FE
                rts
FCBEOFY         orcc    #$01
                rts

*******************************************************************
* FCBGETB - read one byte from a sequential input file.
*   Entry: X = FCB
*   Exit:  ACCA = the byte, carry set at end of file
*******************************************************************
* FCBLFT counts down from the number of valid bytes, where zero
* stands for a full 256 byte sector: the first decrement takes it
* to 255, so a full sector yields 256 bytes before running out.
* Hitting zero means this was the last byte, and the next sector is
* fetched straight away - which is also how the end of the file is
* discovered.
* Only an input handle may be read. A direct handle reaching here
* would be read as though it were sequential, and the flags it shares
* offsets with - FCBDFL sits where a direct file keeps FCBPUT - would
* be written on the way past.
FCBGETB         pshs    b,u
                lda     FCBTYP,x
                beq     FCBGETEOF       a closed file has nothing in it
                cmpa    #INPFIL
                bne     FCBGETBAD
                tst     FCBCFL,x        a character pushed back?
                beq     FCBGETB0
                lda     FCBCDT,x
                clr     FCBCFL,x
                andcc   #$FE
                puls    b,u,pc
* The buffer offset reaches 255, and accumulator offset indexing is
* signed, so the address has to be formed with a 16 bit offset.
FCBGETB0        tst     FCBDFL,x
                bne     FCBGETEOF
                ldb     FCBCPT,x
                clra
                addd    #FCBCON
                leau    d,x             U = the next byte in the buffer
                lda     ,u              the byte to return
                inc     FCBCPT,x
                dec     FCBLFT,x
                bne     FCBGETB9        more of the buffer left
                pshs    a
                lbsr    FCBNEXTR
                bcs     FCBGETB8        that really was the end
                lbsr    FCBFILL
                puls    a
                bra     FCBGETB9
FCBGETB8        ldb     #$FF
                stb     FCBDFL,x
                puls    a
FCBGETB9        andcc   #$FE
                puls    b,u,pc
FCBGETEOF       lda     #$FF
                sta     FCBDFL,x
                orcc    #$01
                puls    b,u,pc
FCBGETBAD       puls    b,u
                jmp     LA616           ?FM - bad file mode

*******************************************************************
* FCBPUTB - write one byte to a sequential output file.
*   Entry: X = FCB, ACCA = the byte
*******************************************************************
* As in FCBGETB, the buffer offset needs 16 bit indexing: an
* accumulator offset is signed and would run backwards out of the
* buffer and over the control bytes once it passed 127.
*
* Only an output handle may be written.  A write through a read
* handle would land in the sector buffer the reader is working
* through, step FCBCPT past the reader's place, and - once the
* pointer wrapped - have FCBFLUSH write the mangled buffer back over
* the file on the media.  A direct handle is refused for the same
* reason: its FCB holds a record cursor, not a sequential one.
FCBPUTB         pshs    a,b,u
                ldb     FCBTYP,x
                cmpb    #OUTFIL
                bne     FCBPUTBAD
                ldb     FCBCPT,x
                clra
                addd    #FCBCON
                leau    d,x             U = where this byte goes
                lda     ,s              the byte passed in
                sta     ,u
                inc     FCBCPT,x
                lda     FCBCPT,x
                bne     FCBPUTB9        the buffer still has room
                bsr     FCBFLUSH        full - write it out and move on
FCBPUTB9        puls    a,b,u,pc
FCBPUTBAD       puls    a,b,u
                jmp     LA616           ?FM - bad file mode

*******************************************************************
* FCBROOM - make sure the FCB's current sector can be written.
*
* When a granule fills, FCBFLUSH leaves FCBSEC one past SECGRAN as
* a marker rather than allocating the next granule there and then.
* The allocation happens here, when a byte actually arrives for the
* new granule.  Allocating eagerly looks harmless but is not: a
* file that ends exactly on a granule boundary would own a granule
* that was never written, chained on and marked as holding a sector
* of valid data - 256 bytes of garbage on the end of every read.
*   Entry: X = FCB
*******************************************************************
FCBROOM         pshs    a,b,u
                lbsr    FCBDRVSEL
                lda     FCBSEC,x
                cmpa    #SECGRAN
                bls     FCBROOM9        this granule still has room
                lbsr    GRANALLOC       ACCA = the new granule
                pshs    a
                lbsr    FATPTR2
                ldb     FCBCGR,x
                sta     b,u             link the old granule to the new
                puls    a
                sta     FCBCGR,x
                pshs    a
* Take the new granule out of the free pool at once - $C0 (a
* terminator with zero sectors) - so a second open file cannot be
* handed the same granule before the first write marks it properly.
                ldb     #$C0
                stb     a,u
                puls    a
                lda     #1
                sta     FCBSEC,x
                lbsr    FATBUMP
FCBROOM9        puls    a,b,u,pc

*******************************************************************
* FCBFLUSH - write the current buffer out as a whole sector and
* step past it, leaving the granule-full marker for FCBROOM when
* this one is used up.
*   Entry: X = FCB
*******************************************************************
FCBFLUSH        pshs    a,b,u
                bsr     FCBROOM         allocate if the granule was full
                lda     #3              write
                lbsr    FCBSECRW
                clr     FCBCPT,x
                ldd     FCBREC,x
                addd    #1
                std     FCBREC,x
                bsr     FCBMARK         this granule now holds FCBSEC sectors
                inc     FCBSEC,x        may step past SECGRAN - see FCBROOM
                puls    a,b,u,pc

*******************************************************************
* FCBMARK - mark the FCB's current granule as the file's last,
* recording how many of its sectors are in use.
*   Entry: X = FCB
*******************************************************************
FCBMARK         pshs    a,b,u
                lbsr    FCBDRVSEL
                lbsr    FATPTR2
                lda     FCBSEC,x
                ora     #$C0
                ldb     FCBCGR,x
                sta     b,u
                puls    a,b,u,pc

*******************************************************************
* FCBZERO - clear an FCB's 25 control bytes.
*
* The FCBs sit above the block that cold start clears, so opening a
* file must not assume anything about what is in them - a stale
* pushback flag, for instance, would hand the reader a byte that
* was never in the file.
*   Entry: X = FCB
*******************************************************************
FCBZERO         pshs    a,b,x
                ldb     #FCBCON         the 25 control bytes
FCBZERO1        clr     ,x+
                decb
                bne     FCBZERO1
                puls    a,b,x,pc

*******************************************************************
* FCBOPENI - open the file named in DNAMBF for input.
*   Entry: X = FCB
*******************************************************************
FCBOPENI        bsr     FCBZERO
                lbsr    FATVAL
                lbsr    DIRSRCH
                lbsr    DIRFOUND        ?NE if it is not there
                ldu     V974            the directory entry
                lda     #INPFIL
                sta     FCBTYP,x
                lda     DCDRV
                sta     FCBDRV,x
                lda     DIRGRN,u
                sta     FCBFGR,x
                sta     FCBCGR,x
                lda     #1
                sta     FCBSEC,x
                ldd     DIRLST,u
                std     FCBLST,x
                lda     DIRTYP,u
                sta     DFLTYP
                sta     FCBFTYP,x
                lda     DIRASC,u
                sta     DASCFL
                sta     FCBFASC,x
                lbsr    FCBSETDIR       remember which entry this is
                clr     FCBCPT,x
                clr     FCBLFT,x
                clr     FCBDFL,x
                ldd     #0
                std     FCBREC,x
                lbsr    FCBFILL         prime the buffer
                lbsr    FATOPEN
                rts

*******************************************************************
* FCBOPENO - create the file named in DNAMBF and open it for output.
*   Entry: X = FCB
*******************************************************************
FCBOPENO        bsr     FCBZERO
                pshs    x
                lbsr    FATVAL
                lbsr    DIRSRCH
                tst     V973
                beq     FCBOPO1
* it exists already - reclaim it, which is what SAVE over a file
* does; unless something still has it open, which is ?AO
                lbsr    OPNCHK
                ldu     V974
                lda     DIRGRN,u
                pshs    a
                ldu     V974
                clr     DIRNAM,u
                ldb     V973
                lbsr    DIRWRB
                puls    a
                lbsr    GRANFREE
                lbsr    DIRSRCH         find the now-free entry again
FCBOPO1         lbsr    DIRMAKE         a fresh entry and its first granule
                puls    x               the FCB again
                sta     FCBFGR,x
                sta     FCBCGR,x
                lda     #OUTFIL
                sta     FCBTYP,x
                lda     DCDRV
                sta     FCBDRV,x
                lda     DFLTYP          keep this file's own type and
                sta     FCBFTYP,x       format for when it is closed
                lda     DASCFL
                sta     FCBFASC,x
                lbsr    FCBSETDIR       and which directory entry it is
                lda     #1
                sta     FCBSEC,x
                clr     FCBCPT,x
                ldd     #0
                std     FCBREC,x
                std     FCBLST,x
                lbsr    FCBMARK         one sector of it is in use
                lbsr    FATBUMP
                lbsr    FATOPEN
                rts

FCBOPOFULL      ldb     #ERRDF
                jmp     LAC46           ?DF - no free directory entry

*******************************************************************
* DIRMAKE - give the name in DNAMBF a directory entry and a first
* granule.  DIRSRCH must have run and must not have found the name.
*   Entry: DNAMBF/DEXTBF = the name, DFLTYP/DASCFL = its format
*   Exit:  ACCA = the first granule, V973/V974 = the entry,
*          the entry written; raises ?DF if the directory is full
*
* The granule is found but not yet marked in the FAT - the caller
* marks it once it knows how much of it is in use.
*******************************************************************
DIRMAKE         lda     V977            is there a free directory entry?
                lbeq    FCBOPOFULL
                sta     V973
                ldb     V977
                lda     #DIRTRK
                ldx     #DBUF0
                jsr     DREAD           re-read that sector
                ldu     V978
                stu     V974
* build the entry
                leax    ,u
                ldb     #DIRLEN
                clra
DIRMAKE1        sta     ,x+
                decb
                bne     DIRMAKE1
                ldx     #DNAMBF
                ldb     #11
                jsr     LA59A           name and extension into the entry
                ldu     V974
                lda     DFLTYP
                sta     DIRTYP,u
                lda     DASCFL
                sta     DIRASC,u
                lbsr    GRANALLOC       the file's first granule
                sta     DIRGRN,u
                pshs    a
                ldb     V973
                lbsr    DIRWRB          commit the directory entry
                puls    a
                rts

*******************************************************************
* FCBOPEND - open the file named in DNAMBF for direct access.
*   Entry: X = FCB, DFFLEN = record length
*
* Unlike input and output, one mode covers both reading and writing,
* so a direct open neither requires the file to exist nor truncates
* it if it does: a file that is not there is created, and one that is
* is opened as it stands.
*
* Nothing here may touch FCBFTYP or FCBFASC: those are offsets 9 and
* 10, which on a direct file are the two bytes of FCBRLN.  Writing a
* file type of 1 and an ASCII flag of $FF there gives a record length
* of $01FF, and the buffer allocation then runs straight past the
* FCBs.  A direct file's type and format live in its directory entry
* and nowhere else.
*******************************************************************
FCBOPEND        lbsr    FCBZERO
                lbsr    FCBRNBUF        ?OB before anything is committed
* The drive has to go in before the first FAT access: FCBMARK reaches
* the FAT through FCBDRVSEL, which points DCDRV at whatever the FCB
* says, and an FCB still reading zero from FCBZERO would send the
* mark to the bubble however the file was named.
                lda     DCDRV
                sta     FCBDRV,x
                pshs    x
                lbsr    FATVAL
                lbsr    DIRSRCH
                tst     V973
                bne     FCBOPD1         it is already there
                lbsr    DIRMAKE         create it
                puls    x
                sta     FCBFGR,x
                sta     FCBCGR,x
                ldd     #0
                std     FCBLST,x        a new direct file is empty
                lda     #1
                sta     FCBSEC,x
                lbsr    FCBMARK         claim the first granule
                lbsr    FATBUMP
                bra     FCBOPD2
* An existing file keeps the format recorded in its directory entry.
* Its first granule must not be marked here: the entry already holds
* the link to the next granule, and a terminator written over that
* would drop the whole rest of the file.
FCBOPD1         puls    x
                ldu     V974            the directory entry
                lda     DIRGRN,u
                sta     FCBFGR,x
                sta     FCBCGR,x
                ldd     DIRLST,u
                std     FCBLST,x
                lda     DIRTYP,u
                sta     DFLTYP
                lda     DIRASC,u
                sta     DASCFL
                lda     #1
                sta     FCBSEC,x
FCBOPD2         lda     #RANFIL
                sta     FCBTYP,x
                lbsr    FCBSETDIR
                ldd     #1
                std     FCBREC,x        records are numbered from 1
                lbsr    FCBRNCOM        the open cannot fail now
                lbsr    FATOPEN
                rts

*******************************************************************
* GETPUT - the record engine behind GET #n and PUT #n.
*
*   GET #n [, record]
*   PUT #n [, record]
*
* Reached from DVEC22, which has already taken the statement over.
* VD8 says which way the data goes: 0 for GET, non-zero for PUT.
* Extended BASIC sets it; this ROM only reads it.
*
* A record number sets the file position; without one the record the
* file is already at is used.  Either way the position then steps on,
* so a bare GET #1 repeated walks the file.
*******************************************************************
GETPUT          lbsr    GPFCB
* PRINT# and INPUT# within a record begin again at the start of it on
* every GET or PUT, as they do in Disk BASIC.
                clr     FCBPOS,x
                ldd     #0
                std     FCBGET,x
                std     FCBPUT,x
                jsr     GETCCH
                beq     GETPUT1         no number given - use the FCB's
                jsr     SYNCOMMA
                jsr     LB73D           evaluate it; result in X
                tfr     x,d
                ldx     FCBTMP
                std     FCBREC,x
GETPUT1         jsr     LA5C7           nothing else may follow
                ldx     FCBTMP
                ldd     FCBREC,x
                lbeq    GPBADREC        ?BR - records are numbered from 1

* RECOFF takes the FCB in X and gives back the sector counted from the
* start of the file, so the FCB comes from FCBTMP after this.
*
* A record that starts past the last sector the drive has is refused
* here, before anything is allocated.  Without the check a PUT to a
* wild record number would walk the chain allocating every free
* granule on the drive and only then fail with ?DF, leaving the file
* owning the whole disk.  Disk BASIC bounds it the same way (LC306).
                lbsr    RECOFF
                cmpx    #GRANMX*SECGRAN
                lbhs    GPBADREC        ?BR - off the end of the drive
                pshs    b               the byte offset within that sector
                lbsr    GPLOCATE
                puls    b
                lbsr    GPXFER
                ldx     FCBTMP
                ldd     FCBREC,x
                addd    #1
                std     FCBREC,x
                rts

GPBADREC        ldb     #ERRBR
                jmp     LAC46           ?BR - bad record
GPEOF           ldb     #ERRIE
                jmp     LAC46           ?IE - input past end of file
GPBADFS         ldb     #ERRFS
                jmp     LAC46           ?FS - bad file structure

*******************************************************************
* GPGROWN - the file has just grown into a new last sector.
*
* FCBLST counts the bytes used in the file's last sector, and GPLAST
* only ever raises it.  When the last sector moves, the count from the
* old one has to go, or it stays attached to a sector it never
* described: a record written at the top of the new last sector would
* leave the count from the old one standing, and a GET could then read
* bytes of the new sector that were never written instead of reaching
* the end of the file.  Disk BASIC resets it at the same point (LC3AD).
*******************************************************************
GPGROWN         pshs    x
                ldx     FCBTMP
                clr     FCBLST,x
                clr     FCBLST+1,x
                puls    x,pc

*******************************************************************
* GPFCB - the '#n' of a direct file, and its FCB.
*   Exit: X = the FCB, FCBTMP = the FCB, its drive selected
*
* Shared by GET, PUT and FIELD, all of which begin the same way and
* all of which are meaningless on anything but a direct file.
*******************************************************************
GPFCB           jsr     LA5A5           the device number, past the '#'
                clr     DEVNUM          messages go to the screen
                tstb
                lble    LB44A           ?FC - not a disk file
                lbsr    GETFCBB
                lda     FCBTYP,x
                lbeq    LA3FB           ?NO - file not open
                cmpa    #RANFIL
                lbne    LA616           ?FM - only a direct file has records
                stx     FCBTMP
                lbsr    FCBDRVSEL
                rts

*******************************************************************
* GPLOCATE - point the FCB at a sector of the file.
*   Entry: X = the sector, counted from the start of the file
*          FCBTMP = the FCB
*   Exit:  FCBCGR and FCBSEC set
*
* The sector number divides into a count of granules to walk from the
* file's first, and a sector within the granule that lands on.
*
* Running off the end of the chain is the end of the file: a GET there
* is ?IE, and a PUT there grows the file, which is the point of a
* direct file being writable at any record.
*******************************************************************
GPLOCATE        pshs    a,b,x,u
                tfr     x,d
* D / SECGRAN by subtraction: the quotient is how many granules to step
* over, the remainder is the sector inside the one that lands on.
* SECGRAN is 9 on the hardware and 3 on the mock image, so there is no
* shift to use here.
                ldx     #0              X counts granules
GPLOC1          cmpd    #SECGRAN
                blo     GPLOC2
                subd    #SECGRAN
                leax    1,x
                bra     GPLOC1
GPLOC2          incb                    the FCB counts sectors from 1
                pshs    b               the sector within its granule
* walk X granules along the chain
                ldu     FCBTMP
                lda     FCBFGR,u
                lbsr    FATPTR2         U = the granule data
GPLOC3          cmpx    #0
                beq     GPLOC6          this is the granule
                ldb     a,u             the link out of it
                cmpb    #$C0
                blo     GPLOC5          a link - follow it
* The chain ends here, so the file has to grow.  Linking the old last
* granule to a new one also declares it full, which is what makes the
* sectors between the old end and this record part of the file.
                tst     VD8
                lbeq    GPEOF           a GET cannot read what is not there
                pshs    a,x
                lbsr    GRANALLOC
                tfr     a,b
                puls    a,x
                stb     a,u             link the old last granule to it
                tfr     b,a
                ldb     #$C0+1
                stb     a,u             one sector of the new one in use
                lbsr    FATBUMP
                lbsr    GPGROWN
                leax    -1,x
                bra     GPLOC3
* A link has to name a granule that exists.  The FAT is indexed with
* an accumulator offset, which is signed, so a corrupt link of 128 or
* more would read from in front of the table rather than past its end.
GPLOC5          cmpb    #GRANMX
                lbhs    GPBADFS
                tfr     b,a
                leax    -1,x
                bra     GPLOC3
* The granule is found.  Is the sector wanted inside what the file
* occupies?  A link out means the granule is full, so every sector of
* it is; a terminator says how many are.
GPLOC6          pshs    a               the granule
                ldb     a,u
                cmpb    #$C0
                blo     GPLOC8          full granule - nothing to check
                andb    #$3F            sectors of it in use
                cmpb    1,s             against the one wanted
                bhs     GPLOC8          it is within the file
                tst     VD8
                lbeq    GPEOF           a GET past the end
                lda     1,s             extend the granule to this sector
                ora     #$C0
                ldb     ,s
                sta     b,u
                lbsr    FATBUMP
                lbsr    GPGROWN
GPLOC8          puls    a
                ldx     FCBTMP
                sta     FCBCGR,x
                puls    a
                sta     FCBSEC,x
                puls    a,b,x,u,pc

*******************************************************************
* GPNEXT - step the FCB on to the next sector of the file.
*   Entry: X = FCB
*
* Like FCBNEXTR, except that a PUT is allowed to run past the end,
* growing the file instead of stopping at it.
*******************************************************************
GPNEXT          pshs    a,b,u
                lbsr    FATPTR2
                lda     FCBSEC,x
                cmpa    #SECGRAN
                bhs     GPNXGRAN        this granule is full
                inca
                sta     FCBSEC,x
* A terminator that stops short of this sector has to be extended.
                lda     FCBCGR,x
                ldb     a,u
                cmpb    #$C0
                blo     GPNX9           a link out - the granule is full
                andb    #$3F
                cmpb    FCBSEC,x
                bhs     GPNX9           already inside the file
                tst     VD8
                lbeq    GPEOF
                ldb     FCBSEC,x
                orb     #$C0
                stb     a,u
                lbsr    FATBUMP
                lbsr    GPGROWN
                bra     GPNX9
GPNXGRAN        lda     FCBCGR,x
                ldb     a,u
                cmpb    #$C0
                blo     GPNXLINK        a link - follow it
                tst     VD8
                lbeq    GPEOF
                pshs    a
                lbsr    GRANALLOC
                tfr     a,b
                puls    a
                stb     a,u             link this granule to the new one
                tfr     b,a
                ldb     #$C0+1
                stb     a,u
                lbsr    FATBUMP
                lbsr    GPGROWN
                bra     GPNXSET
GPNXLINK        cmpb    #GRANMX         see GPLOC5
                lbhs    GPBADFS
                tfr     b,a
GPNXSET         sta     FCBCGR,x
                lda     #1
                sta     FCBSEC,x
GPNX9           puls    a,b,u,pc

*******************************************************************
* GPXFER - move one record between the record buffer and the file.
*   Entry: ACCB = byte offset of the record within the FCB's current
*          sector, which GPLOCATE has already selected
*          FCBTMP = the FCB
*
* A record is not aligned to a sector and may be longer than one, so
* this walks sectors until the record length is used up.
*
* A PUT reads each sector before writing it.  A record rarely fills a
* sector and the rest of that sector belongs to other records, so
* writing without reading first would destroy them.
*
* The frame, once all three are down:
*   0,s 1,s   where the next bytes go in the record buffer
*   2,s 3,s   bytes of the record still to move
*   4,s       byte offset within the current sector
*******************************************************************
GPXFER          pshs    a,b,x,u
                pshs    b               the offset within the sector
                ldx     FCBTMP
                ldd     FCBRLN,x
                pshs    b,a             bytes still to move
                ldd     FCBBUF,x
                pshs    b,a             the record buffer
* --- one sector at a time ----------------------------------------
* How many bytes of this sector belong to the record: what is left of
* the sector, or what is left of the record, whichever is smaller.
GPX1            ldd     #SECLEN
                subb    4,s
                sbca    #0
                cmpd    2,s
                bls     GPX2
                ldd     2,s             the record ends inside this sector
GPX2            pshs    b,a             and now 0,s is that count
                lda     #2
                ldx     FCBTMP
                lbsr    FCBSECRW        read the sector
                leau    FCBCON,x
                clra
                ldb     6,s             the offset within it
                leau    d,u
                ldx     2,s             where in the record buffer
* The count runs from 1 to SECLEN, and SECLEN is 256, so its low byte
* alone drives the loop: zero stands for a full sector, and the first
* decrement takes it to 255, giving 256 moves.
                ldb     1,s
                tst     VD8
                bne     GPXPUT
GPXGET1         lda     ,u+
                sta     ,x+
                decb
                bne     GPXGET1
* Only now is it known how far into the sector the record reached, so
* the end of file test comes after the move rather than before it: the
* bytes copied are discarded by the error either way.
                ldx     FCBTMP
                clra
                ldb     6,s
                addd    ,s
                lbsr    GPCHKEOF
                bra     GPX3
GPXPUT          lda     ,x+
                sta     ,u+
                decb
                bne     GPXPUT
                ldx     FCBTMP
                lda     #3
                lbsr    FCBSECRW        write it back
* and remember how far into the file this reaches
                clra
                ldb     6,s
                addd    ,s
                lbsr    GPLAST
GPX3            ldd     2,s             step the record buffer on
                addd    ,s
                std     2,s
                ldd     4,s             and count the bytes off
                subd    ,s
                std     4,s
                leas    2,s
                ldd     2,s
                beq     GPX9            the record is complete
                clr     4,s             the next sector starts at its top
                ldx     FCBTMP
                lbsr    GPNEXT
                lbra    GPX1
GPX9            leas    5,s
                puls    a,b,x,u,pc

*******************************************************************
* FIELD - divide a direct file's record buffer into string variables.
*
*   FIELD #n, len AS var$ [, len AS var$ ...]
*
* Each variable's string descriptor is pointed straight at its slice
* of the record buffer, so the variable *is* that part of the record:
* a GET makes the new contents visible through it with no copying, and
* LSET or RSET writes through it into the record.
*
* Nothing is copied and no string space is used, which is why a
* FIELDed variable must not be assigned to with plain LET - that would
* point the descriptor back into string space and quietly break the
* connection.  LSET and RSET are the way to write one.
*
* The fields are cumulative and may not outgrow the record: ?FO.
*
* The frame, once a field's length is down:
*   0,s       this field's length
*   1,s 2,s   where in the record buffer it starts
*   3,s 4,s   total length of the fields so far
*   5,s 6,s   the FCB
*******************************************************************
FIELD           lbsr    GPFCB           the file, which must be direct
                ldd     #0
                pshs    x,b,a           the FCB, and no fields yet
FIELD1          jsr     GETCCH
                bne     FIELD2
                puls    a,b,x,pc        end of the statement
FIELD2          jsr     LB738           a comma, then the length in ACCB
                pshs    x,b             the length, and room for its address
                clra
                addd    3,s             the running total
                blo     FIELDFO         it will not even fit in 16 bits
                ldx     5,s
                cmpd    FCBRLN,x
                bhi     FIELDFO         past the end of the record
                ldu     3,s             where this field starts
                std     3,s
                ldd     FCBBUF,x
                leau    d,u
                stu     1,s
                ldb     #$FF            secondary tokens carry this prefix
                jsr     SYNCHK
                ldb     #DHISEC         the AS token
                jsr     SYNCHK
                jsr     LB357           the variable it names
                jsr     LB146           ?TM if it is numeric
* X now points at the variable's descriptor: length, a spare byte, then
* the address.  Writing the record buffer slice into it is what makes
* the variable a window on the record.
                puls    b,u
                stb     ,x
                stu     2,x
                bra     FIELD1

FIELDFO         ldb     #ERRFO
                jmp     LAC46           ?FO - field overflow

*******************************************************************
*******************************************************************
* LSET / RSET - write a string into a FIELDed variable.
*
*   LSET var$ = expr      left justified within the field
*   RSET var$ = expr      right justified
*
* The field is blank filled first, so that nothing of what was there
* before shows through the part the new data does not cover, and the
* data is then copied in.  A string at least as long as the field is
* truncated on the right, which makes it left justified whichever of
* the two was asked for.
*
* This is the only way to write a FIELDed variable.  Plain assignment
* would point its descriptor back into string space, breaking the
* connection to the record, and MID$ does not write through to the
* record either.  A variable whose bytes are not inside a record
* buffer has no slot in a record to write into, which is ?SE.
*
* The bound is RNBFAD rather than FCBADR, which is what Disk BASIC
* compares against: buffers are handed out below RNBFAD, so anything
* at or above it is unallocated space rather than a field.
*******************************************************************
LSET            clra                    left justified
                bra     LRSET
RSET            lda     #1              right justified
LRSET           pshs    a
                jsr     LB357           the variable
                jsr     LB146           ?TM if it is numeric
                pshs    x               its descriptor
                ldx     2,x             and where its bytes are
                cmpx    #DFLBUF
                blo     LRSETSE
                cmpx    RNBFAD
                bhs     LRSETSE
                ldb     #$B3            the '=' token
                jsr     SYNCHK
                jsr     L8748           the data: X = bytes, ACCB = length
                puls    y               the field
                lda     ,y
                beq     LRSET9          a zero length field takes nothing
                pshs    b               how much data there is
* blank fill the whole field
                ldb     #$20
                ldu     2,y
LRSET1          stb     ,u+
                deca
                bne     LRSET1
                ldb     ,s+
                beq     LRSET9          nothing to copy in
                cmpb    ,y
                blo     LRSET2
                ldb     ,y              as long as the field or longer:
                clr     ,s              truncate, and that is left justified
LRSET2          ldu     2,y
                tst     ,s+
                beq     LRSET8
* RSET: start the copy (field length - data length) bytes in, which is
* the data length negated and the field length added.
                pshs    b
                clra
                negb
                sbca    #0
                addb    ,y
                adca    #0
                leau    d,u
                puls    b
LRSET8          jmp     LA59A           move ACCB bytes from X to U
LRSET9          puls    a,pc
LRSETSE         ldb     #ERRSE
                jmp     LAC46           ?SE - set to non-fielded string

*******************************************************************
* FLDMOVE - relocate FIELDed string descriptors after the record
* buffer area has been compacted.
*   Entry: X = the start of the hole that was closed
*          ACCD = how many bytes it was
*
* Called from FCBRNFRE, which slides the buffers above a closed file's
* down over it.  A variable FIELDed onto a buffer that moved has to
* have its descriptor moved by the same amount, or it goes on pointing
* at bytes that now belong to a different file.  Variables FIELDed onto
* the closed file's own buffer no longer refer to anything, so they are
* emptied.
*
* Only descriptors inside the buffer area are candidates: an ordinary
* BASIC string lives in string space near the top of RAM, above
* RNBFAD, and must not be touched.
*
* Disk BASIC does this too, but sends a descriptor *below* the hole to
* be cleared instead of leaving it alone - its own listing marks that
* branch as a bug.  Here it is left alone, which is what a field on a
* still-open lower buffer needs.
*
* U points at the hole throughout, rather than the stack being indexed
* directly, because the array walk keeps a value of its own on top.
*******************************************************************
FLDMOVE         pshs    a,b,x,u
                pshs    b,a             how big the hole was
                pshs    x               where it started
                leau    ,s              0,1 = start, 2,3 = size
* simple variables first: two name bytes then a five byte descriptor
                ldx     VARTAB
FLDM1           cmpx    ARYTAB
                beq     FLDM3
                lda     1,x             the second letter of the name
                leax    2,x             on to the descriptor
                bpl     FLDM2           bit 7 clear: a number, not a string
                bsr     FLDMONE
FLDM2           leax    5,x
                bra     FLDM1
* then arrays, whose header gives the offset to the next one
FLDM3           cmpx    ARYEND
                beq     FLDM9
                tfr     x,d
                addd    2,x
                pshs    b,a             where the next array starts
                lda     1,x
                bpl     FLDM5           a numeric array
                ldb     4,x             how many dimensions
                aslb                    two bytes each
                addb    #5              and five bytes of header
                clra
                leax    d,x             the first element
FLDM4           cmpx    ,s
                beq     FLDM5
                bsr     FLDMONE
                leax    5,x
                bra     FLDM4
FLDM5           puls    a,b
                tfr     d,x
                bra     FLDM3
FLDM9           leas    4,s
                puls    a,b,x,u,pc

* One descriptor, at X, with U pointing at the hole.
FLDMONE         ldd     2,x             the string address
                cmpd    RNBFAD
                bhs     FLDMO9          above the buffer area: string space
                cmpd    ,u
                blo     FLDMO9          below the hole: someone else's field
                subd    ,u              how far into the hole it would be
                cmpd    2,u
                blo     FLDMOCLR        inside it: the buffer is gone
                ldd     2,x
                subd    2,u             it slid down by the size of the hole
                std     2,x
FLDMO9          rts
FLDMOCLR        clr     ,x              no length
                clr     2,x
                clr     3,x             and no address
                rts

*******************************************************************
* GPISLAST - is the FCB's current sector the last one the file has?
*   Entry: X = FCB
*   Exit:  Z set if it is.  ACCD preserved.
*
* A link out of the granule means the granule is full and so the file
* goes on past it; a terminator names the last sector in use.
*
* PULS does not disturb CC, so the flags from the comparison survive
* the return, as they do in GETFCB.
*******************************************************************
GPISLAST        pshs    a,b,u
                lbsr    FATPTR2
                lda     FCBCGR,x
                lda     a,u
                cmpa    #$C0
                blo     GPISL9          a link out - not the last granule
                anda    #$3F
                cmpa    FCBSEC,x
                puls    a,b,u,pc
GPISL9          andcc   #$FB            clear Z: the file continues
                puls    a,b,u,pc

*******************************************************************
* GPCHKEOF - a GET may not read past what was written.
*   Entry: ACCD = where the record ends within this sector
*          X = FCB
*
* Only the last sector of the file needs the test: every sector before
* it is full by definition.  FCBLST is how much of that last one the
* file actually occupies, so a record reaching beyond it is reading
* bytes that were never written - which is the end of the file, not a
* sector of stale media.
*******************************************************************
GPCHKEOF        lbsr    GPISLAST
                bne     GPCHK9          not the last sector
                cmpd    FCBLST,x
                lbhi    GPEOF           ?IE
GPCHK9          rts

*******************************************************************
* GPLAST - after a PUT, record how much of the file is in use.
*   Entry: ACCD = where the record ends within this sector
*          X = FCB
*
* FCBLST is the number of bytes used in the file's last sector: it is
* what the directory entry carries and what tells a later GET where the
* file ends.  It changes only when the sector just written is the last
* one the file has, and it only ever grows - a PUT over an earlier
* record must not shorten the file.
*******************************************************************
GPLAST          lbsr    GPISLAST
                bne     GPLAST9         not the last sector
                cmpd    FCBLST,x
                bls     GPLAST9         the file already reaches further
                cmpd    #SECLEN
                bls     GPLAST8
                ldd     #SECLEN
GPLAST8         std     FCBLST,x
GPLAST9         rts

*******************************************************************
* FCBRNBUF - reserve this file's record buffer.
*
* Record buffers are handed out from RNBFAD upward and the area ends
* where the FCBs begin, so DFFLEN bytes have to fit below FCBADR.
* FILES can move FCBADR up to enlarge the area.
*   Entry: X = FCB, DFFLEN = record length
*   Exit:  FCBBUF and FCBRLN set; raises ?OB if the record does not
*          fit.  RNBFAD is NOT advanced - see FCBRNCOM.
*
* The sum is checked for carry as well as against FCBADR: a record
* length near $FFFF would otherwise wrap and look like it fitted.
*******************************************************************
FCBRNBUF        pshs    a,b,u
                ldu     RNBFAD
                ldd     DFFLEN
                addd    RNBFAD
                blo     FCBRNBOB        the addition wrapped
                cmpd    FCBADR
                bhi     FCBRNBOB
                stu     FCBBUF,x
                ldu     DFFLEN
                stu     FCBRLN,x
                puls    a,b,u,pc
FCBRNBOB        puls    a,b,u
                ldb     #ERROB
                jmp     LAC46           ?OB - out of buffer space

*******************************************************************
* FCBRNCOM - commit the buffer FCBRNBUF reserved.
*   Entry: X = FCB
*
* Opening a direct file can still fail with ?DF after the buffer has
* been sized, and at that point the FCB is not yet marked open, so
* nothing would ever hand the space back.  So the reservation is
* checked early, when ?OB is the honest answer and no file has been
* created, and only becomes real here, once the open cannot fail.
*******************************************************************
FCBRNCOM        pshs    a,b
                ldd     FCBBUF,x
                addd    FCBRLN,x
                std     RNBFAD
                puls    a,b,pc

*******************************************************************
* FCBRNFRE - give this file's record buffer back.
*
* Buffers are a stack only in the order they were taken, and files
* are not required to close in that order, so closing one in the
* middle slides everything above it down over the hole.
*   Entry: X = FCB
*
* FIELD points BASIC strings straight into these buffers, so the slide
* has to relocate their descriptors too - FLDMOVE does that.
*******************************************************************
FCBRNFRE        pshs    a,b,x,u
                ldd     FCBBUF,x        nothing reserved?
                lbeq    FCBRNF8
                pshs    b,a             the hole this leaves
                ldd     FCBRLN,x
                pshs    b,a             and how big it is
* The pointers go first, while the length is still to hand: any file
* whose buffer sits above the hole is about to be moved down by the
* size of it, so its own pointer has to follow.  Disk BASIC omits
* this, which is why closing two direct files in the order they were
* opened leaves the second one pointing at where its buffer was.
                ldb     FCBACT
                incb                    the system FCB counts too
                ldu     #FCBV1
FCBRNF1         pshs    b
                ldx     ,u++
                lda     FCBTYP,x
                cmpa    #RANFIL
                bne     FCBRNF2
                ldd     FCBBUF,x
                cmpd    3,s             above the hole?
                bls     FCBRNF2         no - at it, or below it
                subd    1,s
                std     FCBBUF,x
FCBRNF2         puls    b
                decb
                bne     FCBRNF1
* and the FIELDed variables pointing into what is about to move
                ldx     2,s
                ldd     ,s
                lbsr    FLDMOVE
* now close the hole up
                ldd     2,s
                tfr     d,x             X = where the bytes land
                addd    ,s
                tfr     d,u             U = where they come from
FCBRNF3         cmpu    RNBFAD
                bhs     FCBRNF4
                lda     ,u+
                sta     ,x+
                bra     FCBRNF3
FCBRNF4         stx     RNBFAD
                leas    4,s
FCBRNF8         puls    a,b,x,u,pc

*******************************************************************
* FATOPEN / FATSHUT - count the files holding the FAT image open.
* While the count is non-zero the image is not re-read from the
* media, because it may hold allocations not yet written back.
*******************************************************************
FATOPEN         pshs    a,x
                lbsr    FCBDRVSEL
                lbsr    FATPTR
                inc     FAT0,x
                puls    a,x,pc

FATSHUT         pshs    a,x
                lbsr    FCBDRVSEL
                lbsr    FATPTR
                lda     FAT0,x
                beq     FATSHUT9
                dec     FAT0,x
FATSHUT9        puls    a,x,pc

*******************************************************************
* FCBCLOSE - close one file.
*   Entry: X = FCB
*
* An output file has to have its tail written, its last granule
* marked and the directory told how long it ended up being.
*******************************************************************
FCBCLOSE        lda     FCBTYP,x
                lbeq    FCBCLOS9        already closed
                cmpa    #RANFIL
                lbeq    FCBCLOSD
                cmpa    #OUTFIL
                bne     FCBCLOSI
* --- output ---
                lda     FCBCPT,x
                beq     FCBCLOSO1
                lbsr    FCBROOM         the partial sector may be the
                clra                    first one of a fresh granule
                ldb     FCBCPT,x
                pshs    a,b
                lda     #3
                lbsr    FCBSECRW
                lbsr    FCBMARK
                puls    a,b
                bra     FCBCLOSO2
FCBCLOSO1       ldd     FCBREC,x
                beq     FCBCLOSO3       nothing was ever written
                ldd     #SECLEN         the last sector was exactly full
                bra     FCBCLOSO2
FCBCLOSO3       clra                    an empty file still owns one sector
                clrb
                pshs    a,b
                lda     #3
                lbsr    FCBSECRW
                lbsr    FCBMARK
                puls    a,b
FCBCLOSO2       std     FCBLST,x
* write the length and type back into the directory entry
                pshs    x
                lbsr    FCBDIRUP
                puls    x
                lbsr    FATWR
FCBCLOSI        lbsr    FATSHUT         release this file's hold on the FAT image
FCBCLOS9        clr     FCBTYP,x
                rts

* --- direct access ---
* A direct file has already written every record it was given, so
* there is no tail to flush; what is left is to give the record
* buffer back and get the length and the granule chain onto the
* media.  Its type and format were written when it was created and
* are not the FCB's to restate.
FCBCLOSD        lbsr    FCBRNFRE
                lbsr    FCBDIRLN
                lbsr    FATWR
                bra     FCBCLOSI

*******************************************************************
* FCBSETDIR - remember which directory entry this file occupies.
*   Entry: X = FCB, with V973/V974 describing the entry
*
* Recording the entry number means closing the file does not have to
* search for it by name again, which would find the wrong file if
* anything else had been named in between.
*******************************************************************
FCBSETDIR       pshs    a,b
                lda     V973
                suba    #DIRSEC1
                ldb     #SECLEN/DIRLEN
                mul                     entries in the sectors before it
                pshs    b
                ldd     V974
                subd    #DBUF0          its offset within the sector
                lsrb
                lsrb
                lsrb
                lsrb
                lsrb                    / DIRLEN
                addb    ,s+
                stb     FCBDIR,x
                puls    a,b,pc

*******************************************************************
* FCBGETDIR - read back the directory sector holding this file's
* entry, leaving V973 = sector and V974 = the entry's address.
*   Entry: X = FCB
*******************************************************************
FCBGETDIR       pshs    a,b,x
                ldb     FCBDIR,x
                clra                    A counts whole sectors
FCBGD1          cmpb    #SECLEN/DIRLEN
                blo     FCBGD2
                subb    #SECLEN/DIRLEN
                inca
                bra     FCBGD1
FCBGD2          adda    #DIRSEC1
                sta     V973
                pshs    b               the index within that sector
                ldb     V973
                lda     #DIRTRK
                ldx     #DBUF0
                jsr     DREAD
                puls    b
                lda     #DIRLEN
                mul
                addd    #DBUF0
                std     V974
                puls    a,b,x,pc

*******************************************************************
* FCBDIRUP - copy the final length back into the directory entry.
*   Entry: X = FCB
*******************************************************************
FCBDIRUP        pshs    a,b,x,u
                bsr     FCBDIRU1
                lda     FCBFTYP,x       this file's own type, not the
                sta     DIRTYP,u        globals, which may have moved on
                lda     FCBFASC,x
                sta     DIRASC,u
                bra     FCBDIRU2

*******************************************************************
* FCBDIRLN - the same, but the length only.
*   Entry: X = FCB
*
* For a direct file there are no type bytes in the FCB to copy back:
* offsets 9 and 10 are its record length.  The entry's own type and
* format are left exactly as they are.
*******************************************************************
FCBDIRLN        pshs    a,b,x,u
                bsr     FCBDIRU1
FCBDIRU2        ldb     V973
                lbsr    DIRWRB
                puls    a,b,x,u,pc

* Common part: find this file's directory entry and put the length
* in it.  Leaves U pointing at the entry.
FCBDIRU1        lda     FCBDRV,x
                sta     DCDRV
                lbsr    FCBGETDIR
                ldu     V974
                ldd     FCBLST,x
                std     DIRLST,u
                rts

*******************************************************************
* CLOSEALL - close every file, including the system FCB.
*******************************************************************
* The guard matters: closing a file can raise an error, the error
* driver closes all files, and without it that becomes a loop.
CLOSEALL        tst     CLOSING
                bne     CLOSEA9
                pshs    a,b,x,u
                com     CLOSING
                ldb     FCBACT
                incb                    plus the system FCB
CLOSEA1         pshs    b
                lbsr    GETFCBB         X = that FCB
                lbsr    FCBCLOSE
                puls    b
                decb
                bne     CLOSEA1
                clr     CLOSING
                puls    a,b,x,u,pc
CLOSEA9         rts
