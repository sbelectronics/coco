#!/usr/bin/env python3
"""
bas2rom - wrap a Color BASIC program or a cassette image in a 6809 loader
stub to make an autostart cartridge ROM for the TRS-80 Color Computer.

Accepts either:
  * a tokenized BASIC file (.bas), or
  * a cassette image (.cas) holding tokenized BASIC (type 0) or a raw
    machine-language image (type 2 / CLOADM).

Every cart begins with the "DK" autostart signature at $C000 and puts its
entry at $C002, which is what BASIC's reset autostart check requires (and
what the multicart's cold-boot stub, ending in JMP [$FFFE], relies on).

BASIC loader stub:
  1. forces the program's ORIGINAL load address into TXTTAB (so both the
     BASIC line-links and any absolute addresses baked into embedded
     machine language stay valid),
  2. copies the tokenized image into RAM there,
  3. relinks the line-address chain (also computing the program end),
  4. sets VARTAB / ARYTAB / ARYEND just past the program,
  5. points CHARAD at a null so RUN takes no line-number argument,
  6. enables the PIA0 60Hz field-sync interrupt (BASIC's ready-loop does
     this, but a cartridge autostart skips it -- without it any program
     that waits on VSYNC via SYNC hangs forever),
  7. plants the interpreter statement-loop address ($8302) on the stack
     where the RUN handler expects it, then JMPs the RUN handler.

ML loader stub: copy image to its load address, enable the field-sync
interrupt, JMP to the execution address.  ML images larger than one 16K
bank are laid out for the multicart's BANK16K scheme (which romfs auto-
selects for images > 16K): the stub relocates a small switcher into RAM,
then walks the banks (writing each bank number to $FF40) copying every
bank's window to the load address before jumping to exec.

Validated in XRoar: BASIC path against a real disk load of KLENDATH
(identical trace + graphics); DK autostart, the ML stub, and the banked
stub's relocate/run-from-RAM/hand-off control flow all confirmed.  The
$FF40 bank swap itself is multicart-firmware behavior (not emulable in
XRoar); the banked byte layout is verified to reconstruct the payload
exactly, and Zaxxon's payload runs from a real cassette load.

Usage:  bas2rom.py INPUT.{bas,cas,bin} OUTPUT.rom [--load HEX] [--run HEX] [--ml]
          --load HEX   override load address (hex, no 0x)
          --run  HEX   override BASIC RUN handler / ML exec address
          --ml         force machine-language mode (needs --load and --run)
"""
import sys

# --- Color BASIC zero-page pointers + ROM entry (bas13.rom) ------------
P_TXTTAB = 0x19
P_VARTAB = 0x1B
P_ARYTAB = 0x1D
P_ARYEND = 0x1F
# CHARAD is the self-modified operand of the GETNCH "LDA" in the direct
# page (at $A6/$A7, big-endian) -- NOT $A5, which is the LDA opcode itself.
P_CHARAD = 0xA6
RUN_ADDR = 0xAE75          # Color BASIC 1.3 RUN command handler
# RUN's tail does `LDX ,S : JMP ,X`, expecting the interpreter statement
# loop address on the stack (the command dispatcher normally puts it
# there). We enter RUN via a bare JMP, so we plant it ourselves at 0,S
# (RUN's stack is net-zero when it reads ,S). $8302 is the Extended BASIC
# statement-execution loop.  NOTE: $809E is the cold-start/autostart-check
# routine, NOT the loop -- returning there just re-inits BASIC and hangs.
#
# Both entry points are STABLE across the whole Tandy line (verified in
# XRoar by tracing a real RUN under each ROM set):
#   RUN  = $AE75 in Color BASIC 1.0, 1.1, 1.2, 1.3, 1.4
#   loop = $8302 in Extended BASIC 1.0, 1.1, and 2.0 (CoCo 3)
# so one cart runs unmodified on CoCo 1/2/3 -- no version table needed.
# The only requirement is that Extended BASIC is present ($8302 lives in
# the $8000-$9FFF Extended ROM); a Color-BASIC-only machine cannot run it.
LOOP_ADDR = 0x8302
ORG      = 0xC000          # cartridge autostart entry
ROM_SIZE = 0x4000          # pad to a standard 16K cart
SCREEN   = 0x0400          # CoCo text screen (32x16), for loader messages


def vdg(s):
    """ASCII -> CoCo VDG text codes (green-on-black upper case)."""
    out = bytearray()
    for ch in s:
        c = ord(ch)
        if 0x41 <= c <= 0x5A:   out.append(c - 0x40)   # A-Z -> $01-$1A
        elif 0x20 <= c <= 0x3F: out.append(c)          # space..? incl 0-9 - :
        else:                   out.append(0x2E)        # '.' fallback
    return bytes(out)


# --- read a tokenized .BAS, strip the FF+len preamble -----------------
def load_bas(path):
    data = bytearray(open(path, "rb").read())
    if data and data[0] == 0xFF and len(data) >= 3:
        declared = (data[1] << 8) | data[2]
        prog = data[3:]
        if declared != len(prog):
            print("warning: preamble length %d != %d bytes of data"
                  % (declared, len(prog)), file=sys.stderr)
    else:
        prog = data
    if bytes(prog[-2:]) != b"\x00\x00":
        print("warning: program does not end in 00 00 (not tokenized BASIC?)",
              file=sys.stderr)
    return bytes(prog)


