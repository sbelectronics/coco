; ===================================================================
; equates.inc - every address, bit and constant, defined exactly once.
;
; CoCo AM/FM radio cartridge, TEF6686 module version.
; ===================================================================

; ---------------- build options ----------------
; Each may be overridden on the asl command line with -D NAME=01.

                ifndef  EMU
EMU             equ     0               ; 1 = emulator build (see below)
                endif
                ifndef  TEF_PATCH
TEF_PATCH       equ     0               ; 1 = emit the module firmware blob
                endif
; The blob is NXP/Catena copyrighted and is not distributed with this
; source.  tools/mkpatch.py extracts it into rom/tefpatch.inc and
; rom/tefpatch.bin; the Makefile runs it and builds with TEF_PATCH=1 by
; default.  Without the patch the ROM still does the whole digital
; bring-up, but the module stays in boot state and will not tune.
                ifndef  EE_SDP
EE_SDP          equ     0               ; must stay 0, see ramcode.asm
                endif
; Build stamp from the Makefile, shown on the main, test and About
; screens so the image on the bench can be matched to the image built.
                ifndef  BUILDID
BUILDID         equ     $0000
                endif
                ifndef  DEVMPI
DEVMPI          equ     0               ; 1 = Multi-Pak development build
                endif
                ifndef  MPI_SCS_SLOT
MPI_SCS_SLOT    equ     4               ; PHYSICAL slot 1..4 holding the radio
                endif

; DEVMPI=1 is for bench work with the radio cartridge and a multicart in
; the same Multi-Pak.  The ROM under test runs from the MULTICART, so CTS
; stays wherever it already points, and only SCS is forced to the slot
; holding the radio so that $FF5E/$FF5F and the $FF40 window reach real
; hardware.  Consequences:
;
;   1. Reads of $C000-$DFFF come from the CTS slot, the multicart, while
;      an armed write lands on the radio cartridge (the write decode uses
;      neither CTS nor SCS).  A save would write one chip and verify
;      against another, so the preset store is RAM-only in this build.
;   2. Loading the store reads the multicart's $DF00, which is this
;      image's own default store.
;   3. The $FF40 scratch window is decoded through SCS, so its write and
;      its read-back both reach the radio.  It is the only EEPROM path
;      that can be exercised in this configuration.
;
; The slot register is saved at start-up and restored on the way out, so
; a disk controller in another slot is reachable again after BREAK.

                if      DEVMPI
                if      MPI_SCS_SLOT<1
                fatal   "MPI_SCS_SLOT is the physical slot number and must be 1..4"
                endif
                if      MPI_SCS_SLOT>4
                fatal   "MPI_SCS_SLOT is the physical slot number and must be 1..4"
                endif
                endif

; ---------------- firmware version ----------------
; Shown on the About page and stored in the ROM header at image offsets
; 12-13, where tools/checkrom.py reads it.  Each is 0..255, shown in
; decimal.
VER_MAJOR       equ     1
VER_MINOR       equ     0

; Header flags byte, image offset 15.  checkrom.py warns on bit 0 so a
; development image cannot be mistaken for a production one.
ROMF_DEVMPI     equ     $01
                if      DEVMPI
ROM_FLAGS       equ     ROMF_DEVMPI
                else
ROM_FLAGS       equ     0
                endif

; EMU=1: skip the STATUS ID check, skip the module probe and patch, make
; every I2C transaction report success without touching the bus, and
; return a canned strong stereo signal.  For emulator UI work only; never
; program a chip with it.

; ---------------- CoCo hardware ----------------
PIA0DA          equ     $FF00           ; keyboard rows in (0 = pressed)
PIA0CRA         equ     $FF01
PIA0DB          equ     $FF02           ; keyboard columns out
PIA0CRB         equ     $FF03           ; bit 7 = VSYNC flag (our tick)
PIA1DA          equ     $FF20
PIA1CRA         equ     $FF21
PIA1DB          equ     $FF22           ; VDG mode bits
PIA1CRB         equ     $FF23           ; CB1 = CART* FIRQ enable

; SAM: a write to the even address clears the bit, odd sets it.
SAMV0C          equ     $FFC0
SAMV1C          equ     $FFC2
SAMV2C          equ     $FFC4
SAMF0C          equ     $FFC6
SAMF1S          equ     $FFC9
SAMF2C          equ     $FFCA
SAMF3C          equ     $FFCC
SAMF4C          equ     $FFCE
SAMF5C          equ     $FFD0
SAMF6C          equ     $FFD2
SAMR0C          equ     $FFD6           ; normal speed
SAMR1C          equ     $FFD8

