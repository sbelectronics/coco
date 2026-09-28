*******************************************************************
* stubs.asm - command interpretation stub, token tables, dispatch
*
* COMVEC holds four 10-byte stubs (Color BASIC, Extended BASIC,
* Disk BASIC, user).  This ROM takes the Disk BASIC slot.  Token
* values are base + dictionary index, so keeping DECB's dictionary
* order gives DECB's exact token values - which is what makes
* tokenized program files interchangeable with real disk systems.
*
* CMDSTUB must be immediately followed by VECIMG: the cold start
* code walks straight from the end of one into the other.
*******************************************************************

DLOTOK          equ     $CE             lowest command token (DIR)
DHITOK          equ     $E2             highest command token (BUBDIAG)
* $CE-$E1 are Disk BASIC's own twenty words, in Disk BASIC's order,
* so a tokenised program stays byte-identical to one saved on a real
* disk system.  BUBDIAG is appended at $E2 - a token DECB never
* used - which extends the range without shifting anything in it.
DLOSEC          equ     $A2             lowest secondary token (CVN)
DHISEC          equ     $A7             highest secondary token (AS)

*******************************************************************
* CMDSTUB - the 10-byte command interpretation table
*******************************************************************
CMDSTUB         fcb     DHITOK-DLOTOK+1 number of reserved words (20)
                fdb     CMDDICT         command dictionary
                fdb     CMDHAND         command handler
                fcb     DHISEC-DLOSEC+1 number of secondary functions (6)
                fdb     SECDICT         secondary dictionary
                fdb     SECHAND         secondary handler

