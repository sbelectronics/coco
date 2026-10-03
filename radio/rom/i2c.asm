; ===================================================================
; i2c.asm - bit-banged I2C master on two bits of the '273 control latch.
;
; The latch is PUSH-PULL, not open drain.  A 1 kOhm series resistor sits
; between each latch output and the module pin, and 4.7 kOhm pull-ups sit
; at the module end.  "Releasing" a line means writing 1: the pull-up
; takes it high unless the module is pulling it down through the series
; resistor.  NEVER treat a written 1 as proof the line is high - always
; read STATREG bit 0 for the actual SDA level.
;
; Every edge is a single STA, 5 cycles = 5.6 us at 0.894886 MHz, and each
; primitive is about 20 us.  The bus therefore runs near 40 kHz and no
; delay loops are needed or wanted.  It is not possible to exceed 100 kHz.
;
; SCL is not routed back to the status port, so clock stretching CANNOT
; be detected.  The TEF6686 does not stretch at these speeds; a module
; that did would make this code fail silently.
; ===================================================================

; ---- line primitives ----------------------------------------------
; Each preserves nothing but CTLSHD.  Bit 7 (ARM) is never set here: the
; shadow never holds it, so ORing/ANDing the shadow can never arm a write.

SCLHI           lda     CTLSHD
                ora     #CTL_SCL
                sta     CTLSHD
                sta     CTLREG
                rts

SCLLO           lda     CTLSHD
                anda    #~CTL_SCL
                sta     CTLSHD
                sta     CTLREG
                rts

SDAHI           lda     CTLSHD
                ora     #CTL_SDA
                sta     CTLSHD
                sta     CTLREG
                rts

SDALO           lda     CTLSHD
                anda    #~CTL_SDA
                sta     CTLSHD
                sta     CTLREG
                rts

; ---- START and STOP ------------------------------------------------
; START: SDA falls while SCL is high.  STOP: SDA rises while SCL is high.

I2CSTART        jsr     SDAHI
                jsr     SCLHI
                jsr     SDALO
                jsr     SCLLO
                rts

I2CSTOP         jsr     SDALO
                jsr     SCLHI
                jsr     SDAHI
                rts

; ---- I2COUT: send A.  Sets I2CERR = 0 (ACK) or 1 (NAK). ------------
; Data changes only while SCL is low; the slave samples on the rising
; edge.  After eight bits we release SDA and clock once more: the slave
; pulls SDA low to acknowledge.

I2COUT          pshs    a
                ldb     #8
I2COLP          lsl     ,s                      ; MSB first, into carry
                bcs     I2CO1
                jsr     SDALO
                bra     I2COCK
I2CO1           jsr     SDAHI
I2COCK          jsr     SCLHI
                jsr     SCLLO
                decb
                bne     I2COLP
                jsr     SDAHI                   ; release for the ACK bit
                jsr     SCLHI
                lda     STATREG                 ; slave pulls SDA low to ACK
                anda    #ST_SDA                 ; 0 = ACK, 1 = NAK
                sta     I2CERR
                jsr     SCLLO
                puls    a
                rts

; ---- I2CIN: receive into A.  B = 0 to ACK the byte, 1 to NAK it. ----
; The last byte of a read must be NAKed, which is how the slave is told
; to let go of SDA before the STOP.

I2CIN           pshs    b                       ; keep the ACK/NAK choice
                clr     I2CACC
                jsr     SDAHI                   ; release SDA so the slave drives
                ldb     #8
I2CILP          jsr     SCLHI
                lda     STATREG
                anda    #ST_SDA                 ; A = 0 or 1
                lsl     I2CACC                  ; make room for it
                ora     I2CACC
                sta     I2CACC
                jsr     SCLLO
                decb
                bne     I2CILP
                ldb     ,s                      ; the caller's ACK/NAK choice
                bne     I2CINAK
                jsr     SDALO                   ; ACK: hold SDA low for the clock
                bra     I2CICK
I2CINAK         jsr     SDAHI                   ; NAK: leave it released
I2CICK          jsr     SCLHI
                jsr     SCLLO
                jsr     SDAHI                   ; let go either way
                puls    b
                lda     I2CACC
                rts

; ---- I2CWR: X = buffer, B = count.  START, address, bytes, STOP. ----
; Aborts early and leaves I2CERR set on the first NAK.  A zero count is
; legal and sends address only, which is how a bus probe works.

I2CWR
                if      EMU
                clr     I2CERR                  ; emulator: always succeed
                rts
                endif
; THE COUNT LIVES ON THE STACK, NOT IN B.  I2COUT uses B as its bit
; counter and returns with it at zero, so counting down in B here would
; send 256 bytes on every call.
                pshs    x,b                     ; ,S = count, 1,S = X
                jsr     I2CSTART
                lda     I2CADDR
                jsr     I2COUT
                tst     I2CERR
                bne     I2CWRDN                 ; no ACK to the address
                tst     ,s                      ; a zero count is legal and
                beq     I2CWRDN                 ; means address only, a probe
I2CWRLP         lda     ,x+
                jsr     I2COUT
                tst     I2CERR
                bne     I2CWRDN
                dec     ,s
                bne     I2CWRLP
I2CWRDN         jsr     I2CSTOP
                puls    x,b
                rts

; ---- I2CRD: X = buffer, B = count.  START, address|1, read, STOP. ---
; The final byte is NAKed.  On a NAK to the address nothing is read and
; I2CERR is left set.

I2CRD
                if      EMU
                clr     I2CERR
                rts
                endif
                tstb
                beq     I2CRDNO                 ; refuse a zero-length read
                pshs    x,b
                jsr     I2CSTART
                lda     I2CADDR
                ora     #$01                    ; read address
                jsr     I2COUT
                tst     I2CERR
                bne     I2CRDDN
; THE COUNT LIVES ON THE STACK, NOT IN B, as in I2CWR: the address byte
; went out through I2COUT, which returns B at zero.  Counting down in B
; would write 256 bytes into the 16-byte buffer and wreck the variable
; page, with symptoms (dead keys, blank numbers) that look nothing like
; an I2C fault.
I2CRDLP         dec     ,s                      ; bytes left after this one
                bne     I2CRDACK
                ldb     #1                      ; last byte: NAK it
                bra     I2CRDGO
I2CRDACK        clrb                            ; more to come: ACK it
I2CRDGO         jsr     I2CIN
                sta     ,x+
                tst     ,s
                bne     I2CRDLP
I2CRDDN         jsr     I2CSTOP
                puls    x,b
                rts
I2CRDNO         lda     #I2CERR_NAK
                sta     I2CERR
                rts

; ---- I2CIDLE: put the bus into the defined idle state ---------------
; Two writes, in this order: release SCL while SDA is still low, then
; release SDA while SCL is high.  That second edge is a STOP, the defined
; way to idle a slave left mid-transaction by a reset.  Releasing both in
; one store is not a defined bus condition.

I2CIDLE         lda     #CTL_SCL
                sta     CTLSHD
                sta     CTLREG
                lda     #CTL_IDLE
                sta     CTLSHD
                sta     CTLREG
                clr     I2CERR
                rts