MPIREG          equ     $FF7F           ; Multi-Pak slot register

RSTFLG          equ     $0071           ; BASIC warm-start flag
RSTVEC          equ     $0072           ; BASIC warm-start vector
FIRQVEC         equ     $FFF6           ; points at the RAM FIRQ hook

; ---------------- cartridge registers ----------------
; Two addresses, one direction each.
CTLREG          equ     $FF5E           ; write only, '273, cleared by RESET*
STATREG         equ     $FF5F           ; read only, '244

CTL_SCL         equ     $01             ; I2C clock   1 = released, 0 = low
CTL_SDA         equ     $02             ; I2C data    1 = released, 0 = low
CTL_ARM         equ     $80             ; EEPROM write enable, RAM code only
CTL_IDLE        equ     CTL_SCL|CTL_SDA ; both lines released = I2C idle

ST_SDA          equ     $01             ; I2C data in, at the module pin
ST_IDMASK       equ     $F0
ST_IDVAL        equ     $B0             ; board ID nibble, set by solder jumpers

; ---------------- EEPROM windows ----------------
; The chip is an AT28C256 with A14 grounded, so it answers as 16K and
; sees CPU A13..A0.
EEBASE          equ     $C000           ; chip $0000
STORE_ROM       equ     $DF00           ; chip $1F00, the preset store
; Armed writes are decoded for $C000-$DFFF (SLENB* driven) and for
; $FF40-$FF5D (no SLENB*).  The latter is chip $3F40-$3F5D: 30 bytes of
; scratch that nothing else can address, and not an alias of the store.
SCRATCH_WIN     equ     $FF40
SCRATCH_LEN     equ     30

; ---------------- RAM map ----------------
SCREEN          equ     $0400           ; 32 x 16 text
SCREEN_LEN      equ     512
SCRCOLS         equ     32
SCRROWS         equ     16

RAMCODE         equ     $0600           ; EEPROM save routine, copied at start
FIRQSTUB        equ     $06C0           ; 6-byte FIRQ handler for the exit path
STORE_RAM       equ     $0800           ; working copy of the preset store
STACKTOP        equ     $1000

; ---------------- variables ($0700 page) ----------------
; The whole page is zeroed by START before anything reads it.  Do not
; rely on a variable being set somewhere else: several are written only
; on one branch, and this RAM is arbitrary at power-on and holds the
; previous run's values after EXEC &HC000.
VARBASE         equ     $0700
VARPAGE_LEN     equ     256

