; ===================================================================
; screen.asm - VDG text output, number formatting and the big digits.
;
; VDG character codes, for an ASCII character c:
;   normal    $40-$5F -> c            $20-$3F -> c + $40
;   inverse   $40-$5F -> c AND $3F    $20-$3F -> c
; Lower case is folded to upper case first.  The "lit" cell for the big
; digits is $80, solid black, for the reason given at VDG_BLOCK below.
; ===================================================================

VDG_SPACE       equ     $60             ; a space in normal VDG coding
; THE "INK" CELL IS BLACK, NOT GREEN.  In semigraphics a lit quadrant
; shows the cell's colour and colour 0 is green, the same as the text
; field, so $8F (all lit, colour 0) is invisible next to $60 spaces; it is
; why CLS 1 looks the same as CLS.  $80 lights no quadrants: solid black,
; the same ink as the text.
VDG_BLOCK       equ     $80             ; no quadrants lit: solid black
BIGROW          equ     2               ; top screen row of the big digits

; ---- VSYNC: wait for one 60 Hz field -------------------------------
; The flag sets in PIA0 CRB bit 7 on every field whether or not the
; interrupt is enabled, which is what makes it usable as a tick with
; interrupts masked.  Reading the port B data register clears it.

VSYNC           lda     PIA0CRB
                bpl     VSYNC
                lda     PIA0DB
                rts

; ---- DELAY: B = ticks to wait --------------------------------------

DELAY           pshs    b
DELAYLP         jsr     VSYNC
                dec     ,s
                bne     DELAYLP
                puls    b,pc

; ---- VDGCHR / VDGINV: A = ASCII -> A = VDG code --------------------

VDGCHR          cmpa    #'a'
                blo     VDGC1
                cmpa    #'z'
                bhi     VDGC1
                suba    #$20                    ; fold to upper case
VDGC1           cmpa    #$40
                bhs     VDGC2                   ; $40-$5F passes through
                adda    #$40                    ; $20-$3F moves up
VDGC2           rts

VDGINV          cmpa    #'a'
                blo     VDGV1
                cmpa    #'z'
                bhi     VDGV1
                suba    #$20
VDGV1           cmpa    #$40
                blo     VDGV2                   ; $20-$3F passes through
                anda    #$3F
VDGV2           rts

; ---- SCRADDR: A = row, B = column -> X = screen address ------------
; A and B are preserved, which several callers rely on.

SCRADDR         pshs    a,b                     ; ,S = A (row)  1,S = B (col)
                lda     ,s
                ldb     #SCRCOLS
                mul                             ; D = row * 32
                addb    1,s
                adca    #0
                addd    #SCREEN
                tfr     d,x
                puls    a,b,pc

; ---- CLS / CLRROW --------------------------------------------------

CLS             pshs    a,b,x
                ldx     #SCREEN
                lda     #VDG_SPACE
                clrb                            ; 256 passes, two bytes each
CLSLP           sta     ,x+
                sta     ,x+
                decb
                bne     CLSLP
                puls    a,b,x,pc

CLRROW          pshs    a,b,x
                clrb
                jsr     SCRADDR
                lda     #VDG_SPACE
                ldb     #SCRCOLS
CLRROWL         sta     ,x+
                decb
                bne     CLRROWL
                puls    a,b,x,pc

; ---- PUTS / PUTSI: Y = zero-terminated ASCII, X = screen pointer ---
; Both advance X and Y.

PUTS            pshs    a
PUTSLP          lda     ,y+
                beq     PUTSDN
                jsr     VDGCHR
                sta     ,x+
                bra     PUTSLP
PUTSDN          puls    a,pc

PUTSI           pshs    a
PUTSILP         lda     ,y+
                beq     PUTSIDN
                jsr     VDGINV
                sta     ,x+
                bra     PUTSILP
PUTSIDN         puls    a,pc

; ---- PUTSAT / PUTSATI: A = row, B = column, Y = string -------------

PUTSAT          pshs    x
                jsr     SCRADDR
                jsr     PUTS
                puls    x,pc

PUTSATI         pshs    x
                jsr     SCRADDR
                jsr     PUTSI
                puls    x,pc

; ---- PUTN: Y = buffer, B = count, X = screen pointer ---------------
; Fixed length, not terminated.  Used for names and formatted numbers.

PUTN            pshs    a,b
PUTNLP          lda     ,y+
                jsr     VDGCHR
                sta     ,x+
                decb
                bne     PUTNLP
                puls    a,b,pc

; ---- PUTNAT: A = row, B = column, Y = buffer, TMP = count ----------

PUTNAT          pshs    b,x
                jsr     SCRADDR
                ldb     TMP
                jsr     PUTN
                puls    b,x,pc