# --- read a CoCo .cas cassette: namefile header + data blocks -----------
# Returns dict: name, ftype (0=BASIC,1=data,2=ML), ascii, exec, load, payload.
def load_cas(path):
    d = open(path, "rb").read()
    name = None; ftype = 0; ascii_flag = 0; exec_a = 0; load_a = 0
    payload = bytearray()
    i = 0
    while True:
        j = d.find(0x3C, i)                       # $3C = block sync
        if j < 0 or j + 2 >= len(d):
            break
        btype = d[j + 1]
        blen = d[j + 2]
        data = d[j + 3: j + 3 + blen]
        if btype == 0x00:                         # namefile (header) block
            name = data[0:8].decode("latin1").rstrip()
            ftype = data[8]
            ascii_flag = data[9]
            exec_a = (data[11] << 8) | data[12]
            load_a = (data[13] << 8) | data[14]
        elif btype == 0x01:                       # data block
            payload += data
        elif btype == 0xFF:                       # EOF block
            break
        i = j + 3 + blen + 1                       # skip data + checksum
    if name is None:
        print("error: no namefile block found in cassette", file=sys.stderr)
        sys.exit(1)
    return {"name": name, "ftype": ftype, "ascii": ascii_flag,
            "exec": exec_a, "load": load_a, "payload": bytes(payload)}


# original load address = firstline_link - firstline_length
def orig_load_addr(prog):
    link = (prog[0] << 8) | prog[1]
    j = 4
    while j < len(prog) and prog[j] != 0:
        j += 1
    firstline_len = j + 1                 # include the 00 line terminator
    return link - firstline_len



# --- interrupt state at hand-off --------------------------------------
# HARDWARE-MEASURED, and the two paths genuinely disagree:
#   cassette CLOADM+EXEC : CC=$84 -> IRQ and FIRQ both live
#   multicart cart path  : CART* is held asserted (gated Q) and PIA1 CB1
#                          interrupts are enabled ($FF23=$37), so FIRQ
#                          has a source a tape load simply does not have.
# Copying the tape's CC blindly (ANDCC #$AF) hangs almost every game on
# real hardware; masking F instead runs 8 of 10. So:
#   "mask"    - ANDCC #$EF: leave FIRQ masked. Proven on hardware.
#   "cartoff" - turn the CART* FIRQ source OFF at the PIA (clear CB1
#               interrupt enable, then read PB to drop any latched
#               flag), THEN unmask both. Games that use FIRQ themselves
#               get it live, with no CART storm to drown them.
FIRQ_MODE = "mask"


def _emit_handoff_irq(a):
    """Enable the 60 Hz field-sync IRQ, neutralise the CART FIRQ path,
    then set the interrupt masks."""
    # --- neutralise BASIC's CART-FIRQ handler --------------------------
    # Disassembled from bas13.rom: the FIRQ vector ($FFF6) points at RAM
    # $010F, which BASIC fills with JMP $A0F6:
    #     A0F6  TST $FF23     ; CB1 (CART*) interrupt flag?
    #     A0F9  BMI $A0FC
    #     A0FB  RTI
    #     A0FC  JSR $A7D1 (x2)
    #     A108  CLR <$71      ; clear warm-start flag
    #     A10A  JMP $C000     ; <-- JUMP TO THE CARTRIDGE
    # $C000 holds the "DK" signature ($44 $4B), NOT code: $44 is LSRA and
    # $4B is an UNDEFINED opcode, so the CPU falls into garbage and dies.
    # A tape-loaded game never sees this (nothing asserts CART*), but the
    # multicart drives CART* with gated Q the whole time a cart is
    # selected, so ANY FIRQ -- ours, or one the game unmasks itself -- is
    # fatal. It lands at $C000, two bytes short of our $C002 entry, which
    # is why the loader's restart counter never ticked on a hang.
    # Point the FIRQ vector at an RTI so a spurious CART FIRQ is ignored.
    # This is independent of $FF23, so a game that rewrites the PIA (as
    # Seadragon does, 4x) cannot re-arm the fault. A game wanting its own
    # FIRQ handler just overwrites $010F, exactly as it always could.
    a.raw(0x86, 0x3B)                            # LDA #$3B (RTI)
    a.raw(0xB7, 0x01, 0x0F)                      # STA $010F
    a.raw(0xB6, 0xFF, 0x03)                      # LDA $FF03  (PIA0 CRB)
    a.raw(0x8A, 0x01)                            # ORA #$01   (field-sync IRQ on)
    a.raw(0xB7, 0xFF, 0x03)                      # STA $FF03
    if FIRQ_MODE == "cartoff":
        a.raw(0xB6, 0xFF, 0x23)                  # LDA $FF23  (PIA1 CRB)
        a.raw(0x84, 0xFE)                        # ANDA #$FE  (CB1/CART FIRQ off)
        a.raw(0xB7, 0xFF, 0x23)                  # STA $FF23
        a.raw(0xB6, 0xFF, 0x22)                  # LDA $FF22  (clear latched CB1 flag)
        a.raw(0x1C, 0xAF)                        # ANDCC #$AF (I and F live)
    else:
        a.raw(0x1C, 0xEF)                        # ANDCC #$EF (I live, F masked)


