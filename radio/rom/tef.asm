; ===================================================================
; tef.asm - TEF6686 tuner module control over I2C.
;
; Everything here follows the NXP TEF668X User Manual rev 1.6.  Frame
; shape is  module, command, index, then 16-bit parameters MSB first.
; Index is always 1: /V102 silicon supports no other value and also
; requires that every parameter a command defines is actually sent.
;
; FOUR THINGS THAT ARE NOT OBVIOUS:
;
;  1. THERE IS NO SEEK COMMAND.  Tune mode 2 is called "Search", but it
;     only means "tune and stay muted"; the host does the stepping and
;     the deciding.  Seek is therefore a host loop driven by the module's
;     quality readings.
;
;  2. THE MODULE BOOTS MUTED.  Set_Mute defaults to 1.  Silence after a
;     perfect-looking tune is almost always this.
;
;  3. ONLY Preset AND Search ENABLE THE RADIO.  Jump and Check do not
;     bring it out of standby and do not permit an FM/AM band change, so
;     the first tune after activation must be Preset.
;
;  4. IN BOOT STATE EVERY GET RETURNS ZERO.  The module acknowledges its
;     address and looks perfectly alive while returning nothing but
;     zeros.  If Get_Operation_Status stays 0, it has not been patched.
; ===================================================================

; ---- TEFHDR: A = module, B = command.  Builds the 3-byte header. ----
; Returns X pointing at I2CBUF+3, where parameters go.

TEFHDR          ldx     #I2CBUF
                sta     ,x+
                stb     ,x+
                lda     #TEF_INDEX
                sta     ,x+
                rts

; ---- TEFSEND: B = total bytes in I2CBUF.  Write them. --------------

TEFSEND         ldx     #I2CBUF
                jsr     I2CWR
                rts

; ---- TEFGET: A = module, B = command, TMP = bytes to read ----------
; Writes the 3-byte header, then reads into I2CBUF.  Leaves I2CERR set
; on failure.  At 40 kHz one address byte already takes 225 us, so the
; manual's 50 us read-setup requirement is met with no added delay.

TEFGET          jsr     TEFHDR
                ldx     #I2CBUF
                ldb     #3
                jsr     I2CWR
                tst     I2CERR
                bne     TEFGETX
                ldx     #I2CBUF
                ldb     TMP
                jsr     I2CRD
TEFGETX         rts

; ---- TEFOPSTAT: read the operation status into A -------------------
; 0 boot (needs the patch), 1 idle, 2 active standby, 3 FM, 4 AM.
; Returns A = $FF if the module did not answer.

TEFOPSTAT
                if      EMU
                lda     #TEFS_FM
                clr     I2CERR
                rts
                endif
                lda     #2
                sta     TMP
                lda     #TEFM_APPL
                ldb     #TEFC_OPSTATUS
                jsr     TEFGET
                tst     I2CERR
                bne     TEFOPSNO
                lda     I2CBUF+1                ; low byte of the status word
                rts
TEFOPSNO        lda     #$FF
                rts

; ---- TEFIDENT: read the identification.  Z = 1 if it is a TEF6686. --
; Six bytes back: device, hw_version, sw_version.  device should be
; $090E - type 9 is the Lithio series, variant 14 is the TEF6686.
; Only meaningful once the module has left boot state.

TEFIDENT
                if      EMU
                ldd     #TEF_DEVID
                std     I2CBUF
                clr     I2CERR
                cmpd    #TEF_DEVID
                rts
                endif
                lda     #6
                sta     TMP
                lda     #TEFM_APPL
                ldb     #TEFC_IDENT
                jsr     TEFGET
                tst     I2CERR
                bne     TEFIDNO
                ldd     I2CBUF
                cmpd    #TEF_DEVID
                rts
TEFIDNO         ldd     #0
                cmpd    #TEF_DEVID              ; clears Z
                rts

