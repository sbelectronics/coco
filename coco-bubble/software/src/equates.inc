*******************************************************************
* equates.inc - Color BASIC / Extended BASIC / DECB-layout equates
*
* Every ROM address here was checked against the Disk BASIC 1.1
* disassembly in Disk BASIC Unravelled II (Appendix B): where DECB
* calls a Color BASIC routine, this ROM calls the same one at the
* same address.  RAM addresses reproduce the DECB map exactly, so
* software that peeks DECB variables finds them where it expects.
*
* Targets Color BASIC 1.2/1.3 + Extended BASIC 1.1.
*******************************************************************

* ---------------------------------------------------------------
* Direct page variables (Color BASIC / Extended BASIC)
* ---------------------------------------------------------------
CHARAC          equ     $01     terminator flag 1
ENDCUR          equ     $02     terminator flag 2
TMPLOC          equ     $04     scratch
VALTYP          equ     $06     type flag: 0=numeric $FF=string
GARBFL          equ     $07     string housekeeping
TEMPPT          equ     $0B     temporary string stack pointer
LASTPT          equ     $0D     last used string stack address
TEMPTR          equ     $0F     temporary pointer
BOTSTK          equ     $17     bottom of stack at last check
TXTTAB          equ     $19     start of BASIC program
VARTAB          equ     $1B     start of variables
ARYTAB          equ     $1D     start of arrays
ARYEND          equ     $1F     end of arrays +1
FRETOP          equ     $21     start of string storage / top of free RAM
STRTAB          equ     $23     start of string variables
FRESPC          equ     $25     utility string pointer
MEMSIZ          equ     $27     top of string space
OLDTXT          equ     $29     saved line number during STOP
BINVAL          equ     $2B     binary value of converted line number
OLDPTR          equ     $2D     saved input pointer during STOP
TINPTR          equ     $2F     temporary input pointer
VARDES          equ     $3B     pointer to a variable descriptor
FP0EXP          equ     $4F     FPA0 exponent
FP1EXP          equ     $5C     FPA1 exponent
RESSGN          equ     $62     sign of an add or divide result
FPA0            equ     $50     FPA0 mantissa
FP0SGN          equ     $54     FPA0 sign
STRDES          equ     $56     temporary string descriptor
CURLIN          equ     $68     current line number ($FFFF = direct)
DEVPOS          equ     $6C     print position
DEVWID          equ     $6D     print width
PRTDEV          equ     $6E     print device
DEVNUM          equ     $6F     device number: -3..0 system, 1..15 = files
CINBFL          equ     $70     console in buffer flag
RSTFLG          equ     $71     warm start flag ($55 = warm)
RSTVEC          equ     $72     warm start vector
TOPRAM          equ     $74     top of RAM
FILSTA          equ     $78     file status: 0=closed 1=input 2=output
CINCTR          equ     $79     console in buffer char counter
CINPTR          equ     $7A     console in buffer pointer
CBUFAD          equ     $7E     cassette buffer address
ZERO            equ     $8A     two bytes that are always zero
GETNCH          equ     $9F     get NEXT input character (JSR)
GETCCH          equ     $A5     get CURRENT input character (JSR)
CHARAD          equ     $A6     BASIC input pointer
EXECJP          equ     $9D     jump address used by the EXEC command
USRADR          equ     $B0     address of the USR vector table
PMODE           equ     $B6     PMODE mode argument
ENDGRP          equ     $B7     end of current graphics page
BEGGRP          equ     $BA     start of current graphics page
GRPRAM          equ     $BC     number of blocks before graphics pages

* ---------------------------------------------------------------
* DECB direct-page variables (same addresses)
* ---------------------------------------------------------------
DCOPC           equ     $EA     DSKCON op code 0-3
DCDRV           equ     $EB     DSKCON drive number
DCTRK           equ     $EC     DSKCON track number 0..TRKMAX-1
DSEC            equ     $ED     DSKCON sector number 1..SECTRK
DCBPT           equ     $EE     DSKCON data buffer pointer
DCSTA           equ     $F0     DSKCON status byte
FCBTMP          equ     $F1     temporary FCB pointer