# --- tiny one-purpose 6809 assembler (two-pass, labels + refs) --------
class Asm:
    def __init__(self, org):
        self.org = org
        self.items = []          # (kind, payload)
        self.labels = {}
    def label(self, name):       self.items.append(("label", name))
    def raw(self, *bs):          self.items.append(("raw", bytes(bs)))
    def imm16(self, op, val):    self.items.append(("imm16", (bytes(op), val)))
    def abs16ref(self, op, lbl): self.items.append(("abs16ref", (bytes(op), lbl)))
    def rel8(self, op, lbl):     self.items.append(("rel8", (bytes(op), lbl)))

    def _size(self, kind, payload):
        if kind == "label":    return 0
        if kind == "raw":      return len(payload)
        if kind == "imm16":    return len(payload[0]) + 2
        if kind == "abs16ref": return len(payload[0]) + 2
        if kind == "rel8":     return len(payload[0]) + 1
        return 0

    def assemble(self):
        # pass 1: addresses + labels
        addr = self.org
        for kind, payload in self.items:
            if kind == "label":
                self.labels[payload] = addr
            else:
                addr += self._size(kind, payload)
        # pass 2: emit
        out = bytearray()
        addr = self.org
        for kind, payload in self.items:
            if kind == "label":
                continue
            if kind == "raw":
                out += payload
            elif kind == "imm16":
                op, val = payload
                out += op + bytes([(val >> 8) & 0xFF, val & 0xFF])
            elif kind == "abs16ref":
                op, lbl = payload
                v = self.labels[lbl]
                out += op + bytes([(v >> 8) & 0xFF, v & 0xFF])
            elif kind == "rel8":
                op, lbl = payload
                here = addr + len(op) + 1           # PC after the branch
                rel = self.labels[lbl] - here
                assert -128 <= rel <= 127, "branch out of range to %s" % lbl
                out += op + bytes([rel & 0xFF])
            addr += self._size(kind, payload)
        return bytes(out)


def build_stub(load, prog_len):
    a = Asm(ORG)
    end = load + prog_len                       # copy destination end
    # "DK" autostart signature. On reset BASIC does `CMPX #$444B : CMPX
    # $C000 : LBEQ $C002`, so a cart must begin with 'D','K' and put its
    # entry at $C002. (The multicart's cold-boot stub ends in JMP [$FFFE],
    # i.e. RESET, so real hardware takes exactly this signature path --
    # unlike XRoar's -cart-autorun, which force-jumps $C000.)
    a.raw(0x44, 0x4B)                            # "DK"   ($C000)  entry -> $C002
    # force DP=0 so the direct-page stores below are correct
    a.raw(0x4F)                                  # CLRA
    a.raw(0x1F, 0x8B)                            # TFR A,DP
    a.raw(0x1A, 0x50)                            # ORCC #$50  (mask I,F)
    # TXTTAB = load ; [load-1] = 0
    a.imm16(b"\x8E", load)                       # LDX #load
    a.raw(0x9F, P_TXTTAB)                        # STX  <TXTTAB
    a.imm16(b"\x8E", load - 1)                   # LDX #load-1
    a.raw(0x6F, 0x84)                            # CLR ,X
    # copy prog: X=src(PROG), Y=dst(load), until Y==end
    a.abs16ref(b"\x8E", "PROG")                  # LDX #PROG
    a.imm16(b"\x10\x8E", load)                   # LDY #load
    a.label("COPY")
    a.raw(0xA6, 0x80)                            # LDA ,X+
    a.raw(0xA7, 0xA0)                            # STA ,Y+
    a.imm16(b"\x10\x8C", end)                    # CMPY #end
    a.rel8(b"\x26", "COPY")                      # BNE COPY
    # relink line chain, compute program end -> VARTAB
    a.imm16(b"\x8E", load)                       # LDX #load
    a.label("RL")
    a.raw(0xEC, 0x84)                            # LDD ,X   (next-line ptr)
    a.rel8(b"\x27", "DONE")                      # BEQ DONE (0000 = end)
    a.raw(0x31, 0x04)                            # LEAY 4,X (past link+lineno)
    a.label("SCAN")
    a.raw(0xA6, 0xA0)                            # LDA ,Y+
    a.rel8(b"\x26", "SCAN")                      # BNE SCAN (to line end)
    a.raw(0x10, 0xAF, 0x84)                      # STY ,X   (fixed next-line ptr)
    a.raw(0x1F, 0x21)                            # TFR Y,X
    a.rel8(b"\x20", "RL")                        # BRA RL
    a.label("DONE")
    a.raw(0x30, 0x02)                            # LEAX 2,X (past 00 00 marker)
    a.raw(0x9F, P_VARTAB)                        # STX <VARTAB
    a.raw(0x9F, P_ARYTAB)                        # STX <ARYTAB
    a.raw(0x9F, P_ARYEND)                        # STX <ARYEND
    # CHARAD -> a null pair so RUN sees no line-number argument
    a.abs16ref(b"\x8E", "ZERO")                  # LDX #ZERO
    a.raw(0x9F, P_CHARAD)                        # STX <CHARAD
    # plant the interpreter-loop address where RUN's `LDX ,S` will read it
    a.imm16(b"\x8E", LOOP_ADDR)                  # LDX #LOOP_ADDR
    a.raw(0xAF, 0x60)                            # STX 0,S  (RUN reads ,S here)
    # enable the PIA0 60Hz field-sync (VSYNC) interrupt, exactly as BASIC's
    # ready-loop does.  On a cartridge autostart BASIC never runs that code,
    # so without this the IRQ line never asserts -- and any program that does
    # SYNC/`WAIT for VSYNC` (most ML games) hangs forever.  The RAM IRQ
    # vector ($010C) is already set to BASIC's handler by the reset path.
    _emit_handoff_irq(a)
    a.imm16(b"\x7E", RUN_ADDR)                   # JMP RUN
    a.label("ZERO")
    a.raw(0x00, 0x00, 0x00)                      # nulls (GETNCH pre-increments)
    a.label("PROG")
    return a.assemble()