; ---- TEFPROBE: find the module at $C8, else $CA.  Z = 1 if found. ---
; A module that acknowledges its address proves the bus, the series
; resistors, the pull-ups and the latch wiring, and needs neither the
; patch nor an antenna.

TEFPROBE
                if      EMU
                lda     #I2C_ADDR_W
                sta     I2CADDR
                clr     I2CERR
                rts
                endif
                lda     #I2C_ADDR_W
                sta     I2CADDR
                jsr     TEFPING
                beq     TEFPRBX
                lda     #I2C_ALT_W              ; GPIO_2 was high at power-up
                sta     I2CADDR
                jsr     TEFPING
TEFPRBX         rts

; TEFPING: address-only write.  Z = 1 if the slave acknowledged.
TEFPING         ldx     #I2CBUF
                clrb                            ; zero-length: address only
                jsr     I2CWR
                lda     I2CERR
                tsta
                rts

; ---- TEFSTART: leave boot state.  Raw register write, not a frame. --

TEFSTART        ldx     #I2CBUF
                lda     #TEFR_START
                sta     ,x+
                clr     ,x+
                lda     #1
                sta     ,x+
                ldb     #3
                jmp     TEFSEND

; ---- TEFACTIVATE: idle -> active.  APPL Set_Activate(1). -----------

TEFACTIVATE     lda     #TEFM_APPL
                ldb     #TEFC_ACTIVATE
                jsr     TEFHDR
                clr     ,x+
                lda     #1
                sta     ,x+
                ldb     #5
                jmp     TEFSEND

; ---- TEFMUTE: A = 0 to unmute, 1 to mute ---------------------------
; The module powers up muted, so this is not optional.

TEFMUTE         pshs    a
                lda     #TEFM_AUDIO
                ldb     #TEFC_MUTE
                jsr     TEFHDR
                clr     ,x+                     ; parameter high byte
                puls    a
                sta     ,x+
                ldb     #5
                jmp     TEFSEND

; ---- TEFVOL: push VOLUME to the module -----------------------------
; Stored VOLUME is 0..99.  The module wants a SIGNED value in 0.1 dB
; steps, and anything at or below -600 mutes it.  See equates.inc for
; the ceiling.
;     tenths = VOL_MIN_T + VOLUME * VOL_STEP_T

TEFVOL          lda     VOLUME
                ldb     #VOL_STEP_T
                mul                             ; D = VOLUME * step, always +ve
                addd    #VOL_MIN_T              ; now signed, -400..-4
                pshs    a,b                     ; keep the 16-bit value
                lda     #TEFM_AUDIO
                ldb     #TEFC_VOLUME
                jsr     TEFHDR
                puls    a,b
                sta     ,x+                     ; MSB first
                stb     ,x+
                ldb     #5
                jmp     TEFSEND

; ---- TEFMONO: push MONOFLG to the module ---------------------------
; FM Set_Stereo_Min(mode, limit).  mode 0 = normal, 2 = forced mono.
; The limit parameter must still be sent even in forced mono, because
; /V102 requires every documented parameter.

TEFMONO         lda     #TEFM_FM
                ldb     #TEFC_STEREOMIN
                jsr     TEFHDR
                clr     ,x+                     ; mode, high byte
                lda     MONOFLG
                beq     TEFMONO0
                lda     #2                      ; forced mono
TEFMONO0        sta     ,x+
                ldd     #400                    ; limit = 40.0 dB, the default
                sta     ,x+
                stb     ,x+
                ldb     #7
                jmp     TEFSEND