CTLSHD          equ     $0700           ; 1  shadow of the CTL latch
STATION         equ     $0701           ; 2  current station word
BAND            equ     $0703           ; 1  0 = AM, 1 = FM
REGION          equ     $0704           ; 1  0 = US, 1 = EU
VOLUME          equ     $0705           ; 1  0..99, stored in the preset store
MONOFLG         equ     $0706           ; 1  0/1 forced mono
MUTEFLG         equ     $0707           ; 1  0/1 user mute
I2CERR          equ     $0708           ; 1  0 ok, 1 NAK
PATCHOK         equ     $0709           ; 1  1 = module patched this boot
LEVEL           equ     $070A           ; 1  last signal level read back
TICKS           equ     $070B           ; 2  VSYNC tick counter, wraps
DIRTYHDR        equ     $070D           ; 1  1 = header page needs saving
SETTLE          equ     $070E           ; 2  TICKS at the last change
KEYCUR          equ     $0710           ; 1  key code currently down
KEYPREV         equ     $0711           ; 1  previous scan's key code
KEYHOLD         equ     $0712           ; 1  ticks the current key held
KEYEVT          equ     $0713           ; 1  key event this tick, 0 = none
SHIFTFLG        equ     $0714           ; 1  1 = SHIFT down last scan
UIMODE          equ     $0715           ; 1  see UI modes below
EDITBUF         equ     $0716           ; 9  edit buffer + terminator
EDITPOS         equ     $071F           ; 1  cursor index into EDITBUF
EDITSLOT        equ     $0720           ; 1  preset being named, 1..12
PENDING         equ     $0721           ; 1  pending prefix key, 0 none
MSGTICK         equ     $0722           ; 2  tick when the message expires
DIVTMP          equ     $0724           ; 4  divide / BCD scratch
STEREOFLG       equ     $0728           ; 1  1 = stereo pilot
MPIFLG          equ     $0729           ; 1  1 = a Multi-Pak answered
EEFAIL          equ     $072A           ; 1  0 ok, 1 verify failed, 2 timeout, 3 refused
ALTSTATION      equ     $072B           ; 2  last station on the other band
TMP             equ     $072D           ; 1  scratch
I2CACC          equ     $072E           ; 1  byte being assembled by I2CIN
I2CADDR         equ     $072F           ; 1  module write address in use
I2CBUF          equ     $0730           ; 16 I2C scratch; the quality read
I2CBUF_LEN      equ     16              ;    alone is 14 bytes
KEYDOWN         equ     $0740           ; 1  key currently held, 0 = none
DIVSOR          equ     $0741           ; 1  divisor for DIV16X8
NUMBUF          equ     $0742           ; 6  BCD16 digit buffer
SEEKDIR         equ     $0748           ; 1  +1 / -1 while seeking
SEEKFLG         equ     $0749           ; 1  1 = a seek is running
SEEKSTART       equ     $074A           ; 2  station word the seek began at
QOFFSET         equ     $074C           ; 2  tuning offset, signed 0.1 kHz
QUSN            equ     $074E           ; 1  ultrasonic noise, 0..100
PATCHPG         equ     $074F           ; 1  patch progress counter
DPNUM           equ     $0750           ; 1  preset being drawn
DPROW           equ     $0751           ; 1  its screen row
DPCOL           equ     $0752           ; 1  its screen column
MPISAVE         equ     $0753           ; 1  $FF7F as found, for the exit
TEFFAIL         equ     $0754           ; 1  why module bring-up failed
WATCHFLG        equ     $0755           ; 1  1 = test screen polls the gains
WATCHN          equ     $0756           ; 1  watch pass counter, proves liveness
AGCW            equ     $0757           ; 4  Get_AGC: input_att, feedback_att
PROCW           equ     $075B           ; 8  Get_Processing_Status: softmute,
                                        ;    highcut, stereo, sthiblend
WATCHQ          equ     $0763           ; 6  raw level, usn, wam words
; $0769-$07FF free

; ---------------- UI modes ----------------
UI_MAIN         equ     0
UI_NAME         equ     1
UI_FREQ         equ     2
UI_HELP         equ     3
UI_TEST         equ     4
UI_ABOUT        equ     5

; ---------------- key codes ----------------
K_BREAK         equ     $03
K_RIGHT         equ     $09
K_LEFT          equ     $08
K_DOWN          equ     $0A
K_CLEAR         equ     $0C
K_ENTER         equ     $0D
K_SPACE         equ     $20
K_UP            equ     $5E

KEY_REPDELAY    equ     30              ; ticks before the first repeat
KEY_REPRELOAD   equ     26              ; so later repeats come 4 ticks apart

; ---------------- preset store ----------------
; Four 64-byte pages at $DF00.  Header page, then 12 records of 16 bytes.
; NUMERIC, not character literals.  This assembler CONCATENATES adjacent
; character literals joined with "+", so 'R'+'D' evaluates to the two-byte
; string "RD" = $5244 rather than to 150, which silently corrupts any
; checksum expression they appear in.
ST_MAGIC0       equ     $52             ; the letter R
ST_MAGIC1       equ     $44             ; the letter D
ST_VERSION      equ     1
ST_PAGELEN      equ     64
ST_RECLEN       equ     16
ST_NPRESET      equ     12
ST_RECBASE      equ     $40             ; offset of preset 1 within the store
; Names are seven characters.  The record reserves eight bytes for the
; name; the eighth is never shown.  Seven is what lets a preset line hold
; a decimal FM frequency and still leave a blank column before the
; right-hand half of the screen.
ST_NAMEFLD      equ     8               ; bytes reserved in the record
ST_NAMELEN      equ     7               ; characters shown and edited

; header page field offsets, from the base of the store
STH_MAGIC0      equ     0
STH_MAGIC1      equ     1
STH_VERSION     equ     2
STH_FLAGS       equ     3               ; bit 7 region, bit 6 mono
STH_STATION     equ     4               ; 2
STH_VOLUME      equ     6
STH_CKSUM       equ     63

STF_REGION      equ     $80
STF_MONO        equ     $40