def build_ml_stub(load, prog_len, exec_addr):
    """Loader for a raw machine-language image (cassette type 2 / CLOADM)
    that fits, with the stub, in a single 16K bank: copy the image to its
    load address, enable the field-sync interrupt, JMP to the exec address."""
    a = Asm(ORG)
    end = load + prog_len
    a.raw(0x44, 0x4B)                            # "DK" autostart sig; entry $C002
    a.raw(0x4F)                                  # CLRA
    a.raw(0x1F, 0x8B)                            # TFR A,DP
    a.raw(0x1A, 0x50)                            # ORCC #$50 (mask I,F)
    a.abs16ref(b"\x8E", "PROG")                  # LDX #PROG (source)
    a.imm16(b"\x10\x8E", load)                   # LDY #load (dest)
    a.label("COPY")
    a.raw(0xA6, 0x80)                            # LDA ,X+
    a.raw(0xA7, 0xA0)                            # STA ,Y+
    a.imm16(b"\x10\x8C", end)                    # CMPY #end
    a.rel8(b"\x26", "COPY")                      # BNE COPY
    _emit_handoff_irq(a)
    a.imm16(b"\x7E", exec_addr)                  # JMP exec_addr
    a.label("PROG")
    return a.assemble()


# --- banked ML image (payload too big for one 16K bank) ----------------
# The multicart maps a 16K bank into the cart window on a write of the bank
# number to $FF40 (romfs auto-picks BANK16K for images > 16K). Only the
# CTS* range $C000-$FEFF (BANK_USABLE bytes) is CPU-readable per bank; the
# top 256 bytes of each bank shadow the $FF00 I/O page and are unreachable.
#
# The stub can't switch banks while running from $C000 (the window -- and
# thus the code -- would swap under it), so it copies a small position-
# independent switcher (PartB) into RAM and runs it there. Layout:
#   bank 0: "DK" + PartA + PartB + payload[0 : chunk0]
#   bank b: payload[...] (up to BANK_USABLE per bank)
# PartA (in place at $C002) relocates PartB to a RAM slot OUTSIDE the
# payload's [load, load+n) range (else the payload copy would overwrite the
# switcher mid-run), copies bank 0's payload chunk to `load`, then jumps to
# PartB, which for each further bank writes the bank number to $FF40 and
# copies that window to RAM, finally enabling the field-sync IRQ and JMPing
# to exec.
BANK_BYTES  = 0x4000
BANK_USABLE = 0x3F00        # $C000-$FEFF readable per bank