; ---- TEFDEEMPH: FM de-emphasis to match REGION ---------------------
; FM Set_Deemphasis(timeconstant): 750 = 75 us (the Americas), 500 =
; 50 us (everywhere else, and the module's power-up default).  The
; module does the de-emphasis, so the board needs no network for it.

TEFDEEMPH       lda     #TEFM_FM
                ldb     #TEFC_DEEMPH
                jsr     TEFHDR
                ldd     #750
                tst     REGION
                beq     TEFDEEM0                ; US
                ldd     #500                    ; EU
TEFDEEM0        sta     ,x+
                stb     ,x+
                ldb     #5
                jmp     TEFSEND

; ---- TEFFREQ: STATION -> D in the module's units -------------------
; FM: the station word is in 50 kHz units and the module wants 10 kHz,
; so multiply by five.  1946 (97.3 MHz) becomes 9730.  Maximum 2160 * 5
; = 10800, which still fits sixteen bits.
; AM: both are kHz, so it passes straight through.

TEFFREQ         ldd     STATION
                anda    #$7F                    ; strip the band bit
                tst     BAND
                beq     TEFFRDN                 ; AM needs no conversion
                pshs    a,b                     ; keep x1
                aslb
                rola                            ; x2
                aslb
                rola                            ; x4
                addd    ,s++                    ; x4 + x1
TEFFRDN         rts

; ---- TEFTUNE: A = tune mode (Preset, Search or End) ----------------
; Band comes from BAND and frequency from STATION.  Mode End needs no
; frequency and the manual says it is ignored, but sending it is
; harmless and keeps one code path.

TEFTUNE         pshs    a                       ; the mode
                lda     #TEFM_AM
                tst     BAND
                beq     TEFTUN0
                lda     #TEFM_FM
TEFTUN0         ldb     #TEFC_TUNE
                jsr     TEFHDR
                clr     ,x+                     ; mode, high byte
                puls    a
                sta     ,x+
                jsr     TEFFREQ
                sta     ,x+                     ; frequency, MSB first
                stb     ,x+
                ldb     #7
                jmp     TEFSEND

; ---- TEFQUAL: read the quality block and unpack it -----------------
; Fourteen bytes, seven 16-bit words.  Word 0 is a status whose low ten
; bits are a time stamp: 0 means tuning is still in progress and every
; other word is meaningless.  Reading before it settles is exactly how a
; seek ends up stopping on noise.
;
; Sets LEVEL (whole dBuV, signed), QUSN (percent) and QOFFSET (signed
; 0.1 kHz).  Returns Z = 1 if the data was settled and usable.

TEFQUAL
                if      EMU
                lda     #55                     ; canned strong signal
                sta     LEVEL
                clr     QUSN
                ldd     #0
                std     QOFFSET
                clr     I2CERR
                orcc    #$04                    ; Z = 1, data is good
                rts
                endif
                lda     #QUALITY_LEN
                sta     TMP
                lda     #TEFM_AM
                tst     BAND
                beq     TEFQ0
                lda     #TEFM_FM
TEFQ0           ldb     #TEFC_QUALITY
                jsr     TEFGET
                tst     I2CERR
                bne     TEFQBAD

                ldd     I2CBUF+QW_STATUS        ; time stamp in bits 9..0
                anda    #$03
                cmpd    #0
                beq     TEFQBAD                 ; still tuning, data is junk

; level: signed, 0.1 dBuV, -200..1200.  Bias it positive, divide by ten,
; then unbias, which avoids needing a signed divide.
                lda     #10
                sta     DIVSOR
                ldd     I2CBUF+QW_LEVEL
                addd    #200                    ; 0..1400, now unsigned
                jsr     DIV16X8
                subb    #20                     ; -20..120 dBuV
                stb     LEVEL

; usn: 0..1000 = 0..100 percent
                lda     #10
                sta     DIVSOR
                ldd     I2CBUF+QW_USN
                jsr     DIV16X8
                stb     QUSN

; offset: signed, already in 0.1 kHz
                ldd     I2CBUF+QW_OFFSET
                std     QOFFSET

                clr     I2CERR
                orcc    #$04                    ; Z = 1, data is good
                rts

TEFQBAD         clr     LEVEL
                clr     QUSN
                ldd     #0
                std     QOFFSET
                andcc   #$FB                    ; Z = 0, data is not usable
                rts

; ---- TEFSTEREO: FM Get_Signal_Status -> STEREOFLG ------------------
; Bit 15 of the returned word is the stereo pilot.  This is a SEPARATE
; command from the quality read, not a bit inside it.  AM is always mono.

TEFSTEREO
                if      EMU
                lda     #1
                sta     STEREOFLG
                rts
                endif
                clr     STEREOFLG
                tst     BAND
                beq     TEFSTX                  ; AM: never stereo
                lda     #2
                sta     TMP
                lda     #TEFM_FM
                ldb     #TEFC_SIGNAL
                jsr     TEFGET
                tst     I2CERR
                bne     TEFSTX
                lda     I2CBUF
                bpl     TEFSTX                  ; bit 15 clear = mono
                lda     #1
                sta     STEREOFLG
TEFSTX          rts

; ---- TEFAGC / TEFPROC: the two gain-state reads, for the test screen -
; Get_AGC (UM 4.3) returns the RF front-end attenuation in 0.1 dB, two
; words: input_att 0..420 and feedback_att 0..60, stepping in 60s.
; Get_Processing_Status (UM 4.5) returns the weak-signal control states
; in 0.1 %: softmute, highcut, stereo blend, stereo high blend, then two
; FMSI words that are not present on a TEF6686.  Only the first eight
; bytes are read; a short read is legal, the master NAKs the last byte.
; Softmute is the only control in the chip that reduces audio level
; (20 dB maximum by default); the others shape bandwidth or channel
; separation.  The words land in AGCW / PROCW untouched.

TEFAGC
                if      EMU
                ldd     #0
                std     AGCW
                std     AGCW+2
                rts
                endif
                lda     #4
                sta     TMP
                lda     #TEFM_AM
                tst     BAND
                beq     TEFAGC0
                lda     #TEFM_FM
TEFAGC0         ldb     #TEFC_AGC
                jsr     TEFGET
                ldd     I2CBUF
                std     AGCW
                ldd     I2CBUF+2
                std     AGCW+2
                rts

TEFPROC
                if      EMU
                ldd     #0
                std     PROCW
                std     PROCW+2
                std     PROCW+4
                std     PROCW+6
                rts
                endif
                lda     #8
                sta     TMP
                lda     #TEFM_AM
                tst     BAND
                beq     TEFPROC0
                lda     #TEFM_FM
TEFPROC0        ldb     #TEFC_PROCSTAT
                jsr     TEFGET
                ldd     I2CBUF
                std     PROCW
                ldd     I2CBUF+2
                std     PROCW+2
                ldd     I2CBUF+4
                std     PROCW+4
                ldd     I2CBUF+6
                std     PROCW+6
                rts

; ---- TEFWAITQ: wait for the quality data to settle after a tune ----
; Returns Z = 1 when usable data is in LEVEL / QUSN / QOFFSET.
; TEFQUAL destroys B, so the poll counter lives on the stack.
TEFWAITQ        pshs    b
                ldb     #TEF_TSETTLE
                jsr     DELAY
                ldb     #TEF_POLLMAX
                pshs    b                       ; ,S = counter, 1,S = caller's B
TEFWQLP         jsr     TEFQUAL
                beq     TEFWQOK
                ldb     #1
                jsr     DELAY
                dec     ,s
                bne     TEFWQLP
                andcc   #$FB                    ; never settled
                leas    1,s
                puls    b,pc
TEFWQOK         orcc    #$04
                leas    1,s
                puls    b,pc

; ---- TEFWAITST: wait for operation status A.  Z = 1 on success. ----
; TEFOPSTAT destroys B, so the poll counter lives on the stack across
; the call rather than in a register.
; After PSHS A,B:  ,S = wanted status, 1,S = caller's B.
; After PSHS B:    ,S = counter, 1,S = wanted status, 2,S = caller's B.

TEFWAITST       pshs    a,b                     ; ,S = wanted, 1,S = caller's B
                ldb     #TEF_POLLMAX
                pshs    b                       ; ,S = counter, 1,S = wanted
; AT OR ABOVE the wanted state, not exactly equal.  The states are
; ordered (0 boot, 1 idle, 2 radio standby, 3 FM, 4 AM), and the module
; may settle past the one being waited for: after Activate it can report
; FM or AM rather than standby.  $FF is not a state, it means the read
; failed, so it must not satisfy the comparison.
TEFWSLP         jsr     TEFOPSTAT               ; A = status, B destroyed
                cmpa    #$FF
                beq     TEFWSNX                 ; no answer: keep polling
                cmpa    1,s
                bhs     TEFWSOK
TEFWSNX         ldb     #1
                jsr     DELAY
                dec     ,s                      ; one poll used up
                bne     TEFWSLP
                andcc   #$FB                    ; Z = 0, never reached it
                leas    1,s
                puls    a,b,pc
TEFWSOK         orcc    #$04
                leas    1,s
                puls    a,b,pc

; ---- TEFPSEL: A = target byte.  Sends the raw [w 1C 00 xx] frame. ---
; $00 ends a section, $74 selects the patch, $75 selects the lookup
; table.  Raw register write, not a module/command/index frame.

TEFPSEL         pshs    a
                ldx     #I2CBUF
                lda     #TEFR_PATCHSEL
                sta     ,x+
                clr     ,x+
                puls    a
                sta     ,x+
                ldb     #3
                jmp     TEFSEND

; ---- TEFPBLK: X = source, TMP = count.  Sends [w 1B <data>]. -------
; X is left advanced past the block.  The count must be even: the
; manual allows any split on 16-bit word boundaries but not inside one.
; The data is streamed straight out of ROM rather than through I2CBUF,
; which is only sixteen bytes.

TEFPBLK         jsr     I2CSTART
                lda     I2CADDR
                jsr     I2COUT
                tst     I2CERR
                bne     TEFPBX
                lda     #TEFR_PATCHDAT
                jsr     I2COUT
                tst     I2CERR
                bne     TEFPBX
TEFPBLP         lda     ,x+
                jsr     I2COUT
                tst     I2CERR
                bne     TEFPBX
                dec     TMP
                bne     TEFPBLP
TEFPBX          jsr     I2CSTOP
                rts

; ---- TEFPSTREAM: X = source, Y = total bytes ------------------------
; Splits the blob into TEF_PATCHCHUNK blocks.  Aborts on any NAK.

TEFPSTREAM      cmpy    #0
                beq     TEFPSDN
                cmpy    #TEF_PATCHCHUNK
                blo     TEFPSPART
                lda     #TEF_PATCHCHUNK
                sta     TMP
                leay    -TEF_PATCHCHUNK,y
                bra     TEFPSGO
TEFPSPART       tfr     y,d                     ; a short final block
                stb     TMP
                ldy     #0
TEFPSGO         jsr     TEFPBLK
                tst     I2CERR
                bne     TEFPSDN
                jsr     TEFPBAR
                bra     TEFPSTREAM
TEFPSDN         rts

; ---- TEFPBAR: advance the progress bar every sixteenth block -------
; About 254 blocks for the patch, so the bar reaches roughly sixteen
; cells.

TEFPBAR         pshs    a,b,x
                inc     PATCHPG
                lda     PATCHPG
                anda    #$0F
                bne     TEFPBARX
                lda     PATCHPG
                lsra
                lsra
                lsra
                lsra                            ; block/16 = cell number
                cmpa    #VOLBAR_CELLS
                bhs     TEFPBARX
                tfr     a,b
                addb    #6                      ; bar starts at column 6
                lda     #9                      ; on row 9
                jsr     SCRADDR
                lda     #VDG_BLOCK
                sta     ,x
TEFPBARX        puls    a,b,x,pc

; ---- TEFPATCHLOAD: push the firmware blob -------------------------
; The blob is the patch immediately followed by the lookup table, at
; TEF_PATCH_BASE.  Both sizes come from the generated tefpatch.inc;
; see tools/mkpatch.py.

TEFPATCHLOAD
                if      TEF_PATCH
                clr     PATCHPG
; 6.2 KB at roughly 40 kHz is about a second and a half of solid bus
; traffic with the keyboard not scanned, so say what is happening.
                jsr     CLS
                ldy     #MSGPATCH
                lda     #7
                ldb     #3
                jsr     PUTSAT
                jsr     DRAWHINT                ; CLS above erased it
                clra
                jsr     TEFPSEL                 ; end any previous section
                tst     I2CERR
                bne     TEFPLX
                lda     #TEFPS_PATCH
                jsr     TEFPSEL
                tst     I2CERR
                bne     TEFPLX
                ldx     #TEF_PATCH_BASE
                ldy     #TEF_PATCH_SIZE
                jsr     TEFPSTREAM
                tst     I2CERR
                bne     TEFPLX
                clra
                jsr     TEFPSEL
                lda     #TEFPS_LUT
                jsr     TEFPSEL
                tst     I2CERR
                bne     TEFPLX
                ldx     #TEF_PATCH_BASE+TEF_PATCH_SIZE
                ldy     #TEF_LUT_SIZE
                jsr     TEFPSTREAM
                tst     I2CERR
                bne     TEFPLX
                clra
                jsr     TEFPSEL
TEFPLX
                endif
                rts

; ---- TEFINIT: cold start, boot state to playing.  Z = 1 on success. -
; User Manual 6.2.  The order matters and every step is polled rather
; than delayed, because the manual gives limits, not fixed times.

TEFINIT
                if      EMU
                lda     #1
                sta     PATCHOK
                clr     TEFFAIL
                clr     I2CERR
                orcc    #$04
                rts
                endif
                clr     PATCHOK
                lda     #TEFF_NOACK
                sta     TEFFAIL
                jsr     TEFPROBE                ; is anything on the bus at all
                bne     TEFINFAIL

                jsr     TEFOPSTAT               ; A = status, keep it in A
                ldb     #TEFF_NOREAD
                stb     TEFFAIL
                cmpa    #$FF                    ; answered its address but
                beq     TEFINFAIL               ; returned nothing readable
                cmpa    #TEFS_BOOT
                bne     TEFINACT                ; already past boot state

; Boot state.  The module is alive and talking; it simply has not been
; patched.  Every get returns zero here, so nothing else can be learned
; about it until the firmware blob has been pushed and it has started.
                ldb     #TEFF_BOOT
                stb     TEFFAIL
                if      TEF_PATCH
                jsr     TEFPATCHLOAD
                tst     I2CERR
                bne     TEFINFAIL
                lda     #1
                sta     PATCHOK
                jsr     TEFSTART
                tst     I2CERR
                bne     TEFINFAIL
                ldb     #TEFF_START
                stb     TEFFAIL
                lda     #TEFS_IDLE
                jsr     TEFWAITST
                bne     TEFINFAIL
                else
                bra     TEFINFAIL               ; no patch built in: stuck in boot
                endif

TEFINACT        jsr     TEFOPSTAT
                ldb     #TEFF_NOREAD
                stb     TEFFAIL
                cmpa    #$FF                    ; no answer, not a status
                beq     TEFINFAIL               ; $FF would pass the test below
                cmpa    #TEFS_STANDBY
                bhs     TEFINAUD                ; already active
                ldb     #TEFF_ACT
                stb     TEFFAIL
                jsr     TEFACTIVATE
                tst     I2CERR
                bne     TEFINFAIL
                lda     #TEFS_STANDBY
                jsr     TEFWAITST
                bne     TEFINFAIL

TEFINAUD        clra                            ; the module powers up MUTED
                jsr     TEFMUTE
                clr     TEFFAIL
                clr     I2CERR
                orcc    #$04                    ; Z = 1, success
                rts

TEFINFAIL       andcc   #$FB                    ; Z = 0
                rts

                if      TEF_PATCH
MSGPATCH        fcc     "LOADING TUNER FIRMWARE"
                fcb     0
                endif