; preset record field offsets
STR_STATION     equ     0               ; 2
STR_NAME        equ     2               ; 8
STR_CKSUM       equ     15

; ---------------- band model ----------------
; Station word: bit 15 = 1 FM, 0 AM.  FM bits 14..0 in 50 kHz units,
; AM bits 14..0 in kHz.
BANDBIT         equ     $8000
BAND_AM         equ     0
BAND_FM         equ     1

REGION_US       equ     0
REGION_EU       equ     1

; FM, 50 kHz units.
FM_US_LO        equ     1758            ;  87.9 MHz
FM_US_HI        equ     2158            ; 107.9 MHz
FM_US_STEP      equ     4               ; 200 kHz
FM_EU_LO        equ     1750            ;  87.5 MHz
FM_EU_HI        equ     2160            ; 108.0 MHz
FM_EU_STEP      equ     2               ; 100 kHz

; AM, kHz.  The US ceiling is 1620, not 1710: the module stops there.
AM_US_LO        equ     530
AM_US_HI        equ     1620
AM_US_STEP      equ     10
AM_EU_LO        equ     531
AM_EU_HI        equ     1602
AM_EU_STEP      equ     9

; ---------------- defaults ----------------
DEF_STATION     equ     $879A           ; FM 97.3 MHz (1946 x 50 kHz | bit 15)
DEF_VOLUME      equ     70              ; -12 dB: audible, well under clipping
DEF_REGION      equ     REGION_US
DEF_MONO        equ     0
; ALTSTATION is session state, not stored, so B needs somewhere to go on
; the band the set did not come up on.
DEF_AM_STATION  equ     1000            ; 1000 kHz, no band bit

; ---------------- I2C / module ----------------
; 7-bit address 0x64.  The alternate 0x65 ($CA/$CB) is selected by the
; module's GPIO_2 level at power-up; START probes both.
I2C_ADDR_W      equ     $C8
I2C_ADDR_R      equ     $C9
I2C_ALT_W       equ     $CA
I2C_ALT_R       equ     $CB

I2CERR_OK       equ     0
I2CERR_NAK      equ     1

; ---------------- TEF668X host interface ----------------
; NXP TEF668X User Manual rev 1.6.  Frame shape is
;   module, command, index, then 16-bit parameters MSB byte first.
; Index is ALWAYS 1: /V102 silicon supports no other value, and it also
; requires that every parameter a command defines is actually sent.
; Big-endian suits the 6809, where STD already stores high byte first.

TEFM_FM         equ     $20             ; module 32
TEFM_AM         equ     $21             ; module 33
TEFM_AUDIO      equ     $30             ; module 48
TEFM_APPL       equ     $40             ; module 64
TEF_INDEX       equ     $01

TEFC_TUNE       equ     1               ; FM/AM  Tune_To (mode, frequency)
TEFC_QUALITY    equ     128             ; FM/AM  Get_Quality_Status, 14 bytes back
TEFC_SIGNAL     equ     133             ; FM     Get_Signal_Status, 2 bytes back
TEFC_AGC        equ     132             ; FM/AM  Get_AGC, 4 bytes back
TEFC_PROCSTAT   equ     134             ; FM/AM  Get_Processing_Status, 12 back
TEFC_STEREOMIN  equ     66              ; FM     Set_Stereo_Min (mode, limit)
TEFC_DEEMPH     equ     31              ; FM     Set_Deemphasis (us x 10)
TEFC_VOLUME     equ     10              ; AUDIO  Set_Volume (signed 0.1 dB)
TEFC_MUTE       equ     11              ; AUDIO  Set_Mute (0 off, 1 on)
TEFC_ACTIVATE   equ     5               ; APPL   Set_Activate (mode)
TEFC_OPSTATUS   equ     128             ; APPL   Get_Operation_Status, 2 bytes
TEFC_IDENT      equ     130             ; APPL   Get_Identification, 6 bytes

; Tune modes.  ONLY Preset and Search enable the radio and permit an
; FM/AM band change; Jump and Check do not, so the first tune after
; activation must be Preset or the set stays in standby.
TEFT_PRESET     equ     1
TEFT_SEARCH     equ     2               ; like Preset but stays muted
TEFT_END        equ     7               ; release a Search mute

; Operation status values
TEFS_BOOT       equ     0               ; needs the firmware patch
TEFS_IDLE       equ     1
TEFS_STANDBY    equ     2               ; active, radio standby
TEFS_FM         equ     3
TEFS_AM         equ     4

