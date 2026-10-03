; ===================================================================
; ui.asm - band model, main loop, key handling, screen drawing, seek.
;
; Tuning, volume, mute and mono are module commands.  SEEK IS A HOST
; LOOP: the TEF6686 has no seek command, so this steps the tuner and
; judges each channel from the module's reported quality.
; ===================================================================

; ---- band limits ----------------------------------------------------
; Indexed by BAND*2 + REGION, five bytes each: lo, hi, step.
; The US AM ceiling is 1620, not 1710: the module stops there.

BANDTAB         fdb     AM_US_LO,AM_US_HI
                fcb     AM_US_STEP
                fdb     AM_EU_LO,AM_EU_HI
                fcb     AM_EU_STEP
                fdb     FM_US_LO,FM_US_HI
                fcb     FM_US_STEP
                fdb     FM_EU_LO,FM_EU_HI
                fcb     FM_EU_STEP

; BANDLIM: X -> the entry for the current band and region.
BANDLIM         pshs    a,b
                lda     BAND
                lsla
                adda    REGION
                ldb     #5
                mul
                ldx     #BANDTAB
                leax    d,x
                puls    a,b,pc

; ---- MUL16X8: D = D * DIVSOR ---------------------------------------

MUL16X8         pshs    a,b                     ; ,S = hi, 1,S = lo
                lda     1,s
                ldb     DIVSOR
                mul                             ; D = lo * m
                pshs    a,b                     ; 2,S = hi, 3,S = lo
                lda     2,s
                ldb     DIVSOR
                mul                             ; D = hi * m
                tfr     b,a
                clrb                            ; D = (hi * m) << 8
                addd    ,s++                    ; + lo * m
                leas    2,s
                rts

; ---- SNAPV: D = bare frequency -> clamped and snapped to the grid ---

SNAPV           pshs    x
                jsr     BANDLIM
                cmpd    ,x
                bhs     SNAPV1
                ldd     ,x                      ; below the band: clamp up
                bra     SNAPVX
SNAPV1          cmpd    2,x
                bls     SNAPV2
                ldd     2,x                     ; above the band: clamp down
                bra     SNAPVX
SNAPV2          subd    ,x                      ; offset from the bottom
                pshs    a,b
                ldb     4,x
                stb     DIVSOR
                lsrb
                clra                            ; D = step / 2, for rounding
                addd    ,s++
                jsr     DIV16X8                 ; D = grid index
                jsr     MUL16X8                 ; D = index * step
                addd    ,x
SNAPVX          puls    x,pc

; ---- SNAPSTA: D = station word -> snapped, band bit preserved -------

SNAPSTA         anda    #$7F
                jsr     SNAPV
                tst     BAND
                beq     SNAPSDN
                ora     #$80
SNAPSDN         rts

; ---- STEPSTA: A = +1 or -1.  Step STATION one grid unit, wrapping. --

STEPSTA         pshs    a,x                     ; ,S = dir
                jsr     BANDLIM
                clra
                ldb     4,x
                pshs    a,b                     ; ,S = step, 2,S = dir
                ldd     STATION
                anda    #$7F
                tst     2,s
                bmi     STEPSDW
                addd    ,s
                cmpd    2,x
                bls     STEPSOK
                ldd     ,x                      ; off the top: wrap to the bottom
                bra     STEPSOK
STEPSDW         subd    ,s
                bcs     STEPSWH
                cmpd    ,x
                bhs     STEPSOK
STEPSWH         ldd     2,x                     ; off the bottom: wrap to the top
STEPSOK         leas    2,s
                tst     BAND
                beq     STEPSST
                ora     #$80
STEPSST         std     STATION
                puls    a,x,pc

; ---- RETUNE: push STATION to the module and redraw -----------------

RETUNE          lda     #TEFT_PRESET
                jsr     TEFTUNE
                jsr     DRAWFREQ
                ldd     TICKS
                std     SETTLE
                lda     #1
                sta     DIRTYHDR
                rts

; ===================================================================
; main loop
; ===================================================================

UIMAIN          jsr     VSYNC
                ldx     TICKS
                leax    1,x
                stx     TICKS
                jsr     KBDSCAN
                lda     UIMODE
                cmpa    #UI_MAIN
                bne     UIM1
                jsr     MAINKEYS
                bra     UIMPER
UIM1            cmpa    #UI_NAME
                bne     UIM2
                jsr     NAMEKEYS
                bra     UIMPER
UIM2            cmpa    #UI_FREQ
                bne     UIM3
                jsr     FREQKEYS
                bra     UIMPER
UIM3            cmpa    #UI_HELP
                bne     UIM4
                jsr     HELPKEYS
                bra     UIMPER
UIM4            cmpa    #UI_ABOUT
                bne     UIM5
                jsr     ABOUTKEYS
                bra     UIMPER
UIM5            jsr     TESTKEYS
UIMPER          jsr     INDICATORS
                jsr     SETTLEDSAVE
                jsr     MSGEXPIRE
                rts

; ---- INDICATORS: refresh level and stereo four times a second ------
; Reading the module every tick would keep the bus busy for a large part
; of every frame for no benefit.

INDICATORS      lda     UIMODE
                cmpa    #UI_MAIN
                bne     INDX
                lda     TICKS+1
                ldb     #LEVEL_EVERY
                stb     DIVSOR
                clra
                ldb     TICKS+1
                jsr     DIV16X8
                lda     DIVTMP+2
                bne     INDX                    ; not our tick
                jsr     TEFQUAL
                jsr     TEFSTEREO
                jsr     DRAWINDIC
INDX            rts

; ---- SETTLEDSAVE: write the header page after five quiet seconds ----
; This bounds erase cycles to one per station actually listened to.

SETTLEDSAVE     tst     DIRTYHDR
                beq     SSX
                ldd     TICKS
                subd    SETTLE
                cmpd    #SETTLE_TICKS
                blo     SSX
                jsr     STSAVEHDR
                tst     EEFAIL
                beq     SSX