; ---- DIV16X8: D = dividend, DIVSOR = divisor -----------------------
; Returns D = quotient and DIVTMP+2 = remainder.  Sixteen-step shift and
; subtract; the 6809 has no divide instruction.
;
; DIVTMP+0,1 hold the dividend and collect the quotient as it shifts
; left, DIVTMP+2 is the running remainder.  The remainder can carry out
; of eight bits when shifted, so the carry is tested BEFORE the compare.
; Without that test a dividend with its high bit set divides wrong.

DIV16X8         pshs    a,b,x
                std     DIVTMP
                clr     DIVTMP+2
                ldx     #16
DIVLP           lsl     DIVTMP+1
                rol     DIVTMP
                rol     DIVTMP+2
                bcs     DIVSUB                  ; carried out: certainly >= divisor
                lda     DIVTMP+2
                cmpa    DIVSOR
                blo     DIVNX
DIVSUB          lda     DIVTMP+2
                suba    DIVSOR
                sta     DIVTMP+2
                inc     DIVTMP+1                ; set this quotient bit
DIVNX           leax    -1,x
                bne     DIVLP
                puls    a,b,x
                ldd     DIVTMP                  ; quotient
                rts

; ---- BCD16: D = value -> NUMBUF, five ASCII digits, right aligned ---
; Leading zeros become spaces, so 0 shows as four spaces and a '0'.
; THE DIVISOR IS SET ONCE, OUTSIDE THE LOOP.  Loading it needs A, which
; holds the high byte of the dividend.

BCD16           pshs    a,b,x,y
                ldx     #NUMBUF+5               ; fill backwards
                ldy     #5                      ; digits still to place
                pshs    a,b                     ; hold the value across this
                lda     #10
                sta     DIVSOR
                puls    a,b
BCDLP           jsr     DIV16X8                 ; D = quotient
                pshs    a,b
                lda     DIVTMP+2                ; remainder is this digit
                adda    #'0'
                sta     ,-x
                puls    a,b
                leay    -1,y
                beq     BCDDN                   ; all five placed
                cmpd    #0
                bne     BCDLP
BCDBL           lda     #' '                    ; pad the rest with spaces
                sta     ,-x
                leay    -1,y
                bne     BCDBL
BCDDN           puls    a,b,x,y,pc

; ---- BIGDIG: A = digit 0..9 or $FF for blank, B = column -----------
; Draws five rows of three cells with the top-left at (BIGROW, column).

BIGDIG          pshs    a,b,x,y                 ; ,S=A  1,S=B  2,S=X  4,S=Y
                cmpa    #$FF
                beq     BIGDBL
                ldy     #BIGFONT
                ldb     #5
                mul                             ; D = digit * 5
                leay    d,y                     ; Y = this digit's five rows
                ldb     1,s                     ; recover the column
                lda     #BIGROW
                jsr     SCRADDR                 ; X = top-left cell
                ldb     #5                      ; five rows to draw
BIGRLP          pshs    b                       ; rows remaining
                lda     ,y+                     ; the pattern byte
                pshs    a                       ; ,S = pattern, 1,S = rows
                ldb     #%100                   ; left cell first
BIGCLP          lda     ,s                      ; the pattern again
                pshs    b
                anda    ,s+                     ; A = pattern AND mask
                beq     BIGCOFF
                lda     #VDG_BLOCK
                bra     BIGCPUT
BIGCOFF         lda     #VDG_SPACE
BIGCPUT         sta     ,x+
                lsrb                            ; %100 -> %010 -> %001 -> 0
                bne     BIGCLP
                puls    a                       ; drop the pattern
                leax    SCRCOLS-3,x             ; next screen row, same column
                puls    b
                decb
                bne     BIGRLP
                puls    a,b,x,y,pc

BIGDBL          ldb     1,s                     ; blank: five rows of three spaces
                lda     #BIGROW
                jsr     SCRADDR
                ldb     #5
                lda     #VDG_SPACE
BIGBRLP         sta     ,x
                sta     1,x
                sta     2,x
                leax    SCRCOLS,x
                decb
                bne     BIGBRLP
                puls    a,b,x,y,pc

; ---- BIGDOT / BIGDOTCLR: B = column.  The decimal point cell. ------
; One lit cell on the bottom row of the big-digit block.

BIGDOT          pshs    a,x
                lda     #VDG_BLOCK
                bra     BIGDOTGO
BIGDOTCLR       pshs    a,x
                lda     #VDG_SPACE
BIGDOTGO        pshs    a
                lda     #BIGROW+4
                jsr     SCRADDR
                puls    a
                sta     ,x
                puls    a,x,pc
