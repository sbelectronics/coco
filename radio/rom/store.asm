; ===================================================================
; store.asm - the 256-byte preset store at $DF00.
;
; Four 64-byte pages: a header page, then twelve 16-byte records.  All
; edits happen in the RAM copy at $0800; the EEPROM is written from it,
; a page or a record at a time, by the RAM-resident routine.
;
; Checksum is the 8-bit two's-complement negative of the sum of the
; preceding bytes, so a valid block sums to zero including its checksum.
; ===================================================================

; ---- STSUM: X = pointer, B = length -> A = sum, X advanced ---------

STSUM           clra
STSUMLP         adda    ,x+
                decb
                bne     STSUMLP
                rts

; ---- STCK: X = block, B = length.  Compute and store the checksum. --
; The checksum byte is the last of the block, so summing length-1 bytes
; leaves X pointing exactly at it.
; A IS PRESERVED: STSAVEREC still needs the preset number afterwards, and
; a record address computed from the checksum byte would be an armed
; write somewhere in the ROM's own code.
STCK            pshs    a,x,b
                decb
                jsr     STSUM
                nega
                sta     ,x
                puls    a,x,b,pc

; ---- STVAL: X = block, B = length.  Z = 1 if the block is valid. ----

STVAL           pshs    x,b
                jsr     STSUM
                tsta
                puls    x,b,pc

; ---- STRECADDR: A = preset 1..12 -> X = its record in the RAM copy --

STRECADDR       pshs    a,b
                deca
                ldb     #ST_RECLEN
                mul                             ; D = (n-1) * 16
                addd    #STORE_RAM+ST_RECBASE
                tfr     d,x
                puls    a,b,pc

; ---- STLOAD: copy the store into RAM and validate it ---------------
; A bad header page loads the defaults and marks the page dirty.  A bad
; record simply becomes empty in the RAM copy; it is NOT written back
; until the user stores something there, so a single corrupt record does
; not cost an erase cycle on every boot.

STLOAD          pshs    a,b,x,y
                ldx     #STORE_ROM
                ldy     #STORE_RAM
                clrb                            ; 256 bytes
STLDLP          lda     ,x+
                sta     ,y+
                decb
                bne     STLDLP

                ldx     #STORE_RAM              ; header page valid?
                ldb     #ST_PAGELEN
                jsr     STVAL
                bne     STLDDEF
                lda     STORE_RAM+STH_MAGIC0
                cmpa    #ST_MAGIC0
                bne     STLDDEF
                lda     STORE_RAM+STH_MAGIC1
                cmpa    #ST_MAGIC1
                bne     STLDDEF
                lda     STORE_RAM+STH_VERSION
                cmpa    #ST_VERSION
                bne     STLDDEF
                jsr     STUNPACK
                bra     STLDREC

STLDDEF         jsr     STDEFAULTS
                lda     #1
                sta     DIRTYHDR

STLDREC         lda     #1                      ; check every record
STLDRLP         pshs    a
                jsr     STRECADDR
                ldb     #ST_RECLEN
                jsr     STVAL
                beq     STLDROK
                jsr     STBLANK                 ; X still points at the record
STLDROK         puls    a
                inca
                cmpa    #ST_NPRESET+1
                blo     STLDRLP
                puls    a,b,x,y,pc

; ---- STBLANK: X = record.  Make it an empty preset in RAM. ---------

STBLANK         pshs    a,b,x
                clr     ,x                      ; station word = $0000
                clr     1,x
                leax    STR_NAME,x
                lda     #' '
                ldb     #ST_NAMEFLD             ; all 8 bytes of the field
STBLKLP         sta     ,x+
                decb
                bne     STBLKLP
                ldb     #5                      ; reserved bytes
STBLKRS         clr     ,x+
                decb
                bne     STBLKRS
                puls    a,b,x,pc

; ---- STUNPACK: header page -> the working variables ----------------

STUNPACK        lda     STORE_RAM+STH_FLAGS
                pshs    a
                anda    #STF_REGION
                beq     STUNP0
                lda     #REGION_EU
                bra     STUNP1
STUNP0          lda     #REGION_US
STUNP1          sta     REGION
                puls    a
                anda    #STF_MONO
                beq     STUNP2
                lda     #1
                bra     STUNP3
STUNP2          clra
STUNP3          sta     MONOFLG
                ldd     STORE_RAM+STH_STATION
                std     STATION
                lda     STORE_RAM+STH_VOLUME
                cmpa    #100
                blo     STUNP4
                lda     #DEF_VOLUME             ; a corrupt value would be
STUNP4          sta     VOLUME                  ; unrecoverable at the UI
                jsr     STSETBAND
; MUTEFLG is not restored: the header has no mute bit, so mute is
; session state and always starts off (START zeroes it).
; ALTSTATION is session state too; seed it with a station on the other
; band so B has somewhere sensible to go on the first press.
                tst     BAND
                beq     STUNPAF                 ; on AM now, so the other is FM
                ldd     #DEF_AM_STATION
                bra     STUNPAS