; A failed save leaves DIRTYHDR set so it is tried again, but not on the
; very next tick: that would hammer the chip sixty times a second for as
; long as the fault lasts and freeze the UI in the save routine.  Restart
; the five-second timer, so a persistent fault costs one attempt and one
; message every five seconds.
                ldd     TICKS
                std     SETTLE
                ldy     #MSGSAVEFAIL
                jsr     MSG
SSX             rts

; Bottom of the main screen.  The volume line is the last row and never
; changes position; messages and the NAME: / FREQ? prompts use the row
; above it, which is otherwise left EMPTY so the preset list has a blank
; row under it.  The help hint is on the boot screens (DRAWHINT).
MSGROW          equ     14
VOLROW          equ     15

; ---- MSG / MSGEXPIRE: the message line, row MSGROW -----------------

MSG             pshs    a,b,x
                lda     #MSGROW
                jsr     CLRROW
                lda     #MSGROW
                ldb     #0
                jsr     PUTSAT
                ldd     TICKS
                addd    #MSG_SECS
                std     MSGTICK
                puls    a,b,x,pc

MSGEXPIRE       lda     UIMODE                  ; only the main screen owns
                bne     MEX                     ; the row; in an editor the
                ldd     MSGTICK                 ; row is the prompt and must
                beq     MEX                     ; not be swept away after 3 s
                subd    TICKS
                bhi     MEX
                lda     #MSGROW
                jsr     CLRROW
                ldd     #0
                std     MSGTICK
MEX             rts

; ===================================================================
; main-mode keys
; ===================================================================

MAINKEYS        lda     KEYEVT
                lbeq    MKX
                cmpa    #K_LEFT
                bne     MK1
                tst     SHIFTFLG
                bne     MKSEEKD
                lda     #-1
                jsr     STEPSTA
                jsr     RETUNE
                rts
MKSEEKD         lda     #-1
                jmp     SEEK
MK1             cmpa    #K_RIGHT
                bne     MK2
                tst     SHIFTFLG
                bne     MKSEEKU
                lda     #1
                jsr     STEPSTA
                jsr     RETUNE
                rts
MKSEEKU         lda     #1
                jmp     SEEK
MK2             cmpa    #K_UP
                bne     MK3
                lda     VOLUME
                cmpa    #99
                lbhs    MKX
                inca
                sta     VOLUME
                jsr     TEFVOL
                jsr     DRAWVOL
                lbra    MKDIRTY
MK3             cmpa    #K_DOWN
                bne     MK4
                lda     VOLUME
                lbeq    MKX
                deca
                sta     VOLUME
                jsr     TEFVOL
                jsr     DRAWVOL
                lbra    MKDIRTY
MK4             cmpa    #'M'
                bne     MK5
                lda     MUTEFLG
                eora    #1
                sta     MUTEFLG
                jsr     TEFMUTE
                jsr     DRAWINDIC
                lbra    MKDIRTY
MK5             cmpa    #'S'
                bne     MK6
                lda     MONOFLG
                eora    #1
                sta     MONOFLG
                jsr     TEFMONO
                jsr     DRAWINDIC
                lbra    MKDIRTY
MK6             cmpa    #'B'
                bne     MK7
                ldd     STATION                 ; swap bands
                ldx     ALTSTATION
                std     ALTSTATION
                stx     STATION
                jsr     STSETBAND
                ldd     STATION
                jsr     SNAPSTA
                std     STATION
                jsr     RETUNE
                jsr     DRAWALL
                rts
MK7             cmpa    #'R'
                bne     MK8
                lda     REGION
                eora    #1
                sta     REGION
                ldd     STATION                 ; re-snap to the new grid
                jsr     SNAPSTA
                std     STATION
; ALTSTATION is on the OTHER band by definition, and SNAPSTA snaps to the
; current one, so flip BAND around it or the alternate station is dragged
; onto this band.
                lda     BAND
                eora    #1
                sta     BAND
                ldd     ALTSTATION
                jsr     SNAPSTA
                std     ALTSTATION
                lda     BAND
                eora    #1
                sta     BAND
                jsr     RETUNE
                jsr     TEFDEEMPH               ; the de-emphasis standard
                jsr     DRAWALL                 ; follows the region
                rts
MK8             cmpa    #'F'
                bne     MK9
                lda     #UI_FREQ
                sta     UIMODE
                clr     EDITPOS
                jmp     DRAWEDIT                ; draws the prompt without arming
MK9             cmpa    #'H'                    ; the 3 s message timer
                bne     MK10
                lda     #UI_HELP
                sta     UIMODE
                jmp     HELPDRAW
MK10            cmpa    #'A'
                bne     MK10T
                lda     #UI_ABOUT
                sta     UIMODE
                jmp     ABOUTDRAW
MK10T           cmpa    #'T'
                bne     MK11
                lda     #UI_TEST
                sta     UIMODE
                jmp     TESTDRAW
MK11            cmpa    #'W'
                bne     MK12
                lda     #'W'
                sta     PENDING
                ldy     #MSGSTORE
                jmp     MSG
MK12            cmpa    #'N'
                bne     MK13
                lda     #'N'
                sta     PENDING
                ldy     #MSGNAME
                jmp     MSG
MK13            cmpa    #K_BREAK
                bne     MK14
                jmp     EXITBAS
MK14            cmpa    #K_CLEAR
                bne     MK15
                clr     PENDING
                rts
MK15            cmpa    #'0'                    ; a preset digit?
                lblo    MKCLRP
                cmpa    #'9'
                lbhi    MKCLRP
                jmp     PRESETKEY
MKCLRP          clr     PENDING
MKX             rts
MKDIRTY         ldd     TICKS
                std     SETTLE
                lda     #1
                sta     DIRTYHDR
                rts

; ---- PRESETKEY: A = '0'..'9'.  Recall, store or name. --------------
; '0' is preset 10; SHIFT+1 and SHIFT+2 reach 11 and 12.

PRESETKEY       suba    #'0'
                bne     PK1
                lda     #10
                bra     PK2
PK1             tst     SHIFTFLG
                beq     PK2
                cmpa    #3
                bhs     PK2
                adda    #10                     ; SHIFT+1 = 11, SHIFT+2 = 12