; Expected identification: type 9 = Lithio series, variant 14 = TEF6686
TEF_DEVID       equ     $090E

; Why TEFINIT gave up.  Each gets its own message, because "not
; responding" for the later ones sends you hunting a wiring fault that
; is not there.
TEFF_OK         equ     0
TEFF_NOACK      equ     1               ; nothing acknowledged the address
TEFF_NOREAD     equ     2               ; acknowledged, but read back nothing
TEFF_BOOT       equ     3               ; alive and in boot state: needs the patch
TEFF_START      equ     4               ; would not leave boot state
TEFF_ACT        equ     5               ; would not activate

; Raw register writes.  These are NOT module/command/index frames, and
; mixing up the two shapes produces a device that silently ignores you.
TEFR_START      equ     $14             ; then $00,$01 - leave boot state
TEFR_PATCHDAT   equ     $1B             ; then up to N data bytes
TEFR_PATCHSEL   equ     $1C             ; then $00,$00 / $00,$74 / $00,$75
TEFPS_PATCH     equ     $74             ; select the patch target
TEFPS_LUT       equ     $75             ; select the lookup-table target
TEF_PATCHCHUNK  equ     24              ; data bytes per $1B frame, must be even
TEF_PATCH_BASE  equ     $E000           ; patch blob, then the lookup table

; ---------------- volume mapping ----------------
; The module's volume is SIGNED, in 0.1 dB steps, -599..+240.  Anything
; at or below -600 is mute, and NXP warns that above 0 dB can clip.
; The stored VOLUME is 0..99 and maps linearly onto
;     tenths = VOL_MIN_T + VOLUME * VOL_STEP_T
; The top of the range must not clip the LM358 summer on a strong
; station: on a single 5 V rail it clips near 2 Vpp, and the level trimmer
; sits after it.  Measure at the summer output before raising the ceiling.
VOL_MIN_T       equ     -400            ; VOLUME = 0  -> -40.0 dB
VOL_STEP_T      equ     4               ; VOLUME = 99 ->  -4.0 dB
TEF_MUTE_T      equ     -600            ; at or below this the module mutes

; ---------------- signal quality ----------------
; Get_Quality_Status returns seven 16-bit words:
;   0 status  1 level  2 usn/noise  3 wam/co-channel
;   4 offset  5 bandwidth  6 modulation
QW_STATUS       equ     0
QW_LEVEL        equ     2               ; signed, 0.1 dBuV, -200..1200
QW_USN          equ     4               ; FM: 0..1000 = 0..100%
QW_WAM          equ     6
QW_OFFSET       equ     8               ; signed, 0.1 kHz, -1200..1200
QW_BANDWIDTH    equ     10
QW_MODULATION   equ     12
QUALITY_LEN     equ     14

; status[9:0] is a time stamp: 0 means tuning is still in progress and
; every other word is meaningless.  1000 means settled and continuously
; updated.  Reading before that is how a seek ends up stopping on noise.
QSTAT_TIMEMASK  equ     $03FF
QSTAT_SETTLED   equ     1000

; LEVEL is stored as whole dBuV (the word divided by 10), which fits a
; signed byte over the full -20..+120 range.
TUNED_THRESH    equ     30              ; dBuV at or above this = "tuned"
SEEK_USN_MAX    equ     35              ; FM: reject above this % noise
SEEK_OFF_MAX    equ     80              ; reject beyond +/- 8.0 kHz error

; ---------------- module timing (User Manual 6.1, 5.6) ----------------
; Bit-banged at about 40 kHz, one address byte already takes 225 us, so
; the 50 us read-setup rule is satisfied without any added delay.
; The device does not clock-stretch at these speeds.
TEF_TSTART      equ     4               ; ticks to allow boot -> idle  (50 ms)
TEF_TACTIVATE   equ     7               ; ticks to allow idle -> active (100 ms)
TEF_TSETTLE     equ     3               ; ticks after a tune before quality
TEF_POLLMAX     equ     20              ; status polls before giving up

; ---------------- misc ----------------
MSG_SECS        equ     180             ; 3 s at 60 Hz, message dwell
SETTLE_TICKS    equ     300             ; 5 s of no change before a header save
LEVEL_EVERY     equ     15              ; refresh level/stereo 4 times a second
VOLBAR_CELLS    equ     20              ; volume bar width
