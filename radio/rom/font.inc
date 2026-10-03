; ===================================================================
; font.inc - 3 x 5 big-digit patterns for the frequency display.
;
; One byte per row, five rows per digit.  Bits 2, 1, 0 are the left,
; middle and right cell of that row.  A set bit becomes a lit
; semigraphics cell, a clear bit becomes a space.
; ===================================================================

BIGFONT
                fcb     %111,%101,%101,%101,%111        ; 0
                fcb     %010,%110,%010,%010,%111        ; 1
                fcb     %111,%001,%111,%100,%111        ; 2
                fcb     %111,%001,%111,%001,%111        ; 3
                fcb     %101,%101,%111,%001,%001        ; 4
                fcb     %111,%100,%111,%001,%111        ; 5
                fcb     %111,%100,%111,%101,%111        ; 6
                fcb     %111,%001,%001,%001,%001        ; 7
                fcb     %111,%101,%111,%101,%111        ; 8
                fcb     %111,%101,%111,%001,%111        ; 9
BIGFONT_LEN     equ     *-BIGFONT