PK2             pshs    a
                lda     PENDING
                cmpa    #'W'
                beq     PKSTORE
                cmpa    #'N'
                beq     PKNAME
                puls    a                       ; plain recall
                pshs    a
                jsr     STGETSTA
                cmpd    #0
                bne     PKRECALL
                puls    a
                clr     PENDING
                ldy     #MSGEMPTY
                jmp     MSG
; Set BAND from the preset's own band bit BEFORE snapping.  SNAPSTA clamps
; to the current band's range and re-applies the current band's bit, so
; snapping first would turn an AM preset recalled from FM into 87.9 MHz.
PKRECALL        std     STATION
                jsr     STSETBAND
                ldd     STATION
                jsr     SNAPSTA
                std     STATION
                leas    1,s
                clr     PENDING
                jsr     RETUNE
                jsr     DRAWALL
                rts
PKSTORE         puls    a
                clr     PENDING
                jsr     STPUTSTA
                jsr     DRAWPRESETS
                tst     EEFAIL
                beq     PKSX
                ldy     #MSGSAVEFAIL
                jmp     MSG
PKSX            rts
PKNAME          puls    a
                sta     EDITSLOT
                clr     PENDING
                lda     #UI_NAME
                sta     UIMODE
                lda     EDITSLOT                ; current name into the buffer
                jsr     STRECADDR
                leax    STR_NAME,x
                ldy     #EDITBUF
                ldb     #ST_NAMELEN
PKNCP           lda     ,x+
                sta     ,y+
                decb
                bne     PKNCP
                clr     EDITBUF+ST_NAMELEN
                clr     EDITPOS
                jmp     DRAWEDIT                ; prompt plus the current name,
                                                ; and no expiry timer
; ===================================================================
; seek - a HOST loop.  The module has no seek command.
; ===================================================================
; Tune with Search, which stays muted, step by step until the quality
; passes or the seek arrives back where it started.  End releases the
; mute.  Any key aborts.  A channel passes on enough level, and for FM
; also low ultrasonic noise and a small tuning offset.

SEEK            sta     SEEKDIR
                ldd     STATION
                std     SEEKSTART
                lda     #1
                sta     SEEKFLG
                ldy     #MSGSEEK
                jsr     MSG
SEEKLP          lda     SEEKDIR
                jsr     STEPSTA
                lda     #TEFT_SEARCH            ; tunes but stays muted
                jsr     TEFTUNE
                jsr     DRAWFREQ
                jsr     TEFWAITQ
                bne     SEEKNEXT                ; never settled: keep going
                lda     LEVEL
                cmpa    #TUNED_THRESH
                blt     SEEKNEXT
                tst     BAND
                beq     SEEKHIT                 ; AM: level alone is enough
                lda     QUSN
                cmpa    #SEEK_USN_MAX
                bhi     SEEKNEXT
                ldd     QOFFSET
                bpl     SEEKOFFP
                nega                            ; absolute value
                negb
                sbca    #0
SEEKOFFP        cmpd    #SEEK_OFF_MAX
                bhi     SEEKNEXT
SEEKHIT         lda     #TEFT_END               ; release the Search mute
                jsr     TEFTUNE
                clr     SEEKFLG
                lda     #MSGROW
                jsr     CLRROW
                jsr     DRAWALL
                ldd     TICKS
                std     SETTLE
                lda     #1
                sta     DIRTYHDR
                rts
; Any NEW key aborts.  The scanner is called once per channel step, and
; the shift+arrow that started the seek is still held; left alone, its
; hold count would reach the auto-repeat threshold, fire an event and
; abort the seek on a noisy channel.  Clearing the hold count each step
; stops a held key repeating; a fresh press still aborts.
SEEKNEXT        jsr     KBDSCAN
                clr     KEYHOLD
                lda     KEYEVT
                bne     SEEKABRT
                ldd     STATION
                cmpd    SEEKSTART
                bne     SEEKLP
                ldd     SEEKSTART               ; all the way round
                std     STATION
                jsr     STSETBAND
                lda     #TEFT_PRESET
                jsr     TEFTUNE
                clr     SEEKFLG
                jsr     DRAWALL
                ldy     #MSGNOSTA
                jmp     MSG
SEEKABRT        lda     #TEFT_END
                jsr     TEFTUNE
                clr     SEEKFLG
                jsr     DRAWALL
                lda     #MSGROW
                jsr     CLRROW
                ldd     TICKS                   ; the station DID change, so
                std     SETTLE                  ; it must be saved like any
                lda     #1                      ; other tune, as SEEKHIT does
                sta     DIRTYHDR
                rts

; ===================================================================
; name entry
; ===================================================================

NAMEKEYS        lda     KEYEVT
                lbeq    NKX
                cmpa    #K_BREAK
                bne     NK1
                clr     UIMODE
                jsr     DRAWALL
                rts
NK1             cmpa    #K_ENTER
                bne     NK2
                lda     EDITSLOT                ; commit
                jsr     STRECADDR
                leax    STR_NAME,x
                ldy     #EDITBUF
                ldb     #ST_NAMELEN
NKCP            lda     ,y+
                sta     ,x+
                decb
                bne     NKCP
                lda     EDITSLOT
                jsr     STSAVEREC
                clr     UIMODE
                jsr     DRAWALL
                tst     EEFAIL
                lbeq    NKX
                ldy     #MSGSAVEFAIL
                jmp     MSG
NK2             cmpa    #K_CLEAR
                bne     NK3
                ldx     #EDITBUF                ; blank the whole name
                ldb     #ST_NAMELEN
                lda     #' '
NKBL            sta     ,x+
                decb
                bne     NKBL
                clr     EDITPOS
                bra     NKDRAW
NK3             cmpa    #K_LEFT
                bne     NK4
                lda     EDITPOS
                beq     NKDRAW
                deca
                sta     EDITPOS
                ldx     #EDITBUF
                ldb     EDITPOS
                abx
                lda     #' '
                sta     ,x                      ; backspace also blanks
                bra     NKDRAW
NK4             cmpa    #K_RIGHT
                bne     NK5
                lda     EDITPOS
                cmpa    #ST_NAMELEN-1
                bhs     NKDRAW
                inca
                sta     EDITPOS
                bra     NKDRAW
