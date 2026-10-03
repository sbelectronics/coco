; ===================================================================
; ramcode.asm - the EEPROM save routine and the FIRQ stub.
;
; GROUND RULE 1: the ROM is an EEPROM and code must never execute from it
; while it is writing itself.  Everything between the first armed store
; and the end of polling runs from RAM, with interrupts already masked,
; and calls nothing in ROM.
;
; The bytes below sit in the ROM image but every label resolves to its
; RAM address, because of PHASE.  START copies them down before use.
; ===================================================================

                if      EE_SDP
                fatal   "EE_SDP cannot work on this board.  The AT28C256 unlock sequence needs chip address $5555, which has A14 = 1, and A14 (pin 1) is tied to ground here so that address cannot be presented.  Disable Software Data Protection with the chip programmer instead."
                endif

; ---- the save routine, copied to $0600 -----------------------------
; EESAVE   X = source in STORE_RAM, Y = destination in $DF00-$DFFF (or
;          the $FF40 scratch window), B = count 1..64, all inside one
;          64-byte page of Y.
;          Returns EEFAIL = 0 ok, 1 verify mismatch, 2 poll timeout,
;          3 refused (destination outside the writable windows or the
;          range crosses a page).
;          Destroys A, B, X, Y.  Interrupts must already be masked.
;
; A range that crosses a 64-byte boundary, (Y & $3F) + B > 64, is
; refused.

RAMCODE_ROM     equ     *
                phase   RAMCODE

EESAVE          pshs    x,y,b                   ; keep the originals for verify
; Refuse anything outside the two writable windows, and any range that
; would leave its 64-byte page.  The PLD arms the whole chip, so a bad Y
; would be an armed write into the ROM's own code.
; After PSHS X,Y,B the count is at ,S.  B is used as scratch here and
; reloaded from the stack before the first armed store.
                tfr     y,d
                cmpa    #STORE_ROM>>8
                beq     EESRNG                  ; $DF00-$DFFF: any page
                cmpa    #SCRATCH_WIN>>8
                bne     EESBADR
                cmpb    #SCRATCH_WIN&$FF        ; $FF40-$FF5D only: CTLREG and
                blo     EESBADR                 ; STATREG share this page
                pshs    b
                addb    1,s                     ; end = low byte + count
                cmpb    #(SCRATCH_WIN&$FF)+SCRATCH_LEN
                puls    b                       ; PULS leaves CC alone
                bhi     EESBADR
EESRNG          andb    #$3F
                addb    ,s                      ; the count
                cmpb    #64
                bhi     EESBADR
                ldb     ,s                      ; the count, for the loop
                lda     CTLSHD
                ora     #CTL_ARM
                sta     CTLREG                  ; ARM.  The shadow is deliberately
                                                ; NOT updated: it never holds bit 7,
                                                ; so nothing else can leave it armed.
EESLP           lda     ,x+
                sta     ,y+                     ; armed write.  The PLD drives /EEWR
                                                ; for this cycle, and /SLENB too when
                                                ; the address is $C000-$DFFF.
                decb                            ; 17 cycles per byte, about 19 us.
                bne     EESLP                   ; The chip's limit is 150 us.
                lda     CTLSHD
                sta     CTLREG                  ; disarm.  Doing this before the
                                                ; chip's 150 us page-load window
                                                ; closes does not abort the page.

; Wait about 500 us so the page-load window has closed and the internal
; write cycle has actually STARTED.  Polling earlier can read the old
; data, decide the chip is done, and then verify against a busy chip.
; A rewrite of unchanged data (the test screen's E) is exactly that case.
; B is the counter because it is the only free register here: X and Y
; still point past the last source and destination bytes.
                ldb     #90                     ; 90 x 5 cycles
EESWT           decb
                bne     EESWT

                lda     -1,x                    ; the last byte written, from source
                ldx     #0                      ; poll timeout, 65536 iterations,
                                                ; about 1.2 s - far beyond the 10 ms
                                                ; maximum.  It exists so a missing
                                                ; chip cannot hang the ROM.
EESPL           cmpa    -1,y                    ; read the last address written: the
                                                ; chip returns bit 7 inverted until
                                                ; the internal cycle completes
                beq     EESDN
                leax    -1,x
                bne     EESPL
                ldb     #2                      ; timed out
                bra     EESRT

EESDN           puls    x,y,b                   ; restore the originals, verify all
                pshs    b                       ; keep the count for the exit
EESVF           lda     ,x+
                cmpa    ,y+
                bne     EESBAD
                decb
                bne     EESVF
                clrb                            ; 0 = verified
                puls    a                       ; discard the saved count
                bra     EESST
EESBAD          ldb     #1                      ; verify mismatch
                puls    a
                bra     EESST

EESBADR         ldb     #3                      ; refused: bad destination
EESRT           leas    5,s                     ; timeout path: drop the saved Y,X,B
EESST           stb     EEFAIL
                rts                             ; the RTS reads its return address
                                                ; from the RAM stack and lands back
                                                ; in ROM, which is readable again

RAMCODE_END     equ     *
                dephase
RAMCODE_LEN     equ     RAMCODE_END-EESAVE

                if      RAMCODE_LEN>(FIRQSTUB-RAMCODE)
                fatal   "RAMCODE has outgrown the space below FIRQSTUB"
                endif

; ---- the FIRQ stub, copied to $06C0 --------------------------------
; CART* carries Q, a 0.9 MHz square wave, so once
; the cartridge is running the FIRQ line toggles forever.  The exit path
; disables the interrupt at PIA1 CRB, and installs this as a second line
; of defence in case BASIC's warm start re-enables it.
;
; FIRQ stacks only CC and PC, so A must be preserved by hand.  Reading
; PIA1 port B is what clears the CB1 flag.

FIRQSTUB_ROM    equ     *
                phase   FIRQSTUB

FIRQENT         pshs    a
                lda     PIA1DB                  ; clears the CB1 interrupt flag
                puls    a
                rti

FIRQSTUB_END    equ     *
                dephase
FIRQSTUB_LEN    equ     FIRQSTUB_END-FIRQENT