* ---------------------------------------------------------------
* Fixed RAM (Color BASIC)
* ---------------------------------------------------------------
SW3VEC          equ     $0100
SW2VEC          equ     $0103
SWIVEC          equ     $0106
NMIVEC          equ     $0109
IRQVEC          equ     $010C
FRQVEC          equ     $010F
COMVEC          equ     $0120   command interpretation vector table (4 x 10)
CMDTBL_CB       equ     COMVEC+0        Color BASIC's table
CMDTBL_ECB      equ     COMVEC+10       Extended BASIC's table
CMDTBL_DSK      equ     COMVEC+20       Disk BASIC's table  (ours)
CMDTBL_USR      equ     COMVEC+30       user table
RVEC0           equ     $015E   RAM hook table: 25 x 3-byte JMP slots
RVEC1           equ     $0161
RVEC2           equ     $0164
RVEC3           equ     $0167
RVEC4           equ     $016A
RVEC5           equ     $016D
RVEC6           equ     $0170
RVEC7           equ     $0173
RVEC8           equ     $0176
RVEC9           equ     $0179
RVEC10          equ     $017C
RVEC11          equ     $017F
RVEC12          equ     $0182
RVEC13          equ     $0185
RVEC14          equ     $0188
RVEC15          equ     $018B
RVEC16          equ     $018E
RVEC17          equ     $0191
RVEC18          equ     $0194
RVEC19          equ     $0197
RVEC20          equ     $019A
RVEC21          equ     $019D
RVEC22          equ     $01A0
RVEC23          equ     $01A3
RVEC24          equ     $01A6
LINBUF          equ     $02DC   BASIC line input buffer
LBUFMX          equ     250     maximum characters in a BASIC line
STRBUF          equ     $03D7   string buffer