NK5             cmpa    #K_SPACE                ; printable?
                lblo    NKX
                cmpa    #'Z'
                lbhi    NKX
                ldx     #EDITBUF
                ldb     EDITPOS
                abx
                sta     ,x
                lda     EDITPOS
                cmpa    #ST_NAMELEN-1
                bhs     NKDRAW
                inca
                sta     EDITPOS
NKDRAW          jsr     DRAWEDIT
NKX             rts

; ===================================================================
; direct frequency entry
; ===================================================================

FREQKEYS        lda     KEYEVT
                lbeq    FKX
                cmpa    #K_BREAK
                bne     FK1
                clr     UIMODE
                lda     #MSGROW
                jsr     CLRROW
                jsr     DRAWALL
                rts
FK1             cmpa    #K_ENTER
                bne     FK2
                jmp     FREQCOMMIT
FK2             cmpa    #K_CLEAR
                bne     FK3
                clr     EDITPOS
                bra     FKDRAW
FK3             cmpa    #'0'
                lblo    FKX
                cmpa    #'9'
                lbhi    FKX
                ldb     EDITPOS
                cmpb    #5
                lbhs    FKX
                ldx     #EDITBUF
                abx
                sta     ,x
                inc     EDITPOS
FKDRAW          jsr     DRAWEDIT
FKX             rts

; FREQCOMMIT: parse the digits as tenths of a MHz (FM) or kHz (AM).
FREQCOMMIT      lda     EDITPOS
                beq     FCBAD
; Clear the accumulator BEFORE loading the digit count into B: LDD #0
; writes both halves of D, and B is the low half.
                ldd     #0
                std     DIVTMP
                ldx     #EDITBUF
                ldb     EDITPOS
FCLP            pshs    b
                ldd     DIVTMP
                pshs    a,b
                aslb
                rola                            ; x2
                aslb
                rola                            ; x4
                addd    ,s++                    ; x5
                aslb
                rola                            ; x10
                std     DIVTMP
                lda     ,x+
                suba    #'0'
                tfr     a,b
                clra
                addd    DIVTMP
                std     DIVTMP
                puls    b
                decb
                bne     FCLP
                ldd     DIVTMP
                tst     BAND
                beq     FCAM
; FM: the user typed tenths of a MHz, the station word is 50 kHz units,
; so double it.  879 -> 1758.
                aslb
                rola
FCAM            jsr     SNAPV
                tst     BAND
                beq     FCSET
                ora     #$80
FCSET           std     STATION
                jsr     STSETBAND
                clr     UIMODE
                clr     EDITPOS
                lda     #MSGROW
                jsr     CLRROW
                jsr     RETUNE
                jsr     DRAWALL
                rts
FCBAD           ldy     #MSGRANGE
                jmp     MSG

; ===================================================================
; help
; ===================================================================

HELPKEYS        lda     KEYEVT
                lbeq    HKX
                clr     UIMODE
                jsr     DRAWALL
HKX             rts

HELPDRAW        jsr     CLS
                ldy     #HELPTXT
                lda     #1
                ldb     #1
HELPLP          pshs    a,b
                jsr     PUTSAT                  ; PUTS post-increments Y past
                puls    a,b                     ; the terminator, so Y already
                                                ; points at the next string.  A
                                                ; "skip to next" loop here ate
                                                ; every other line of help.
                inca
                cmpa    #15
                bhs     HELPDN
                tst     ,y                      ; empty string ends the list
                bne     HELPLP
HELPDN          rts

; ===================================================================
; hardware test screen
; ===================================================================

TESTDRAW        jsr     CLS
                ldy     #TESTTITLE
                lda     #0
                ldb     #4
                jsr     PUTSATI
                jsr     DRAWBUILD
                if      DEVMPI
                ldy     #MSGDEVMPI
                lda     #8
                ldb     #1
                jsr     PUTSAT
                lda     #8
                ldb     #25
                jsr     SCRADDR
                lda     #MPI_SCS_SLOT+$30       ; not +'0': see equates.inc on
                jsr     VDGCHR                  ; character-literal concatenation
                sta     ,x
                endif
                rts

TESTKEYS        jsr     TESTWATCH
                jsr     TESTREFRESH
                lda     KEYEVT
                lbeq    TKX
                cmpa    #K_BREAK
                bne     TK1
                clr     UIMODE
                jsr     DRAWALL
                rts
TK1             cmpa    #'0'
                bne     TK2
                lda     CTLSHD                  ; toggle SCL, debug header pin 2
                eora    #CTL_SCL
                sta     CTLSHD
                sta     CTLREG
                rts
TK2             cmpa    #'1'
                bne     TK3
                lda     CTLSHD                  ; toggle SDA, debug header pin 3
                eora    #CTL_SDA
                sta     CTLSHD
                sta     CTLREG
                rts
TK3             cmpa    #'I'
                bne     TK4
                jsr     TEFPROBE
                jsr     TEFIDENT
                rts
TK4             cmpa    #'P'
                bne     TK5
                jsr     TEFINIT
                rts
TK5             cmpa    #'L'
                bne     TK6
                jsr     TEFQUAL
                jsr     TEFSTEREO
                rts
TK6             cmpa    #'E'
                bne     TK7
                jmp     TESTEE
TK7             cmpa    #'A'
                lbne    TK8
                jmp     TESTSCRATCH
TK8             cmpa    #'D'
                bne     TK9
                jmp     TESTDIGITS
TK9             cmpa    #'W'
                bne     TKX
                lda     WATCHFLG                ; toggle the gain watch
                eora    #1
                sta     WATCHFLG
                clr     WATCHN
                lda     #11
                ldb     #1
                ldy     #TWL1
                jsr     PUTSAT
                lda     #12
                ldb     #1
                ldy     #TWL2
                jsr     PUTSAT
                lda     #13
                ldb     #1
                ldy     #TWL3
                jsr     PUTSAT
                lda     #14
                ldb     #1
                ldy     #TWL4
                jsr     PUTSAT
TKX             rts

