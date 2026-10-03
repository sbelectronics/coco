; ===================================================================
; kbd.asm - PIA0 keyboard matrix scanner with debounce and auto-repeat.
;
; $FF02 drives the columns (write one bit low), $FF00 senses the rows
; (bits 0..6, 0 = pressed; bit 7 is the joystick comparator and is
; masked off).  Called once per VSYNC tick and never blocks.
;
; SHIFT is not a key and never produces an event.  It sets SHIFTFLG and
; the UI tests that alongside the event, which is how SHIFT+LEFT becomes
; seek-down and SHIFT+1 becomes preset 11.  The scanner does not
; translate shifted punctuation; the UI only ever needs the unshifted
; code plus the flag.
; ===================================================================

KB_NONE         equ     $00             ; table entry: ignore this key
KB_SHIFT        equ     $01             ; table entry: it is SHIFT

; ---- KBDSCAN: one full scan.  Leaves KEYEVT = key or 0. -------------

KBDSCAN         pshs    a,b,x,y
                clr     KEYCUR
                clr     SHIFTFLG
                clr     KEYEVT
                ldy     #KBCOLMASK
                clrb                            ; B = column 0..7

KBCOLLP         lda     b,y                     ; drive one column low
                sta     PIA0DB
                lda     PIA0DA
                coma                            ; now 1 = pressed
                anda    #$7F                    ; drop the joystick comparator
                beq     KBNEXTC

                pshs    b                       ; keep the column number
                ldx     #KEYTAB
                abx                             ; X = row 0 entry for this column
                ldb     #7                      ; seven real rows
KBROWLP         lsra                            ; row 0 first, into carry
                bcc     KBROWNX
                pshs    a,b
                lda     ,x
                beq     KBROWSK                 ; KB_NONE: CoCo 3 key, ignored
                cmpa    #KB_SHIFT
                bne     KBROWKY
                lda     #1
                sta     SHIFTFLG
                bra     KBROWSK
KBROWKY         tst     KEYCUR
                bne     KBROWSK                 ; already have a key this scan
                sta     KEYCUR
KBROWSK         puls    a,b
KBROWNX         leax    8,x                     ; same column, next row
                decb
                bne     KBROWLP
                puls    b

KBNEXTC         incb
                cmpb    #8
                blo     KBCOLLP

                lda     #$FF
                sta     PIA0DB                  ; columns idle again

; ---- debounce and repeat -------------------------------------------
; A key becomes "down" once it has read the same for two consecutive
; ticks and was not already down.  Only the four arrows auto-repeat;
; every other key fires once per press.

                lda     KEYCUR
                beq     KBUP                    ; nothing pressed
                cmpa    KEYPREV
                bne     KBSTORE                 ; not stable yet, wait a tick
                cmpa    KEYDOWN
                beq     KBREPEAT                ; same key still held
                sta     KEYDOWN                 ; a new press
                sta     KEYEVT
                clr     KEYHOLD
                bra     KBSTORE

KBREPEAT        jsr     KBISARROW
                bne     KBSTORE                 ; only arrows repeat
                inc     KEYHOLD
                lda     KEYHOLD
                cmpa    #KEY_REPDELAY
                blo     KBSTORE
                lda     KEYDOWN
                sta     KEYEVT
                lda     #KEY_REPRELOAD          ; next repeat in 4 ticks
                sta     KEYHOLD
                bra     KBSTORE

KBUP            clr     KEYDOWN
                clr     KEYHOLD

KBSTORE         lda     KEYCUR
                sta     KEYPREV
                puls    a,b,x,y,pc

; ---- KBISARROW: A = key code.  Returns Z = 1 if it is an arrow. -----

KBISARROW       cmpa    #K_UP
                beq     KBIAYES
                cmpa    #K_DOWN
                beq     KBIAYES
                cmpa    #K_LEFT
                beq     KBIAYES
                cmpa    #K_RIGHT                ; sets Z itself
KBIAYES         rts

; ---- tables ---------------------------------------------------------

KBCOLMASK       fcb     $FE,$FD,$FB,$F7,$EF,$DF,$BF,$7F

; Seven rows of eight columns, indexed row*8 + column.
KEYTAB
                ; row 0
                fcb     '@','A','B','C','D','E','F','G'
                ; row 1
                fcb     'H','I','J','K','L','M','N','O'
                ; row 2
                fcb     'P','Q','R','S','T','U','V','W'
                ; row 3
                fcb     'X','Y','Z',K_UP,K_DOWN,K_LEFT,K_RIGHT,K_SPACE
                ; row 4
                fcb     '0','1','2','3','4','5','6','7'
                ; row 5
                fcb     '8','9',':',';',',','-','.','/'
                ; row 6   (ALT, CTRL, F1, F2 are CoCo 3 only and ignored)
                fcb     K_ENTER,K_CLEAR,K_BREAK,KB_NONE
                fcb     KB_NONE,KB_NONE,KB_NONE,KB_SHIFT