* ---------------------------------------------------------------
* Color BASIC ROM entry points  ($A000-$BFFF)
* ---------------------------------------------------------------
LA0E2           equ     $A0E2   BASIC main loop (re-entry after init)
LA171           equ     $A171   get a character from console in
LA176           equ     $A176   get a char from the input buffer
LA282           equ     $A282   send character to console out
LA35F           equ     $A35F   set print parameters
LA37C           equ     $A37C   save the print parameters
LA3ED           equ     $A3ED   verify that the file type was INPUT
LA3FB           equ     $A3FB   'file not open' error
LA406           equ     $A406   test device number
LA426           equ     $A426   close all files
LA429           equ     $A429   close files
LA42D           equ     $A42D   close one file
LA549           equ     $A549   break check
LA59A           equ     $A59A   move (B) bytes from (X) to (U)
LA5A2           equ     $A5A2   get device number
LA5A5           equ     $A5A5   evaluate an expression (device number)
LA5AE           equ     $A5AE   strip device number off the input line
LA5C7           equ     $A5C7   error if further characters on line
LA5DA           equ     $A5DA   BASIC's EOF for a non-disk device
LA5E4           equ     $A5E4   BASIC's EOF statement, entered with ACCB
LA603           equ     $A603   Color BASIC's OPEN, past the disk part
LA616           equ     $A616   'bad file mode' error
LA61C           equ     $A61C   'file already open' error
LA61F           equ     $A61F   'device number' error
LA7D1           equ     $A7D1   wait a while
LA7E9           equ     $A7E9   turn off the cassette motor
LA928           equ     $A928   clear screen
LA951           equ     $A951   SOUND (beep)
LA974           equ     $A974   disable the analog multiplexer
LAC37           equ     $AC37   check for enough room in RAM
LAC44           equ     $AC44   'out of memory' error
LAC46           equ     $AC46   error handler; entry with B = errcode*2
LAC60           equ     $AC60   BASIC's error handler
LAC73           equ     $AC73   return to direct mode
LAC7C           equ     $AC7C   BASIC main loop
LACEF           equ     $ACEF   relocate BASIC's next-line pointers
LAD19           equ     $AD19   NEW: erase variables, reset vectors
LAD21           equ     $AD21   NEW, minus the program erase (moves CHARAD)
LAD26           equ     $AD26   erase variables only, leaving CHARAD alone
LAD33           equ     $AD33   reset the CONT flag
LAD9E           equ     $AD9E   command interpretation loop
LADC6           equ     $ADC6   BASIC main interpretation loop
LADD4           equ     $ADD4   command dispatch: X = jump table, A = index
LADEB           equ     $ADEB   break check if a key is down
LAE15           equ     $AE15   STOP
LAF9A           equ     $AF9A   inside LET: where it calls for the value
LAFA4           equ     $AFA4   BASIC's LET
LAFB1           equ     $AFB1   move string out of a random file buffer
LB069           equ     $B069   re-entry into BASIC INPUT after an item
LB143           equ     $B143   'TM' error if VALTYP = string
LB146           equ     $B146   'TM' error if numeric variable
LB148           equ     $B148   do a 'TM' check
LB156           equ     $B156   evaluate an expression
LB166           equ     $B166   inside expression evaluation, one level in
LB244           equ     $B244   strip prompt string from BASIC
LB262           equ     $B262   syntax check for '(' and evaluate expression
SYNCOMMA        equ     $B26D   syntax check for a comma
SYNCHK          equ     $B26F   syntax check for the character in ACCB
LB277           equ     $B277   'SYNTAX' error
LB2CE           equ     $B2CE   secondary dispatch: X = table, B = index
LB357           equ     $B357   evaluate a variable
LB3E6           equ     $B3E6   evaluate an expression
LB44A           equ     $B44A   'FC' (illegal function call) error
LB4F3           equ     $B4F3   load ACCB into FPA0
GIVABF          equ     $B4F4   convert ACCD to a floating point number
LB50F           equ     $B50F   reserve five bytes in string space
LB516           equ     $B516   put on the temporary string stack
LB654           equ     $B654   get length and address of a string
LB657           equ     $B657   purge string from the string stack
LB659           equ     $B659   get length and address of string 1
LB69B           equ     $B69B   save string descriptor on the string stack
LB6A4           equ     $B6A4   get first byte of a string expression
EVALEXPB        equ     $B70B   evaluate expression; result in ACCB
LB70E           equ     $B70E   evaluate expression (no leading GETNCH)
LB738           equ     $B738   syntax check comma, then evaluate expression
LB73D           equ     $B73D   evaluate expression; result in X
LIST            equ     $B764   LIST - with Z set, lists the whole program
LB958           equ     $B958   print a CR to console out
LB95C           equ     $B95C   send a CR to the screen
LB99F           equ     $B99F   print string to console out
STRINOUT        equ     $B99C   print the string pointed to by X
LB9A2           equ     $B9A2   send filename to console out
LB9AC           equ     $B9AC   send a blank to console out
LB9AF           equ     $B9AF   send a '?' to the screen
LB9C5           equ     $B9C5   floating point add
LBB91           equ     $BB91   floating point divide
LBC14           equ     $BC14   copy a packed FP number from (X) to FPA0
LBC33           equ     $BC33   pack FPA0 into VARDES
LBC35           equ     $BC35   pack FPA0 and store it in string space
LBC5F           equ     $BC5F   copy FPA0 into FPA1
LBCEE           equ     $BCEE   INT: FPA0 to an integer, towards minus infinity
LBDCC           equ     $BDCC   convert ACCD to decimal and print it
LBDD9           equ     $BDD9   convert an FP number to an ASCII string