; TESTWATCH: W on the test screen.  Four times a second, read every word
; the chip exposes about its gain state and print them raw, in hex, on
; rows 11 to 14, to show whether anything inside the chip moves in step
; with an audio problem.
;
;   row 11  LV level 0.1 dBuV    US usn 0.1 %     WM wam 0.1 %
;   row 12  AGC IN input_att 0.1 dB (steps of 60)   FB feedback_att
;   row 13  SM softmute 0.1 % of max attenuation   HC highcut   ST stereo
;   row 14  HB stereo high blend    N pass count    S stereo pilot
;
; Softmute (SM) is the only control that reduces audio level.  Level,
; AGC and SM all steady while the audio misbehaves means the fault is
; downstream of the chip.  SM swinging toward 03E8 means the chip is
; muting on the level or noise it sees, and LV / US show which.

TESTWATCH       tst     WATCHFLG
                lbeq    TWX
                lda     #LEVEL_EVERY            ; the same cadence as the
                sta     DIVSOR                  ; main screen's INDICATORS
                clra
                ldb     TICKS+1
                jsr     DIV16X8
                lda     DIVTMP+2
                lbne    TWX
                jsr     TEFQUAL
                ldd     I2CBUF+QW_LEVEL         ; keep the raw words before
                std     WATCHQ                  ; the next read reuses I2CBUF
                ldd     I2CBUF+QW_USN
                std     WATCHQ+2
                ldd     I2CBUF+QW_WAM
                std     WATCHQ+4
                jsr     TEFSTEREO
                jsr     TEFAGC
                jsr     TEFPROC
                inc     WATCHN
                lda     #11
                ldb     #4
                ldy     #WATCHQ
                jsr     PUTHEXW
                ldb     #12
                leay    2,y
                jsr     PUTHEXW
                ldb     #20
                leay    2,y
                jsr     PUTHEXW
                lda     #12
                ldb     #8
                ldy     #AGCW
                jsr     PUTHEXW
                ldb     #16
                leay    2,y
                jsr     PUTHEXW
                lda     #13
                ldb     #4
                ldy     #PROCW
                jsr     PUTHEXW
                ldb     #12
                leay    2,y
                jsr     PUTHEXW
                ldb     #20
                leay    2,y
                jsr     PUTHEXW
                lda     #14
                ldb     #4
                leay    2,y
                jsr     PUTHEXW
                lda     #14
                ldb     #11
                jsr     SCRADDR
                lda     WATCHN
                jsr     PUTHEX
                lda     #14
                ldb     #17
                jsr     SCRADDR
                lda     STEREOFLG
                jsr     PUTHEX
TWX             rts

; PUTHEXW: A = row, B = column, Y = a big-endian word.  Four hex digits.
; A, B and Y are preserved so a caller can step along a row of words.

PUTHEXW         pshs    a,b,x,y
                jsr     SCRADDR
                lda     ,y+
                jsr     PUTHEX
                lda     ,y
                jsr     PUTHEX
                puls    a,b,x,y,pc

; TESTDIGITS: draw 0 1 2 3 4 across the big-digit rows, straight from
; the font, bypassing BCD16 and the station word entirely.  If these
; appear, the renderer is fine and the frequency display is being handed
; nothing to draw.  If they do not, the renderer is the fault.

TESTDIGITS      clra
                ldb     #6
TDLP            jsr     BIGDIG                  ; preserves A and B
                inca
                addb    #4
                cmpa    #5
                blo     TDLP
                rts

; TESTEE: rewrite the header page with its own contents.  Exercises the
; real $DF00 path, so /EEWR and /SLENB on the debug header must both
; pulse.  A rewrite of unchanged data is the hardest case for the poll.
;
; In a Multi-Pak development build this path is RAM-only and proves
; nothing, because the read-back would come from the multicart.  Use A
; instead: the $FF40 window is decoded through SCS, so both halves of
; that test reach the radio cartridge.
TESTEE
                if      DEVMPI
                ldy     #MSGDEVEE
                jmp     MSG
                endif
                jsr     STSAVEHDR
                rts

; TESTSCRATCH: write a pattern to $FF40-$FF5D and read it back.
; That window is chip $3F40-$3F5D, thirty bytes nothing else can reach,
; so this cannot touch a preset.  It needs no SLENB*, so /EEWR pulses
; while /SLENB stays high.  The pattern alternates between runs so a
; stale pass cannot look like a fresh one.
TESTSCRATCH     ldx     #STORE_RAM+$C0          ; scratch area in the RAM copy
                lda     TMP
                coma
                sta     TMP                     ; alternate the pattern
                ldb     #SCRATCH_LEN
                lda     TMP
TSCFILL         sta     ,x+
                inca
                decb
                bne     TSCFILL
                ldx     #STORE_RAM+$C0
                ldy     #SCRATCH_WIN
                ldb     #SCRATCH_LEN
                jsr     EESAVE
                rts

TESTREFRESH     lda     #2
                ldb     #1
                jsr     SCRADDR
                lda     STATREG
                jsr     PUTHEX
                lda     #3
                ldb     #1
                jsr     SCRADDR
                lda     CTLSHD
                jsr     PUTHEX
                lda     #4
                ldb     #1
                jsr     SCRADDR
                lda     I2CERR
                jsr     PUTHEX
                lda     #5
                ldb     #1
                jsr     SCRADDR
                lda     LEVEL
                jsr     PUTHEX
                lda     #6
                ldb     #1
                jsr     SCRADDR
                lda     EEFAIL
                jsr     PUTHEX
                lda     #7
                ldb     #1
                jsr     SCRADDR
                lda     TEFFAIL
                jsr     PUTHEX
                lda     PATCHOK
                jsr     PUTHEX
; STATION, BAND and VOLUME, so an empty display can be told apart from
; empty data.  A station word of 0000 means the store never reached the
; variables; 879A is the compiled-in default of FM 97.3.
                lda     #10
                ldb     #1
                jsr     SCRADDR
                lda     STATION
                jsr     PUTHEX
                lda     STATION+1
                jsr     PUTHEX
                lda     #10
                ldb     #7
                jsr     SCRADDR
                lda     BAND
                jsr     PUTHEX
                lda     VOLUME
                jsr     PUTHEX
                lda     #10
                ldb     #13
                jsr     SCRADDR
                lda     NUMBUF+1                ; what BCD16 last produced
                jsr     PUTHEX
                lda     NUMBUF+2
                jsr     PUTHEX
                lda     NUMBUF+3
                jsr     PUTHEX
                lda     NUMBUF+4
                jsr     PUTHEX
                rts