def _partB(dest1, banks_bytes, exec_addr, reloc, bank_sums, load, total_len, total_sum):
    """RAM switcher/copier assembled to run at `reloc`. For each bank after
    bank 0 it selects the bank ($FF40), copies its window to the running
    dest while accumulating a 16-bit additive checksum, then compares that
    to the build-time checksum in `bank_sums`; on mismatch it re-selects and
    re-copies the SAME bank. This makes the load robust against an
    occasionally mis-sampled $FF40 bank-select strobe (or a serve read that
    briefly returns the wrong bank): a bad copy is simply retried until the
    bytes in RAM are provably correct, instead of running a corrupt image.

    Two 2-byte scratch words live at the top of the reloc page (past the
    copied stub code): DST = the current bank's dest start (so a retry
    rewinds Y), CK = the running checksum. build_ml_banked asserts the stub
    is short enough to leave that room."""
    a = Asm(reloc)
    # PartB owns a TWO-page (512-byte) RAM slot: code from `reloc`, four
    # scratch words at the top of the second page.
    MP  = reloc + 0x1F6       # scratch: screen ptr for per-retry reload ticks
    RC  = reloc + 0x1F8       # scratch: retry count (nonzero => wait for a key)
    DST = reloc + 0x1FA       # scratch: this bank's dest start (retry rewind)
    CK  = reloc + 0x1FC       # scratch: running 16-bit additive checksum
    def hl(x): return (x >> 8) & 0xFF, x & 0xFF
    ck_hi, ck_lo   = hl(CK)
    dst_hi, dst_lo = hl(DST)
    rc_hi, rc_lo   = hl(RC)
    mp_hi, mp_lo   = hl(MP)
    a.raw(0xCC, 0x00, 0x00)                      # LDD #0
    a.raw(0xFD, rc_hi, rc_lo)                    # STD RC   (retry count = 0)
    a.imm16(b"\xCC", SCREEN + 32)               # LDD #row1 (reload-tick cursor)
    a.raw(0xFD, mp_hi, mp_lo)                    # STD MP
    a.imm16(b"\x10\x8E", dest1)                  # LDY #dest1 (running dest)
    bank = 1
    for idx, nb in enumerate(banks_bytes):
        want = bank_sums[idx] & 0xFFFF
        a.raw(0x10, 0xBF, dst_hi, dst_lo)        # STY DST  (save bank dest start)
        a.label("RT%d" % bank)                    # retry entry
        a.raw(0x86, bank & 0xFF)                 # LDA #bank
        a.raw(0xB7, 0xFF, 0x40)                  # STA $FF40  (select bank)
        a.imm16(b"\x8E", 0x0200)                 # LDX #$200  (settle delay)
        a.label("DLY%d" % bank)
        a.raw(0x30, 0x1F)                        # LEAX -1,X
        a.rel8(b"\x26", "DLY%d" % bank)          # BNE DLY
        a.raw(0x10, 0xBE, dst_hi, dst_lo)        # LDY DST  (rewind dest)
        a.raw(0xCC, 0x00, 0x00)                  # LDD #0
        a.raw(0xFD, ck_hi, ck_lo)               # STD CK   (checksum = 0)
        a.imm16(b"\x8E", 0xC000)                 # LDX #$C000 (bank window)
        a.label("CP%d" % bank)
        a.raw(0xE6, 0x80)                        # LDB ,X+   (src byte)
        a.raw(0xE7, 0xA0)                        # STB ,Y+   (-> dest)
        a.raw(0x4F)                              # CLRA      (D = 00:B)
        a.raw(0xF3, ck_hi, ck_lo)               # ADDD CK
        a.raw(0xFD, ck_hi, ck_lo)               # STD CK
        a.imm16(b"\x8C", 0xC000 + nb)            # CMPX #(C000+nb)
        a.rel8(b"\x26", "CP%d" % bank)           # BNE CP
        a.raw(0xFC, ck_hi, ck_lo)               # LDD CK
        a.raw(0x10, 0x83, (want >> 8) & 0xFF, want & 0xFF)  # CMPD #want
        a.rel8(b"\x27", "OK%d" % bank)           # BEQ OK  (copy verified)
        # bad copy: bump retry count, drop a visible "Rn" tick, re-copy bank
        a.raw(0xFC, rc_hi, rc_lo)               # LDD RC
        a.raw(0xC3, 0x00, 0x01)                 # ADDD #1
        a.raw(0xFD, rc_hi, rc_lo)               # STD RC
        if bank == 1:
            # On the first-ever retry, dump what the firmware actually served
            # for this bank (first 4 bytes copied) to row 3, as "n:xxxxxxxx".
            # Lets us tell "serving bank 0" (44 4B ..) from a real data fault.
            a.raw(0x10, 0x83, 0x00, 0x01)       # CMPD #1 (first retry overall?)
            a.rel8(b"\x26", "TICK%d" % bank)     # BNE TICK
            a.raw(0x10, 0x8E, ((SCREEN + 96) >> 8) & 0xFF, (SCREEN + 96) & 0xFF)  # LDY #row3
            a.raw(0x86, 0x30 + (bank & 0x0F)); a.raw(0xA7, 0xA0)  # LDA #digit; STA ,Y+
            a.raw(0x86, 0x3A); a.raw(0xA7, 0xA0)                  # LDA #':'  ; STA ,Y+
            a.raw(0xBE, dst_hi, dst_lo)         # LDX DST (first copied bytes)
            a.raw(0xC6, 0x04)                   # LDB #4
            a.label("DL%d" % bank)
            a.raw(0xA6, 0x80)                   # LDA ,X+
            a.abs16ref(b"\xBD", "HEXB")        # JSR HEXB (A -> 2 hex at ,Y+)
            a.raw(0x5A)                         # DECB
            a.rel8(b"\x26", "DL%d" % bank)      # BNE DL
            a.label("TICK%d" % bank)
        a.raw(0xBE, mp_hi, mp_lo)               # LDX MP
        a.raw(0x86, 0x12)                       # LDA #'R'  (VDG $12)
        a.raw(0xA7, 0x80)                       # STA ,X+
        a.raw(0x86, 0x30 + (bank & 0x0F))       # LDA #bank digit
        a.raw(0xA7, 0x80)                       # STA ,X+
        a.imm16(b"\x8C", SCREEN + 0x1E0)        # CMPX #(near end of screen)
        a.rel8(b"\x25", "MPOK%d" % bank)         # BLO MPOK (still on screen)
        a.imm16(b"\x8E", SCREEN + 32)           # LDX #row1 (wrap; never march into RAM)
        a.label("MPOK%d" % bank)
        a.raw(0xBF, mp_hi, mp_lo)               # STX MP
        a.rel8(b"\x20", "RT%d" % bank)           # BRA RT (retry whole bank)
        a.label("OK%d" % bank)
        bank += 1
    # If we ever had to reload a bank, show a message and wait for a key so
    # the user can't miss that the copy was flaky (skipped on a clean load).
    a.raw(0xFC, rc_hi, rc_lo)                    # LDD RC
    a.rel8(b"\x27", "NOWAIT")                    # BEQ NOWAIT (no retries)
    a.abs16ref(b"\x8E", "MSGRL")                # LDX #MSGRL
    a.imm16(b"\x10\x8E", SCREEN + 64)           # LDY #row2
    a.label("PRL")
    a.raw(0xA6, 0x80)                            # LDA ,X+
    a.rel8(b"\x27", "PRLX")                      # BEQ PRLX
    a.raw(0xA7, 0xA0)                            # STA ,Y+
    a.rel8(b"\x20", "PRL")                       # BRA PRL
    a.label("PRLX")
    a.label("WK")
    a.raw(0xAD, 0x9F, 0xA0, 0x00)               # JSR [$A000] (POLCAT)
    a.raw(0x4D)                                  # TSTA
    a.rel8(b"\x27", "WK")                        # BEQ WK (loop until a key)
    a.label("NOWAIT")
    # Read-back verify: sum the ENTIRE payload back out of RAM and compare
    # to the build-time total. The per-bank checksums above only prove the
    # bytes READ FROM THE CART were right; this proves the bytes that
    # actually LANDED IN RAM are right (catches a store that never took,
    # or anything that clobbered the image after it was written).
    a.imm16(b"\x8E", load)                       # LDX #load
    a.raw(0xCC, 0x00, 0x00)                      # LDD #0
    a.raw(0xFD, ck_hi, ck_lo)                    # STD CK
    a.label("RVL")
    a.raw(0xE6, 0x80)                            # LDB ,X+
    a.raw(0x4F)                                  # CLRA
    a.raw(0xF3, ck_hi, ck_lo)                    # ADDD CK
    a.raw(0xFD, ck_hi, ck_lo)                    # STD CK
    a.imm16(b"\x8C", (load + total_len) & 0xFFFF)  # CMPX #end
    a.rel8(b"\x26", "RVL")                       # BNE RVL
    a.raw(0xFC, ck_hi, ck_lo)                    # LDD CK
    a.raw(0x10, 0x83, (total_sum >> 8) & 0xFF, total_sum & 0xFF)  # CMPD #total
    a.rel8(b"\x27", "RAMOK")                     # BEQ RAMOK
    # RAM image is WRONG: say so loudly and stop (do not run a bad image).
    a.abs16ref(b"\x8E", "MSGRB")                # LDX #MSGRB
    a.imm16(b"\x10\x8E", SCREEN + 64)           # LDY #row2
    a.label("PRB")
    a.raw(0xA6, 0x80); a.rel8(b"\x27", "PRBX")
    a.raw(0xA7, 0xA0); a.rel8(b"\x20", "PRB")
    a.label("PRBX")
    a.raw(0xB6, ck_hi, ck_lo)                    # LDA CK   (hi byte)
    a.abs16ref(b"\xBD", "HEXB")                 # JSR HEXB
    a.raw(0xB6, ck_hi, (ck_lo + 1) & 0xFF)      # LDA CK+1 (lo byte)
    a.abs16ref(b"\xBD", "HEXB")                 # JSR HEXB
    a.label("RBH")
    a.rel8(b"\x20", "RBH")                       # BRA * (halt: bad image)
    a.label("RAMOK")
    # All banks copied and verified: append "LOADED" to the banner and
    # pause ~0.6 s so it is visible even if the game clears the screen —
    # tells the user the hand-off happened and any hang after this point
    # is the GAME's startup, not the loader.
    a.abs16ref(b"\x8E", "MSGOK")                # LDX #MSGOK
    a.imm16(b"\x10\x8E", SCREEN + 11)           # LDY #row0 col 11
    a.label("POK")
    a.raw(0xA6, 0x80)                            # LDA ,X+
    a.rel8(b"\x27", "POKX")                      # BEQ POKX
    a.raw(0xA7, 0xA0)                            # STA ,Y+
    a.rel8(b"\x20", "POK")                       # BRA POK
    a.label("POKX")
    a.imm16(b"\x8E", 0xFFFF)                    # LDX #$FFFF (~0.6 s pause)
    a.label("PDLY")
    a.raw(0x30, 0x1F)                            # LEAX -1,X
    a.rel8(b"\x26", "PDLY")                      # BNE PDLY
    _emit_handoff_irq(a)
    a.imm16(b"\x7E", exec_addr)                  # JMP exec
    # HEXB: A = byte -> two VDG hex chars at ,Y+ (falls through HEXN twice).
    a.label("HEXB")
    a.raw(0x34, 0x02)                           # PSHS A
    a.raw(0x44); a.raw(0x44); a.raw(0x44); a.raw(0x44)  # LSRA x4 (hi nibble)
    a.rel8(b"\x8D", "HEXN")                      # BSR HEXN
    a.raw(0x35, 0x02)                           # PULS A
    a.raw(0x84, 0x0F)                           # ANDA #$0F (lo nibble; fall into HEXN)
    a.label("HEXN")                              # A = nibble -> VDG char at ,Y+
    a.raw(0x81, 0x0A)                           # CMPA #10
    a.rel8(b"\x25", "HN_D")                      # BLO HN_D (0-9)
    a.raw(0x8B, (0x01 - 0x0A) & 0xFF)           # ADDA #(1-10): A..F -> $01..$06
    a.rel8(b"\x20", "HN_S")                      # BRA HN_S
    a.label("HN_D")
    a.raw(0x8B, 0x30)                           # ADDA #$30: 0..9 -> '0'..'9'
    a.label("HN_S")
    a.raw(0xA7, 0xA0)                           # STA ,Y+
    a.raw(0x39)                                 # RTS
    a.label("MSGRL")
    a.raw(*vdg("RELOAD - PRESS KEY"), 0x00)
    a.label("MSGOK")
    a.raw(*vdg("LOADED RAMOK"), 0x00)
    a.label("MSGRB")
    a.raw(*vdg("RAM BAD "), 0x00)
    return a.assemble()