* ---------------------------------------------------------------
* Extended BASIC ROM entry points ($8000-$9FFF)
* ---------------------------------------------------------------
XBWMST          equ     $80C0   Extended BASIC warm start
L813C           equ     $813C   ECB command interpretation handler
L8168           equ     $8168   ECB secondary command handler
L8311           equ     $8311   ECB CLOAD
L8316           equ     $8316   ECB check for CSAVEM
L836C           equ     $836C   evaluate expression, push 2-byte int on stack
L8748           equ     $8748   evaluate a string expression
L880E           equ     $880E   convert to a floating point number
L894C           equ     $894C   ECB IRQ handler, the entry ECB itself uses
L8955           equ     $8955   the same handler, past the part Disk BASIC
*                               does for itself before chaining in
L8C1B           equ     $8C1B   ECB DLOAD
XVEC9           equ     $8E90   ECB routine used as DECB's RVEC9 (PRINT)
XVEC15          equ     $8846   ECB's expression hook: &H, &O and &B
L95AC           equ     $95AC   reset SAM display page and VDG mode
L962E           equ     $962E   ECB PMODE command
L9650           equ     $9650   ECB PMODE comma path
L96CB           equ     $96CB   readjust line numbers etc.
L96EC           equ     $96EC   initialize ECB variables and do a NEW
L9FB5           equ     $9FB5   unsigned multiply

* ECB internal address used by the GET/PUT hook (RVEC22) to
* recognise that it was reached from ECB's GET/PUT parser.
* ECB 1.1 value; the hook also accepts the 1.0 value.
ECBGETPUT11     equ     $975F
ECBGETPUT10     equ     $9754

* Extended BASIC's own GET/PUT direction flag: 0 for GET, 1 for PUT.
* It is set before the CLS hook fires, so DVEC22 can read it and needs
* no flag of its own.  Verified by peeking it after a graphics GET and
* a graphics PUT, which take the same path.
VD8             equ     $D8