*******************************************************************
* VECIMG - ROM image of the RAM hook addresses, in RVEC order.
* The cold start code prefixes each with a JMP opcode.
* The table stops after RVEC18, as Disk BASIC 1.1's does.  RVEC19
* through RVEC24 are left as found, except RVEC22, which cold start
* installs on its own: it is the CLS hook, and GET/PUT on a direct
* file comes in through it.
*******************************************************************
VECIMG          fdb     DVEC0           RVEC0  OPEN
                fdb     DVEC1           RVEC1  device number validity
                fdb     DVEC2           RVEC2  set print parameters
                fdb     DVEC3           RVEC3  console out
                fdb     DVEC4           RVEC4  console in
                fdb     DVEC5           RVEC5  INPUT device check
                fdb     DVEC6           RVEC6  PRINT device check
                fdb     DVEC7           RVEC7  close all files
                fdb     DVEC8           RVEC8  close one file
                fdb     XVEC9           RVEC9  PRINT (Extended BASIC's)
                fdb     DVEC10          RVEC10 INPUT#
                fdb     DVEC11          RVEC11 break check
                fdb     DVEC12          RVEC12 (RTS)
                fdb     DVEC13          RVEC13 line input termination
                fdb     DVEC14          RVEC14 EOF
                fdb     DVEC15          RVEC15 expression evaluation
                fdb     DVEC12          RVEC16 ON ERROR GOTO (RTS)
                fdb     DVEC17          RVEC17 error driver
                fdb     DVEC18          RVEC18 RUN "file"
VECEND

*******************************************************************
* CMDDICT - command dictionary.  High bit set on the last character
* of each word.  Order defines the token values ($CE upward).
*******************************************************************
CMDDICT         fcc     'DI'
                fcb     $80+'R          $CE DIR
                fcc     'DRIV'
                fcb     $80+'E          $CF DRIVE
                fcc     'FIEL'
                fcb     $80+'D          $D0 FIELD
                fcc     'FILE'
                fcb     $80+'S          $D1 FILES
                fcc     'KIL'
                fcb     $80+'L          $D2 KILL
                fcc     'LOA'
                fcb     $80+'D          $D3 LOAD
                fcc     'LSE'
                fcb     $80+'T          $D4 LSET
                fcc     'MERG'
                fcb     $80+'E          $D5 MERGE
                fcc     'RENAM'
                fcb     $80+'E          $D6 RENAME
                fcc     'RSE'
                fcb     $80+'T          $D7 RSET
                fcc     'SAV'
                fcb     $80+'E          $D8 SAVE
                fcc     'WRIT'
                fcb     $80+'E          $D9 WRITE
                fcc     'VERIF'
                fcb     $80+'Y          $DA VERIFY
                fcc     'UNLOA'
                fcb     $80+'D          $DB UNLOAD
                fcc     'DSKIN'
                fcb     $80+'I          $DC DSKINI
                fcc     'BACKU'
                fcb     $80+'P          $DD BACKUP
                fcc     'COP'
                fcb     $80+'Y          $DE COPY
                fcc     'DSKI'
                fcb     $80+'$          $DF DSKI$
                fcc     'DSKO'
                fcb     $80+'$          $E0 DSKO$
                fcc     'DO'
                fcb     $80+'S          $E1 DOS
                fcc     'BUBDIA'
                fcb     $80+'G          $E2 BUBDIAG

*******************************************************************
* CMDJUMP - command jump table, same order as CMDDICT
*******************************************************************
CMDJUMP         fdb     DIR             $CE
                fdb     DRIVE           $CF
                fdb     FIELD           $D0
                fdb     FILES           $D1
                fdb     KILL            $D2
                fdb     LOAD            $D3
                fdb     LSET            $D4
                fdb     MERGE           $D5
                fdb     RENAME          $D6
                fdb     RSET            $D7
                fdb     SAVE            $D8
                fdb     WRITE           $D9
                fdb     VERIFY          $DA
                fdb     UNLOAD          $DB
                fdb     DSKINI          $DC
                fdb     BACKUP          $DD
                fdb     COPY            $DE
                fdb     DSKI            $DF
                fdb     DSKO            $E0
                fdb     DOSCMD          $E1
                fdb     BUBDIAG         $E2

*******************************************************************
* SECDICT / SECJUMP - secondary (function) tokens, $A2 upward
*******************************************************************
SECDICT         fcc     'CV'
                fcb     $80+'N          $A2 CVN
                fcc     'FRE'
                fcb     $80+'E          $A3 FREE
                fcc     'LO'
                fcb     $80+'C          $A4 LOC
                fcc     'LO'
                fcb     $80+'F          $A5 LOF
                fcc     'MKN'
                fcb     $80+'$          $A6 MKN$
                fcc     'A'
                fcb     $80+'S          $A7 AS

SECJUMP         fdb     CVN             $A2
                fdb     FREE            $A3
                fdb     LOC             $A4
                fdb     LOF             $A5
                fdb     MKNS            $A6
                fdb     LB277           $A7 AS is only legal inside FIELD

*******************************************************************
* CMDHAND - command token dispatch.
*
* Entered by Color BASIC with the token in ACCA.  If it is one of
* ours, index the jump table; otherwise hand it to the next table
* in the chain (the user table) so that later extensions still work.
*******************************************************************
CMDHAND         cmpa    #DHITOK
                bhi     CMDHAND1        above this table's range - pass it on
                ldx     #CMDJUMP
                suba    #DLOTOK
                jmp     LADD4           Color BASIC's dispatcher
CMDHAND1        jmp     [CMDTBL_USR+3]  the user table's command handler

*******************************************************************
* SECHAND - secondary function dispatch.
*
* Entered with the modified token in ACCB (token-$80, doubled).
*******************************************************************
SECHAND         cmpb    #(DHISEC-$80)*2
                bls     SECHAND1
                jmp     [CMDTBL_USR+8]  the user table's secondary handler
SECHAND1        subb    #(DLOSEC-$80)*2
                pshs    b
                jsr     LB262           syntax check '(' and evaluate
                puls    b
                ldx     #SECJUMP
                jmp     LB2CE           Color BASIC's secondary dispatcher

*******************************************************************
* DXCVEC - interposer on Extended BASIC's command handler.
*
* PMODE must be told how much RAM sits below the graphics pages now
* that this ROM has claimed its own, and DLOAD must close files first.
* Everything else goes straight to Extended BASIC.
*******************************************************************
DXCVEC          cmpa    #$CA            DLOAD?
                beq     DXDLOAD
                cmpa    #$C8            PMODE?
                lbne    L813C           no - Extended BASIC's handler
                jsr     GETNCH
                cmpa    #',
                lbeq    L9650           PMODE ,n
                jsr     EVALEXPB
                cmpb    #4
                lbhi    LB44A           ?FC if PMODE > 4
                lda     GRPRAM          blocks below the graphics pages
                jmp     L962E

DXDLOAD         jsr     LA429           close files
                jsr     GETNCH
                jmp     L8C1B           Extended BASIC's DLOAD

*******************************************************************
* DXIVEC - interposer on Extended BASIC's secondary handler.
*
* POS(n) on a random file returns the PUT item counter.
*******************************************************************
DXIVEC          cmpb    #($9A-$80)*2    POS?
                lbne    L8168           no - Extended BASIC's handler
                jsr     LB262           syntax check '(' and evaluate
                lda     DEVNUM
                pshs    a
                jsr     LA5AE           evaluate the device number
                jsr     LA406           test it
                tst     DEVNUM
                ble     DXIPOS1         not a file - normal POS
                jsr     GETFCB          point X at the FCB
                ldb     FCBTYP,x
                cmpb    #RANFIL
                bne     DXIPOS1
                puls    a
                sta     DEVNUM
                ldd     FCBPUT,x
                jmp     GIVABF          return it as a floating point value
DXIPOS1         puls    a
                sta     DEVNUM
                jmp     LA35F           set print parameters (normal POS)