def _partA(load, partB_rom, partB_len, payload0_rom, reloc):
    # Restart-counter cell, in the gap between PartB's code and its
    # scratch words at reloc+0x1F6. Below `load`, so the payload copy
    # never touches it and it survives a re-entry through $C002.
    CNT_MAGIC = reloc + 0x1F0                    # 2 bytes
    CNT       = reloc + 0x1F2                    # 1 byte
    """Runs in place at $C002 while bank 0 is mapped."""
    a = Asm(ORG + 2)                             # entry at $C002 (after "DK")
    a.raw(0x1A, 0x50)                            # ORCC #$50 (mask I,F)
    # CLEAR the screen before the banner. Redrawing the same text over
    # itself would make a re-entry invisible: if something (a stray FIRQ
    # off the gated-Q CART* line, or a reset) sends us back through
    # $C002, an un-cleared screen looks identical to a single clean run.
    a.imm16(b"\x8E", SCREEN)                     # LDX #SCREEN
    a.raw(0x86, 0x60)                            # LDA #$60 (green space)
    a.label("CLS")
    a.raw(0xA7, 0x80)                            # STA ,X+
    a.imm16(b"\x8C", SCREEN + 0x200)             # CMPX #$0600
    a.rel8(b"\x26", "CLS")                       # BNE CLS
    # Restart counter, top-right of row 0: survives a re-entry because it
    # lives in RAM below the payload (magic-guarded so the first run
    # starts it at 1). "1" = one clean pass; anything higher, or a digit
    # that keeps climbing, means we are being restarted.
    a.raw(0xFC, (CNT_MAGIC >> 8) & 0xFF, CNT_MAGIC & 0xFF)   # LDD magic
    a.raw(0x10, 0x83, 0xC0, 0xC0)                            # CMPD #$C0C0
    a.rel8(b"\x27", "HAVEM")                                 # BEQ HAVEM
    a.raw(0xCC, 0xC0, 0xC0)                                  # LDD #$C0C0
    a.raw(0xFD, (CNT_MAGIC >> 8) & 0xFF, CNT_MAGIC & 0xFF)   # STD magic
    a.raw(0x7F, (CNT >> 8) & 0xFF, CNT & 0xFF)               # CLR CNT
    a.label("HAVEM")
    a.raw(0x7C, (CNT >> 8) & 0xFF, CNT & 0xFF)               # INC CNT
    a.raw(0xB6, (CNT >> 8) & 0xFF, CNT & 0xFF)               # LDA CNT
    a.raw(0x8B, 0x30)                                        # ADDA #'0'
    a.raw(0xB7, ((SCREEN + 31) >> 8) & 0xFF, (SCREEN + 31) & 0xFF)  # STA row0 col31
    # banner: "LOADING..." at the top-left of the text screen (bank 0 is
    # mapped, so the string reads from ROM here).
    a.abs16ref(b"\x8E", "MSGLD")                # LDX #MSGLD
    a.imm16(b"\x10\x8E", SCREEN)               # LDY #SCREEN
    a.label("PLD")
    a.raw(0xA6, 0x80)                            # LDA ,X+
    a.rel8(b"\x27", "PLDX")                      # BEQ PLDX
    a.raw(0xA7, 0xA0)                            # STA ,Y+
    a.rel8(b"\x20", "PLD")                       # BRA PLD
    a.label("PLDX")
    a.imm16(b"\x8E", partB_rom)                  # LDX #partB_rom
    a.imm16(b"\x10\x8E", reloc)                 # LDY #reloc  (switcher dest)
    a.label("CPB")
    a.raw(0xA6, 0x80)                            # LDA ,X+
    a.raw(0xA7, 0xA0)                            # STA ,Y+
    a.imm16(b"\x8C", partB_rom + partB_len)     # CMPX #end
    a.rel8(b"\x26", "CPB")                       # BNE CPB
    a.imm16(b"\x8E", payload0_rom)              # LDX #payload0_rom
    a.imm16(b"\x10\x8E", load)                  # LDY #load
    a.label("CP0")
    a.raw(0xA6, 0x80)                            # LDA ,X+
    a.raw(0xA7, 0xA0)                            # STA ,Y+
    a.imm16(b"\x8C", 0xFF00)                    # CMPX #$FF00 (end of bank 0 window)
    a.rel8(b"\x26", "CP0")                       # BNE CP0
    a.imm16(b"\x7E", reloc)                     # JMP reloc (PartB in RAM)
    a.label("MSGLD")
    a.raw(*vdg("LOADING..."), 0x00)
    return a.assemble()