STUNPAF         ldd     #DEF_STATION
STUNPAS         std     ALTSTATION
                rts

; ---- STPACK: the working variables -> the header page --------------

STPACK          lda     #ST_MAGIC0
                sta     STORE_RAM+STH_MAGIC0
                lda     #ST_MAGIC1
                sta     STORE_RAM+STH_MAGIC1
                lda     #ST_VERSION
                sta     STORE_RAM+STH_VERSION
                clra
                tst     REGION
                beq     STPK0
                ora     #STF_REGION
STPK0           tst     MONOFLG
                beq     STPK1
                ora     #STF_MONO
STPK1           sta     STORE_RAM+STH_FLAGS
                ldd     STATION
                std     STORE_RAM+STH_STATION
                lda     VOLUME
                sta     STORE_RAM+STH_VOLUME
                rts

; ---- STSETBAND: derive BAND from the station word ------------------

STSETBAND       lda     STATION                 ; high byte
                bpl     STSBAM
                lda     #BAND_FM
                sta     BAND
                rts
STSBAM          clr     BAND
                rts

; ---- STDEFAULTS: a valid default store in the RAM copy -------------

STDEFAULTS      pshs    a,b,x
                ldx     #STORE_RAM              ; wipe the header page
                ldb     #ST_PAGELEN
STDFHL          clr     ,x+
                decb
                bne     STDFHL
                ldd     #DEF_STATION
                std     STATION
                lda     #DEF_VOLUME
                sta     VOLUME
                lda     #DEF_REGION
                sta     REGION
                lda     #DEF_MONO
                sta     MONOFLG
                clr     MUTEFLG
                ldd     #DEF_AM_STATION         ; the default station is FM, so
                std     ALTSTATION              ; the alternate must be AM or B
                                                ; does nothing after a reset
                jsr     STSETBAND
                jsr     STPACK
                ldx     #STORE_RAM
                ldb     #ST_PAGELEN
                jsr     STCK
                lda     #1                      ; twelve empty presets
STDFRL          pshs    a
                jsr     STRECADDR
                jsr     STBLANK
                ldb     #ST_RECLEN
                jsr     STCK
                puls    a
                inca
                cmpa    #ST_NPRESET+1
                blo     STDFRL
                puls    a,b,x,pc

; ---- STSAVEHDR: write the header page to the EEPROM ----------------

STSAVEHDR       pshs    a,b,x,y
                jsr     STPACK
                ldx     #STORE_RAM
                ldb     #ST_PAGELEN
                jsr     STCK
                if      DEVMPI
; Multi-Pak development build: the ROM runs from the multicart on CTS
; while SCS points at the radio, so an armed write reaches the radio's
; EEPROM but every read-back comes from the multicart, and a save could
; only ever fail verify.  Keep the store in RAM instead.  See equates.inc.
                clr     EEFAIL
                clr     DIRTYHDR
                puls    a,b,x,y,pc
                endif
                ldx     #STORE_RAM
                ldy     #STORE_ROM
                ldb     #ST_PAGELEN
                jsr     EESAVE
                tst     EEFAIL
                bne     STSHDX
                clr     DIRTYHDR
STSHDX          puls    a,b,x,y,pc

; ---- STSAVEREC: A = preset 1..12.  Write that record. --------------
; A record is sixteen bytes at a sixteen-byte offset inside a 64-byte
; page, so it can never straddle a page boundary.

STSAVEREC       pshs    a,b,x,y
                jsr     STRECADDR
                ldb     #ST_RECLEN
                jsr     STCK
                if      DEVMPI
                clr     EEFAIL                  ; RAM-only, see STSAVEHDR
                puls    a,b,x,y,pc
                endif
                lda     ,s                      ; the preset number, from the
                jsr     STRECADDR               ; stack, not from A: see STCK
                tfr     x,d
                subd    #STORE_RAM
                addd    #STORE_ROM
                tfr     d,y
                ldb     #ST_RECLEN
                jsr     EESAVE
                puls    a,b,x,y,pc

; ---- STGETSTA: A = preset 1..12 -> D = its station word ------------

STGETSTA        jsr     STRECADDR
                ldd     ,x
                rts

; ---- STPUTSTA: A = preset 1..12, D on the stack.  Store the tune. ---
; If the record was empty its name becomes dashes; an existing name is
; kept, which is what makes re-storing over a named preset safe.

STPUTSTA        pshs    a
                jsr     STRECADDR
                ldd     ,x
                bne     STPSNAME                ; already has a station
                pshs    x
                leax    STR_NAME,x
                lda     #'-'
                ldb     #ST_NAMELEN
STPSDL          sta     ,x+
                decb
                bne     STPSDL
                puls    x
STPSNAME        ldd     STATION
                std     ,x
                puls    a
                jsr     STSAVEREC
                rts