* ---------------------------------------------------------------
* DECB RAM map, reproduced exactly
* ---------------------------------------------------------------
DBUF0           equ     $0600   main sector I/O buffer (256)
DBUF1           equ     $0700   scratch sector buffer (256)
FATBL0          equ     $0800   FAT image, drive 0 - the bubble (74)
FATBL1          equ     $084A   drive 1 - the first floppy
FATBL2          equ     $0894   drive 2
FATBL3          equ     $08DE   drive 3
FCBV1           equ     $0928   FCB pointer table (16 x 2)
RNBFAD          equ     $0948   start of free random file buffer area
FCBADR          equ     $094A   start of the FCBs
DNAMBF          equ     $094C   filename buffer (8)
DEXTBF          equ     $0954   extension buffer (3)
DFLTYP          equ     $0957   file type
DASCFL          equ     $0958   ASCII flag
DRUNFL          equ     $0959   RUN flag
DEFDRV          equ     $095A   default drive number
FCBACT          equ     $095B   number of active FCBs
DRESFL          equ     $095C   reset flag
DLODFL          equ     $095D   load flag
DMRGFL          equ     $095E   merge flag
DUSRVC          equ     $095F   10 USR vectors (20 bytes)
V973            equ     $0973   dir search: existing file sector number
V974            equ     $0974   dir search: existing file RAM image address
V976            equ     $0976   dir search: existing file first granule
V977            equ     $0977   dir search: unused entry sector number
V978            equ     $0978   dir search: unused entry RAM image address
WFATVL          equ     $097A   FAT write-back trigger value
DFFLEN          equ     $097C   direct access record length
DR0TRK          equ     $097E   floppy: head position per drive (4)
NMIFLG          equ     $0982   floppy: this NMI ends a transfer
DNMIVC          equ     $0983   floppy: where that NMI lands (2)
RDYTMR          equ     $0985   motor off timer - loaded, never counted down
DRGRAM          equ     $0986   RAM image of DSKREG
DVERFL          equ     $0987   verify flag: 0=off $FF=on
ATTCTR          equ     $0988   read/write attempt counter
* ---------------------------------------------------------------
* ROM private state - ours, not DECB's
* ---------------------------------------------------------------
* This holds every byte of state the ROM keeps for itself that has to
* survive a sector transfer.  It cannot live in DBUF1, which is the
* verify read-back buffer, and the direct page is full, so it is
* carved out of the RAM DECB spends on record buffers instead.
*
* It sits *below* the buffer area rather than above it.  That puts
* the ceiling on record buffer allocation at FCBADR exactly, which is
* the comparison Disk BASIC itself makes, and keeps it right when
* FILES moves the FCBs up to make room for more buffers.  Being below
* DFLBUF also means cold start's clear covers it.
*
* The size is bounded - see the assert below - so MPIRESV comes first
* and the fields are laid out against it.
MPIRESV         equ     43              size of this block
ROMPRIV         equ     $0989
* MPIBBV and MPIFDV hold the complete latch byte to write - CTS*
* nibble as found plus the device's SCS* slot - so switching to a
* device is one store.  Bit 7 is the "found" marker: it is one of
* the four bits the MPI documents as having no hardware function,
* so it rides along in the stored value harmlessly and zero can
* mean "absent" without ambiguity.
MPIPRES         equ     ROMPRIV+0       non-zero if an MPI answered
MPIBBV          equ     ROMPRIV+1       latch value selecting the bubble
MPIFDV          equ     ROMPRIV+2       latch value selecting the FDC
MPIBASE         equ     ROMPRIV+3       latch slot bits as first found
MPICUR          equ     ROMPRIV+4       latch value for the slot being probed
FDCTRY          equ     ROMPRIV+5       floppy retries left
VFYCTR          equ     ROMPRIV+6       write attempts left when verifying
* COPY walks the source's granule chain and builds the destination's,
* writing a sector at each step, so all of this has to outlive a
* transfer - including the names, which are needed again at the end.
COPYTYP         equ     ROMPRIV+7       file type being copied
COPYASC         equ     ROMPRIV+8       ASCII flag
COPYNM          equ     ROMPRIV+9       source name and extension (11)
COPYNM2         equ     ROMPRIV+20      destination name (11)
COPYDR1         equ     ROMPRIV+31      source drive
COPYDR2         equ     ROMPRIV+32      destination drive
COPYLST         equ     ROMPRIV+33      source bytes in its last sector (2)
COPYSGR         equ     ROMPRIV+35      source first granule
COPYSCUR        equ     ROMPRIV+36      source granule being read
COPYSSEC        equ     ROMPRIV+37      sector within it
COPYSNS         equ     ROMPRIV+38      sectors of it that hold file data
COPYSLF         equ     ROMPRIV+39      non-zero if it is the last granule
COPYDGR         equ     ROMPRIV+40      destination first granule
COPYDCUR        equ     ROMPRIV+41      destination granule being written
COPYDSEC        equ     ROMPRIV+42      sector within it

DFLBUF          equ     ROMPRIV+MPIRESV initial record buffer (256)

* The budget, asserted.  Cold start walks X from DFLBUF up over the
* record buffer and the three FCBs, then rounds the end up to a 256
* byte even page and adds 4 graphics pages to get TXTTAB.  That
* rounding hides growth until suddenly it doesn't: the end lands on
* exactly $0E00 today and TXTTAB on $2601, but one more byte in the
* block above rounds to $0F00, then up to $1000 for the even page,
* and BASIC's program moves to $2801 - breaking every PEEK and PRINT
* MEM that knows where a Disk BASIC program starts.  The a/(b+1)
* idiom is explained in config.inc.
FCBEND          equ     DFLBUF+SECLEN+1+3*FCBLEN
                if      FCBEND/($0E00+1)
                error   "FCB area passes $0E00 - TXTTAB would move off $2601"
                endc

MPIFOUND        equ     $80     "found" marker, a no-function latch bit
MPISCRAT        equ     $CC     the four no-function bits (2,3,6,7)
MPISLOTB        equ     $33     the slot bits (0,1 = SCS*, 4,5 = CTS*)

* ---------------------------------------------------------------
* FAT image offsets
* ---------------------------------------------------------------
FAT0            equ     0       active file counter (blocks FAT reload)
FAT1            equ     1       dirty counter (compared with WFATVL)
* bytes 2..5 unused; granule data starts at FATCON (config.inc)