; ===================================================================
; drawing
; ===================================================================

DRAWALL         jsr     CLS
                ldy     #TITLE
                lda     #0
                ldb     #6
                jsr     PUTSATI
                jsr     DRAWFREQ
                jsr     DRAWINDIC
                jsr     DRAWPRESETS
                jsr     DRAWVOL
                jsr     DRAWBUILD
                rts

; ---- DRAWBUILD: the build stamp, top right ------------------------
; Four hex digits of whatever the Makefile stamped in.  If this does not
; change after a rebuild, the machine is running a stale image.

DRAWBUILD       pshs    a,b,x
                lda     #0
                ldb     #27
                jsr     SCRADDR
                lda     #BUILDID>>8
                jsr     PUTHEX
                lda     #BUILDID&$FF
                jsr     PUTHEX
                puls    a,b,x,pc

; ---- DRAWFREQ: the big digits and the unit -------------------------
; FM shows tenths of a MHz, which is the station word halved, with a
; decimal point.  AM shows kHz.

DRAWFREQ        pshs    a,b,x,y
                ldd     STATION
                anda    #$7F
                tst     BAND
                beq     DFAM
                lsra                            ; 50 kHz units -> tenths of MHz
                rorb
DFAM            jsr     BCD16                   ; NUMBUF = five right-aligned
; Columns 0-6 and 28-31 belong to the indicators, so the digits have
; columns 7-27.  Each digit is three cells wide.
;   FM  9, 13, 17, point 21, 23   -> 9..25, one blank column each side
;                                    of the point, two each side of the block
;   AM  10, 14, 18, 22            -> 10..24, centred, three each side
; The point must have a blank column on BOTH sides, or it merges with
; the last digit into one bar on the bottom row.  The unit sits in the right-hand indicator
; column, not against the digits, on row 6 so it is level with their
; bottom row; MUTE and MONO are above it on rows 2 and 4.  Rows 1 and 7
; stay blank over the digits.
                tst     BAND
                beq     DFAMPOS
                ldb     #9                      ; FM
                lda     NUMBUF+1
                jsr     DFDIG
                ldb     #13
                lda     NUMBUF+2
                jsr     DFDIG
                ldb     #17
                lda     NUMBUF+3
                jsr     DFDIG
                ldb     #23
                lda     NUMBUF+4
                jsr     DFDIG
                ldb     #21
                jsr     BIGDOT
                ldy     #UNITMHZ
                bra     DFUNIT
; AM.  The point's cell is a gap column in the AM layout, so clearing it
; cannot disturb a digit; the band switch redraws the whole screen anyway.
DFAMPOS         ldb     #21
                jsr     BIGDOTCLR
                ldb     #10
                lda     NUMBUF+1
                jsr     DFDIG
                ldb     #14
                lda     NUMBUF+2
                jsr     DFDIG
                ldb     #18
                lda     NUMBUF+3
                jsr     DFDIG
                ldb     #22
                lda     NUMBUF+4
                jsr     DFDIG
                ldy     #UNITKHZ
DFUNIT          lda     #BIGROW+4               ; level with the digits' bottom row
                ldb     #28
                jsr     PUTSAT
                puls    a,b,x,y,pc

; DFDIG: A = ASCII digit or space, B = column.
DFDIG           cmpa    #' '
                bne     DFDIG1
                lda     #$FF                    ; blank cell
                jmp     BIGDIG
DFDIG1          suba    #'0'
                jmp     BIGDIG

; ---- DRAWINDIC: band, stereo, tuned, mute, mono --------------------
; Every field is fixed width and written in place whether it is on or
; off; clearing and redrawing would make the indicators blink at the
; four-times-a-second refresh.
;
; Layout: two columns level with the digit block, rows 2, 4 and 6 with
; a blank row between each.  Left: band, STEREO, TUNED.  Right: MUTE,
; MONO and the unit (drawn by DRAWFREQ), so the band is level with the
; top of the digits and the unit with their bottom.  Rows 1 and 7 are
; left blank above and below the frequency.

DRAWINDIC       pshs    a,b,x,y
                ldy     #TXTAM
                tst     BAND
                beq     DIBAND
                ldy     #TXTFM
DIBAND          lda     #2
                ldb     #1
                jsr     PUTSATI                 ; the band is always shown

                lda     STEREOFLG
                sta     TMP
                ldy     #TXTSTEREO
                ldx     #TXTBLANK6
                lda     #4
                ldb     #1
                jsr     INDFLD

                clr     TMP                     ; LEVEL is signed dBuV
                lda     LEVEL
                cmpa    #TUNED_THRESH
                blt     DITU
                inc     TMP
DITU            ldy     #TXTTUNED
                ldx     #TXTBLANK5
                lda     #6
                ldb     #1
                jsr     INDFLD

                lda     MUTEFLG
                sta     TMP
                ldy     #TXTMUTE
                ldx     #TXTBLANK4
                lda     #2
                ldb     #28
                jsr     INDFLD

                lda     MONOFLG
                sta     TMP
                ldy     #TXTMONO
                ldx     #TXTBLANK4
                lda     #4
                ldb     #28
                jsr     INDFLD
                puls    a,b,x,y,pc

; INDFLD: A = row, B = column, Y = the label, X = blanks of the same
;         width, TMP nonzero to show the label.  The label goes up in
;         inverse video and the blanks in normal video, because an
;         inverse space is a solid block rather than a gap.

INDFLD          tst     TMP
                beq     INDFOFF
                jmp     PUTSATI
INDFOFF         tfr     x,y
                jmp     PUTSAT

; ---- DRAWPRESETS: two columns of six -------------------------------

DRAWPRESETS     pshs    a,b,x,y
                lda     #1
