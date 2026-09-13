; ======================================================================
; stub6809.asm — cold-boot stub served in place of the chosen ROM for
; one CoCo boot cycle.
;
; Pulling !RESET is electrically the reset button, i.e. a WARM start:
; RAM survives and Color BASIC honours the $71 = $55 warm-start flag.
; This stub turns that into a real cold start:
;
;   1. entered at $C000 by the cartridge FIRQ (CART*/Q autostart)
;   2. copies its tail into RAM (the ROM is about to change underneath)
;   3. clears the warm-start flag, forcing a cold start
;   4. writes $A5 to $FF5F -> the SCS write strobe the Pico watches;
;      that is the "swap me now" signal
;   5. spins ~145 ms while the Pico swaps in the real image and sets the
;      autostart gate for it (generous: the Pico's pump loop can stall
;      ~45 ms behind a flash sector erase during a concurrent upload)
;   6. jumps through the reset vector -> guaranteed cold boot into the
;      new ROM
;
; The tail is position independent (all absolute operands, relative
; branches), so the copy destination is arbitrary; $0600 is used because
; a cold start reinitialises everything above the direct page anyway.
;
; Assemble with lwasm for reference; stub6809.h holds the bytes so the
; build has no assembler dependency:
;     lwasm --raw -o stub6809.bin stub6809.asm
; ======================================================================

            org     $C000

ENTRY:
            orcc    #$50            ; belt+braces: FIRQ entry already masks
            ldx     #TAIL           ; source in cartridge ROM
            ldy     #$0600          ; destination in RAM
            ldb     #TAILLEN
COPY:
            lda     ,x+
            sta     ,y+
            decb
            bne     COPY
            jmp     $0600

; ---- everything below runs from RAM ----------------------------------
TAIL:
            clr     $0071           ; warm-start flag -> cold start
            lda     #$A5
            sta     $FF5F           ; strobe: Pico swaps to the real image
            ldx     #$4000
DLY:
            leax    -1,x            ; 8 cycles/iteration @0.89 MHz
            bne     DLY             ; ~= 145 ms
            jmp     [$FFFE]         ; reset vector -> cold boot

TAILLEN     equ     *-TAIL

            end     ENTRY