* ---------------------------------------------------------------
* Directory entry offsets
* ---------------------------------------------------------------
DIRNAM          equ     0       filename (8) - $00 killed, $FF never used
DIREXT          equ     8       extension (3)
DIRTYP          equ     11      file type 0=BASIC 1=data 2=ML 3=text
DIRASC          equ     12      ASCII flag 0=binary $FF=ASCII
DIRGRN          equ     13      first granule
DIRLST          equ     14      number of bytes used in the last sector (2)

* ---------------------------------------------------------------
* File control block offsets
* ---------------------------------------------------------------
FCBTYP          equ     0       $10 input $20 output $40 direct 0 closed
FCBDRV          equ     1       drive number
FCBFGR          equ     2       first granule in the file
FCBCGR          equ     3       current granule
FCBSEC          equ     4       current sector within the granule (1..SECGRAN)
FCBCPT          equ     5       sequential: character pointer
FCBPOS          equ     6       print position
FCBREC          equ     7       direct: record number / seq: whole sectors
FCBRLN          equ     9       direct: record length
* Offsets 9..15 are unused by a sequential file; they hold the file's
* type and format there so that closing it does not have to rely on
* the global DFLTYP/DASCFL still describing this particular file.
FCBFTYP         equ     9       sequential: file type for the directory
FCBFASC         equ     10      sequential: ASCII flag for the directory
FCBBUF          equ     11      direct: pointer to the record buffer
* Disk BASIC keeps a sector offset at 13 and a GET/PUT flag at 15.
* Neither is used here - every record transfer reads its sectors and
* the direction comes from Extended BASIC's VD8 - but the offsets are
* kept so the layout stays Disk BASIC's.
FCBSOF          equ     13      direct: sector offset (Disk BASIC's, unused)
FCBFLG          equ     15      direct: GET/PUT flag (Disk BASIC's, unused)
FCBCFL          equ     16      sequential: cache flag
FCBCDT          equ     17      sequential: cache data byte
FCBDIR          equ     18      directory entry number
FCBLST          equ     19      number of bytes in the last sector (2)
FCBGET          equ     21      direct: INPUT# position within the record
FCBDFL          equ     23      sequential input: data-left flag
FCBPUT          equ     23      direct: PRINT# position within the record
FCBLFT          equ     24      sequential: characters left in the buffer
FCBCON          equ     25      offset to the 256-byte FCB data buffer

* File type codes
INPFIL          equ     $10
OUTFIL          equ     $20
RANFIL          equ     $40

* ---------------------------------------------------------------
* Error codes (value = code number; pass code*2 in ACCB to LAC46)
* ---------------------------------------------------------------
ERRBR           equ     27*2    bad record
ERRDF           equ     28*2    disk full
ERROB           equ     29*2    out of buffer space
ERRWP           equ     30*2    write protected
ERRFN           equ     31*2    bad file name
ERRFS           equ     32*2    bad file structure
ERRAE           equ     33*2    file already exists
ERRFO           equ     34*2    field overflow
ERRSE           equ     35*2    set to non-fielded string
ERRVF           equ     36*2    verification error
ERRER           equ     37*2    write or input past end of record
ERRLOWEST       equ     25*2    lowest error this table covers (UF)
* Color BASIC's own codes (LABAF) - anything below 27 is handed to
* the displaced error driver, which chains onward to Extended BASIC
* (25 UF, 26 NE) and Color BASIC.
ERRFC           equ     4*2     illegal function call
ERROM           equ     6*2     out of memory
ERRFD           equ     17*2    bad file data
ERRAO           equ     18*2    file already open
ERRDN           equ     19*2    device number error
ERRIO           equ     20*2    I/O error
ERRFM           equ     21*2    bad file mode
ERRNO           equ     22*2    file not open
ERRIE           equ     23*2    input past end of file
ERRDS           equ     24*2    direct statement in file
ERRNE           equ     26*2    file not found (Extended BASIC)