DPLP            pshs    a
                jsr     DPONE
                puls    a
                inca
                cmpa    #ST_NPRESET+1
                blo     DPLP
                puls    a,b,x,y,pc

; DPONE: A = preset 1..12.  Draws "n name freq" in its field.  The
; screen position is recomputed from DPROW / DPCOL for each part rather
; than carried on the stack.

DPONE           sta     DPNUM
                deca
                cmpa    #6
                blo     DPLEFT
                suba    #6
                ldb     #17                     ; right-hand half, 17-31
                bra     DPPOS
DPLEFT          clrb
DPPOS           adda    #8                      ; rows 8..13
                sta     DPROW
                stb     DPCOL

; The slot label is the key that recalls it: 1-9, 0 for ten, and S1 / S2
; (SHIFT 1, SHIFT 2) for eleven and twelve.  Labels are right-aligned on
; the half's first column, so the "S" of S1 and S2 takes the column just
; left of it.  That is why the right half starts at 17, not 16: column 16
; is free for the "S", and column 15 stays the left half's gutter.
                lda     DPROW
                ldb     DPCOL
                jsr     SCRADDR
                lda     DPNUM
                cmpa    #10
                blo     DPL1
                beq     DPL0
                suba    #10                     ; 11 -> 1, 12 -> 2
                pshs    a
                lda     #'S'
                jsr     VDGCHR
                sta     -1,x                    ; the S, one column left
                puls    a
                adda    #'0'
                bra     DPLPUT
DPL0            lda     #'0'
                bra     DPLPUT
DPL1            adda    #'0'
DPLPUT          jsr     VDGCHR
                sta     ,x+
                lda     #VDG_SPACE
                sta     ,x+

; the name, straight out of the RAM copy of the store
                lda     DPNUM
                jsr     STRECADDR
                leax    STR_NAME,x
                tfr     x,y
                lda     DPROW
                ldb     DPCOL
                addb    #2
                jsr     SCRADDR
                ldb     #ST_NAMELEN
                jsr     PUTN

; the frequency, or five blanks when the slot is empty
                lda     DPNUM
                jsr     STGETSTA
                cmpd    #0
                beq     DPEMPTY
; Five characters at column 10 of the half.  The left half then writes
; a blank gutter column, so the right half's label never touches it.
;   FM  " 87.9" .. "108.0"   tenths of a MHz with the decimal point
;   AM  "  540" .. " 1620"   kHz, right aligned, no point
; The point is what tells the bands apart; there is no room for a unit.
                pshs    a                       ; keep the band bit
                anda    #$7F
                tst     ,s
                bpl     DPFAM                   ; AM is already in kHz
                lsra                            ; FM: 50 kHz units to tenths
                rorb
DPFAM           jsr     BCD16                   ; NUMBUF = 5 right-aligned
                tst     ,s+                     ; FM?  (drops the band byte)
                bpl     DPFNUM
; FM: "  973" -> " 97.3", " 1080" -> "108.0".  Shift the top four digits
; left one place and put the point in the fourth, in place, front first.
                ldx     #NUMBUF
                lda     1,x
                sta     ,x
                lda     2,x
                sta     1,x
                lda     3,x
                sta     2,x
                lda     #'.'
                sta     3,x
DPFNUM          ldy     #NUMBUF
                bra     DPFPUT
DPEMPTY         ldy     #TXTBLANK5
DPFPUT          lda     DPROW
                ldb     DPCOL
                addb    #10
                jsr     SCRADDR
                ldb     #5
                jsr     PUTN
                tst     DPCOL                   ; the right half ends at
                bne     DPFX                    ; column 31: no gutter, and
                lda     #VDG_SPACE              ; ,X there is the next row
                sta     ,x                      ; the gutter column, left half
DPFX            rts

; ---- DRAWVOL: the bar and the region tag ---------------------------