def build_ml_banked(payload, load, exec_addr):
    n = len(payload)

    # The switcher must run from RAM OUTSIDE the payload's [load, load+n)
    # range; it owns a 512-byte slot (code + scratch words at the top).
    # Prefer the two pages just below load (free RAM at autostart); if
    # load is too low, use the pages just above the payload.
    reloc = load - 0x200
    if reloc < 0x0600:
        reloc = (load + n + 0xFF) & ~0xFF        # page-aligned, just past payload
    if not (0x0600 <= reloc and reloc + 0x200 <= 0x7F00):
        print("error: no safe RAM slot for the bank switcher (load=$%04X, "
              "%d bytes)" % (load, n), file=sys.stderr); sys.exit(1)

    # Operand widths are fixed, so PartA/PartB lengths don't depend on the
    # constant *values* -- only PartB's length depends on the bank count.
    # Find the smallest bank count whose capacity holds the payload.
    def layout(nbanks):
        lb = len(_partB(0, [BANK_USABLE] * (nbanks - 1), exec_addr, reloc,
                        [0] * (nbanks - 1), load, n, 0))
        la = len(_partA(load, 0, lb, 0, reloc))
        code_end = 2 + la + lb                    # "DK" + PartA + PartB
        chunk0   = BANK_USABLE - code_end         # payload bytes in bank 0
        cap      = chunk0 + (nbanks - 1) * BANK_USABLE
        return la, lb, code_end, chunk0, cap

    nbanks = 2
    while True:
        la, lb, code_end, chunk0, cap = layout(nbanks)
        if cap >= n:
            break
        nbanks += 1
        if nbanks > 8:
            print("error: image needs >8 banks (>128K) -- unsupported",
                  file=sys.stderr); sys.exit(1)

    # payload bytes served from each bank after bank 0, plus the build-time
    # 16-bit additive checksum of each bank's payload slice (PartB re-sums
    # the copied bytes against these and retries a bank on mismatch).
    rest = n - chunk0
    off  = chunk0
    banks_bytes = []
    bank_sums   = []
    while rest > 0:
        take = min(BANK_USABLE, rest)
        banks_bytes.append(take)
        bank_sums.append(sum(payload[off:off + take]) & 0xFFFF)
        off  += take
        rest -= take

    dest1       = load + chunk0
    partB_rom   = ORG + 2 + la
    payload0_rom = ORG + code_end
    partA = _partA(load, partB_rom, lb, payload0_rom, reloc)
    partB = _partB(dest1, banks_bytes, exec_addr, reloc, bank_sums, load, n, sum(payload) & 0xFFFF)
    assert len(partA) == la and len(partB) == lb, "stub size drift"
    # PartB keeps four 2-byte scratch words at reloc+0x1F6..0x1FF; the
    # copied stub code (incl. its message strings) must not reach them.
    assert lb <= 0x1F6, "PartB too long for its scratch slot (%d bytes)" % lb

    # assemble the multi-bank image
    img = bytearray(b"\xFF" * (nbanks * BANK_BYTES))
    img[0:2]            = b"DK"
    img[2:2 + la]       = partA
    img[2 + la:code_end] = partB
    img[code_end:code_end + chunk0] = payload[0:chunk0]     # bank 0 tail
    off = chunk0
    for b, take in enumerate(banks_bytes, start=1):
        base = b * BANK_BYTES
        img[base:base + take] = payload[off:off + take]
        off += take
    assert off == n
    return bytes(img), nbanks, len(partA) + len(partB) + 2


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    inp, outp = sys.argv[1], sys.argv[2]
    load = run = None
    force_ml = False
    args = sys.argv[3:]
    for i, x in enumerate(args):
        if x == "--load": load = int(args[i + 1], 16)
        if x == "--run":  run = int(args[i + 1], 16)
        if x == "--ml":   force_ml = True
        if x == "--firq":
            global FIRQ_MODE
            FIRQ_MODE = args[i + 1]
    global RUN_ADDR
    if run is not None:
        RUN_ADDR = run

    # --- figure out what kind of input this is --------------------------
    is_cas = inp.lower().endswith(".cas")
    ml_mode = force_ml
    exec_addr = None
    if is_cas:
        cas = load_cas(inp)
        prog = cas["payload"]
        print("cassette : name=%s type=%d ascii=%d exec=$%04X load=$%04X"
              % (cas["name"], cas["ftype"], cas["ascii"], cas["exec"], cas["load"]))
        if cas["ascii"]:
            print("error: ASCII-format cassette (untokenized). Save the program\n"
                  "       tokenized (CSAVE without the ,A option) and retry.",
                  file=sys.stderr); sys.exit(1)
        if cas["ftype"] == 2:                     # machine-language image
            ml_mode = True
            if load is None: load = cas["load"]
            exec_addr = cas["exec"] if run is None else run
        elif cas["ftype"] == 0:                   # tokenized BASIC
            pass
        else:
            print("error: unsupported cassette file type %d (want 0=BASIC or 2=ML)"
                  % cas["ftype"], file=sys.stderr); sys.exit(1)
    else:
        prog = open(inp, "rb").read() if force_ml else load_bas(inp)

    if ml_mode and exec_addr is None:             # raw --ml input
        exec_addr = run

    # --- build the ROM --------------------------------------------------
    if ml_mode:
        if load is None or exec_addr is None:
            print("error: --ml needs --load and --run (or a type-2 cassette)",
                  file=sys.stderr); sys.exit(1)
        flat = build_ml_stub(load, len(prog), exec_addr)
        if len(flat) + len(prog) <= BANK_USABLE:      # fits one 16K bank
            rom = bytearray(b"\xFF" * ROM_SIZE)
            rom[:len(flat) + len(prog)] = flat + prog
            rom = bytes(rom)
            nbanks, stublen = 1, len(flat)
        else:                                         # spill into banks
            rom, nbanks, stublen = build_ml_banked(prog, load, exec_addr)
        kind = "ML @ exec $%04X" % exec_addr
    else:
        if load is None:
            load = orig_load_addr(prog)           # from the BASIC line links
        stub = build_stub(load, len(prog))
        body = stub + prog
        if len(body) > ROM_SIZE:
            print("error: stub+program (%d) exceeds %d-byte cart"
                  % (len(body), ROM_SIZE), file=sys.stderr); sys.exit(1)
        rom = bytearray(b"\xFF" * ROM_SIZE)
        rom[:len(body)] = body
        rom = bytes(rom)
        nbanks, stublen = 1, len(stub)
        kind = "BASIC (RUN=$%04X)" % RUN_ADDR

    open(outp, "wb").write(rom)

    print("bas2rom: %s -> %s" % (inp, outp))
    print("  program   : %d bytes, load $%04X, end $%04X"
          % (len(prog), load, load + len(prog)))
    print("  loader     : %d bytes, entry $%04X, %s" % (stublen, ORG + 2, kind))
    print("  rom        : %d bytes, %d x 16K bank%s%s"
          % (len(rom), nbanks, "" if nbanks == 1 else "s",
             " (BANK16K)" if nbanks > 1 else ""))


if __name__ == "__main__":
    main()