; The bar is on the bottom row, and the 6847's border in text mode is
; BLACK, so a solid black bar ($80, the big digits' ink) would run
; straight into it.  Each lit cell is $87: semigraphics with UR, LL and LR
; lit in colour 0 (green, the field) and only UL unlit (black).  That is
; a black block in the top-left quarter: a segmented meter with a gap
; between segments and a green strip under them, clear of the border.
; The value 0-99 follows the bar, since segments are five steps apiece.
VOL_SEG         equ     $87

DRAWVOL         pshs    a,b,x,y
                lda     #VOLROW
                jsr     CLRROW
                ldy     #TXTVOL
                lda     #VOLROW
                ldb     #0
                jsr     PUTSAT
                lda     #VOLROW
                ldb     #4
                jsr     SCRADDR
                lda     #5                      ; bar cells = (VOLUME + 4) / 5,
                sta     DIVSOR                  ; rounded up so 99 fills all
                clra                            ; twenty and 1 shows one
                ldb     VOLUME
                addb    #4
                jsr     DIV16X8
                tfr     b,a
                ldb     #VOLBAR_CELLS
DVLP            pshs    a,b
                tsta
                beq     DVOFF
                lda     #VOL_SEG
                bra     DVPUT
DVOFF           lda     #VDG_SPACE
DVPUT           sta     ,x+
                puls    a,b
                tsta
                beq     DVNX
                deca
DVNX            decb
                bne     DVLP
                clra                            ; the value, two columns,
                ldb     VOLUME                  ; right-aligned: " 5" .. "99"
                jsr     BCD16
                ldy     #NUMBUF+3
                lda     #VOLROW
                ldb     #25
                jsr     SCRADDR
                ldb     #2
                jsr     PUTN
                ldy     #TXTUS
                tst     REGION
                beq     DVREG
                ldy     #TXTEU
DVREG           lda     #VOLROW
                ldb     #29
                jsr     PUTSAT
                puls    a,b,x,y,pc

; ---- DRAWEDIT: the name or frequency being typed -------------------

DRAWEDIT        pshs    a,b,x,y
                lda     #MSGROW
                jsr     CLRROW
                lda     UIMODE
                cmpa    #UI_NAME
                bne     DELFREQ
                ldy     #MSGNAMING
                lda     #MSGROW
                ldb     #0
                jsr     PUTSAT
                ldy     #EDITBUF
                lda     #MSGROW
                ldb     #14
                jsr     SCRADDR
                ldb     #ST_NAMELEN
                jsr     PUTN
                bra     DEDN
DELFREQ         ldy     #MSGFREQ
                lda     #MSGROW
                ldb     #0
                jsr     PUTSAT
                lda     #MSGROW
                ldb     #7
                jsr     SCRADDR
                ldy     #EDITBUF
                ldb     EDITPOS
                beq     DEDN
                jsr     PUTN
DEDN            puls    a,b,x,y,pc

; ===================================================================
; about page
; ===================================================================
; A from the main screen.  Any key returns.  The version comes from
; VER_MAJOR / VER_MINOR and the build stamp from BUILDID.

ABOUTDRAW       jsr     CLS
                ldy     #TITLE
                lda     #0
                ldb     #6
                jsr     PUTSATI
                ldy     #ABTVER
                lda     #4
                ldb     #10
                jsr     SCRADDR
                jsr     PUTS                    ; PUTS leaves X after the text
                ldb     #VER_MAJOR
                jsr     PUTDEC8
                lda     #'.'
                jsr     VDGCHR
                sta     ,x+
                ldb     #VER_MINOR
                jsr     PUTDEC8
                ldy     #ABTBUILD
                lda     #6
                ldb     #10
                jsr     SCRADDR
                jsr     PUTS
                lda     #BUILDID>>8
                jsr     PUTHEX
                lda     #BUILDID&$FF
                jsr     PUTHEX
                if      DEVMPI
                ldy     #ABTDEV
                lda     #7
                ldb     #10
                jsr     PUTSAT
                endif
                ldy     #ABTNAME
                lda     #10
                ldb     #7
                jsr     PUTSAT
                ldy     #ABTURL
                lda     #11
                ldb     #4
                jsr     PUTSAT
                ldy     #ABTKEY
                lda     #15
                ldb     #9
                jmp     PUTSAT

ABOUTKEYS       lda     KEYEVT
                beq     ABKX
                clr     UIMODE
                jsr     DRAWALL
ABKX            rts

; PUTDEC8: B = 0..255, X = screen.  Decimal, no leading blanks; X advances.
PUTDEC8         pshs    a,b,y
                clra
                jsr     BCD16                   ; NUMBUF = five right-aligned
                ldy     #NUMBUF
PD8SKIP         lda     ,y
                cmpa    #' '
                bne     PD8OUT
                leay    1,y
                bra     PD8SKIP                 ; BCD16 always leaves a digit
PD8OUT          lda     ,y+
                jsr     VDGCHR
                sta     ,x+
                cmpy    #NUMBUF+5
                blo     PD8OUT
                puls    a,b,y,pc

; ===================================================================
; text
; ===================================================================

TITLE           fcc     " COCO AM/FM RADIO "
                fcb     0
TXTFM           fcc     "FM"
                fcb     0
TXTAM           fcc     "AM"
                fcb     0
TXTSTEREO       fcc     "STEREO"
                fcb     0
TXTTUNED        fcc     "TUNED"
                fcb     0
TXTMONO         fcc     "MONO"
                fcb     0
TXTMUTE         fcc     "MUTE"
                fcb     0
TXTVOL          fcc     "VOL"
                fcb     0
TXTUS           fcc     "US"
                fcb     0
TXTEU           fcc     "EU"
                fcb     0
TXTBLANK4       fcc     "    "
                fcb     0
TXTBLANK5       fcc     "     "
                fcb     0
TXTBLANK6       fcc     "      "
                fcb     0
UNITMHZ         fcc     "MHZ"
                fcb     0
UNITKHZ         fcc     "KHZ"
                fcb     0
MSGSTORE        fcc     "STORE IN PRESET 1-9,0,SHIFT 1-2"
                fcb     0
MSGNAME         fcc     "NAME WHICH PRESET?"
                fcb     0
MSGNAMING       fcc     "NAME:"
                fcb     0
MSGFREQ         fcc     "FREQ?"
                fcb     0
MSGEMPTY        fcc     "PRESET EMPTY"
                fcb     0
MSGRANGE        fcc     "OUT OF RANGE"
                fcb     0
MSGSEEK         fcc     "SEEKING..."
                fcb     0
MSGNOSTA        fcc     "NO STATION FOUND"
                fcb     0
MSGSAVEFAIL     fcc     "PRESET SAVE FAILED"
                fcb     0
ABTVER          fcc     "VERSION "
                fcb     0
ABTBUILD        fcc     "BUILD   "
                fcb     0
ABTNAME         fcc     "DR. SCOTT M BAKER"
                fcb     0
ABTURL          fcc     "HTTPS://WWW.SMBAKER.COM/"
                fcb     0
ABTKEY          fcc     "PRESS ANY KEY"
                fcb     0
                if      DEVMPI
ABTDEV          fcc     "DEV BUILD"
                fcb     0
                endif
TESTTITLE       fcc     " HARDWARE TEST "
                fcb     0
TWL1            fcc     "LV      US      WM"
                fcb     0
TWL2            fcc     "AGC IN      FB"
                fcb     0
TWL3            fcc     "SM      HC      ST"
                fcb     0
TWL4            fcc     "HB      N     S"
                fcb     0
                if      DEVMPI
MSGDEVEE        fcc     "DEV BUILD: USE A, NOT E"
                fcb     0
MSGDEVMPI       fcc     "DEV: SCS FORCED TO SLOT"
                fcb     0
                endif

HELPTXT
                fcc     "ARROWS  TUNE / VOLUME"
                fcb     0
                fcc     "SHIFT+L/R  SEEK"
                fcb     0
                fcc     "1-9,0,SH1,SH2  PRESET"
                fcb     0
                fcc     "W  STORE      N  NAME"
                fcb     0
                fcc     "B  BAND       R  REGION"
                fcb     0
                fcc     "M  MUTE       S  MONO"
                fcb     0
                fcc     "F  FREQUENCY  T  TEST"
                fcb     0
                fcc     "A  ABOUT"
                fcb     0
                fcc     "BREAK  EXIT TO BASIC"
                fcb     0
                fcb     0
