#!/usr/bin/env python3
"""
mkdiag - build a 64K / 4-bank BANK16K diagnostic cartridge for the CoCo
multicart.  It exercises the $FF40 bank switch and reports the result of
every step on its own text-screen line, and it CANNOT crash if a bank
switch fails: the test is copied into RAM and run there, it only *reads*
the cart window ($C000-$FEFF), interrupts are masked throughout, and it
ends in a safe halt.

Layout: bank 0 = "DK" + bootstrap + test-blob + fill; banks 1..3 = a
position-varying pattern  value(addr,b) = (addr_hi ^ addr_lo ^ seed[b]).
That pattern makes every bank's data unique (switch detection), varies
with the address (aliasing / integrity detection) and has a precomputable
checksum.  Expected per-bank signatures and checksums for banks 1..3 are
baked into the blob; bank 0 is the running reference.

On real hardware a working board shows every line OK.  Under XRoar (which
does not emulate $FF40 banking) banks 1..3 correctly read as SWFAIL -- a
built-in proof that the failure path is graceful.
"""
import sys

ORG        = 0xC000
ENTRY      = 0xC002          # after "DK"
RAM_CODE   = 0x0E00          # test blob runs here (free RAM at autostart)
STACK      = 0x3F00
SCREEN     = 0x0400
WIN        = 0xC000          # cart window base
WIN_END    = 0xFF00          # first unreadable address ($C000-$FEFF served)
SIG_ADDR   = 0xF800          # signature probe (in every bank's pattern area)
TAG_ADDR   = 0xFE00          # last readable page: per-bank constant "tag"
FF40       = 0xFF40
BANK_BYTES = 0x4000
NBANKS     = 4
SEED       = [0x00, 0x5A, 0xA5, 0xC3]
# The XOR pattern below is sum-invariant (XOR with a constant permutes the
# byte values over this address range), so a plain checksum is identical
# across banks. To make each bank's checksum genuinely distinct, the last
# readable page ($FE00-$FEFF) of every bank is filled with a per-bank
# constant TAG[b]; it shifts the sum by 256*TAG[b]. The pattern-integrity
# scan stops at TAG_ADDR so it only checks the position-varying region.
TAG        = [0x11, 0x22, 0x33, 0x44]
# The first HEADER_LEN bytes are identical in EVERY bank ("DK" + select-
# bank-0). So a RESET from ANY currently-selected bank re-runs autostart:
# the "DK" at $C000 exists in every bank, and its $C002 entry immediately
# writes bank 0 to $FF40 and falls through to the real bootstrap
# continuation (which lives only in bank 0, at offset HEADER_LEN).
HEADER     = bytes([0x44, 0x4B,          # "DK"
                    0x86, 0x00,          # LDA #0
                    0xB7, 0xFF, 0x40])   # STA $FF40 -> select bank 0
HEADER_LEN = len(HEADER)                 # 7


def pattern(addr, b):
    return ((addr >> 8) ^ (addr & 0xFF) ^ SEED[b]) & 0xFF

def exp_sig(b):
    return pattern(SIG_ADDR, b)

def exp_sum(b):
    # matches the runtime CKSUM over $C000-$FEFF for a data bank (1..3):
    # the 7-byte reset header, then pattern up to TAG_ADDR, then the
    # 256-byte per-bank tag page.
    s = sum(HEADER)
    s += sum(pattern(a, b) for a in range(WIN + HEADER_LEN, TAG_ADDR))
    s += (WIN_END - TAG_ADDR) * TAG[b]
    return s & 0xFFFF


def vdg(s):
    """ASCII -> CoCo VDG text codes (green-on-black upper case)."""
    out = bytearray()
    for ch in s:
        c = ord(ch)
        if 0x41 <= c <= 0x5A:   out.append(c - 0x40)   # A-Z -> $01-$1A
        elif 0x20 <= c <= 0x3F: out.append(c)          # space..? incl 0-9 : =
        else:                   out.append(0x2E)        # '.' (incl '@', which is VDG $00)
    return bytes(out)


# --- tiny two-pass 6809 assembler ------------------------------------
class Asm:
    def __init__(self, org):
        self.org = org; self.items = []; self.labels = {}
    def label(self, n):        self.items.append(("label", n))
    def raw(self, *bs):        self.items.append(("raw", bytes(bs)))
    def imm16(self, op, v):    self.items.append(("imm16", (bytes(op), v)))
    def ref(self, op, lbl):    self.items.append(("ref", (bytes(op), lbl)))   # op + label addr
    def rel8(self, op, lbl):   self.items.append(("rel8", (bytes(op), lbl)))
    def fcb(self, *bs):        self.raw(*bs)
    def fdb(self, v):          self.raw((v >> 8) & 0xFF, v & 0xFF)
    def bytes_(self, b):       self.items.append(("raw", bytes(b)))

    def _sz(self, k, p):
        if k == "label": return 0
        if k == "raw":   return len(p)
        if k in ("imm16", "ref"): return len(p[0]) + 2
        if k == "rel8":  return len(p[0]) + 1
        return 0

    def assemble(self):
        addr = self.org
        for k, p in self.items:
            if k == "label": self.labels[p] = addr
            else: addr += self._sz(k, p)
        out = bytearray(); addr = self.org
        for k, p in self.items:
            if k == "label": continue
            if k == "raw": out += p
            elif k == "imm16":
                op, v = p; out += op + bytes([(v >> 8) & 0xFF, v & 0xFF])
            elif k == "ref":
                op, lbl = p; v = self.labels[lbl]
                out += op + bytes([(v >> 8) & 0xFF, v & 0xFF])
            elif k == "rel8":
                op, lbl = p; here = addr + len(op) + 1
                rel = self.labels[lbl] - here
                assert -128 <= rel <= 127, "branch OOR to %s" % lbl
                out += op + bytes([rel & 0xFF])
            addr += self._sz(k, p)
        return bytes(out)


def build_blob():
    a = Asm(RAM_CODE)
    # ---- entry: set stack, clear screen, title ----
    a.imm16(b"\x10\xCE", STACK)                 # LDS #STACK
    a.imm16(b"\x8E", SCREEN)                     # LDX #SCREEN
    a.raw(0x86, 0x60)                            # LDA #$60 (green space)
    a.label("CLS")
    a.raw(0xA7, 0x80)                            # STA ,X+
    a.imm16(b"\x8C", SCREEN + 0x200)            # CMPX #$0600
    a.rel8(b"\x26", "CLS")                       # BNE CLS
    a.imm16(b"\x10\x8E", SCREEN)                # LDY #SCREEN (row 0)
    a.ref(b"\x8E", "S_TITLE")                    # LDX #S_TITLE
    a.ref(b"\xBD", "PRINT")                      # JSR PRINT

    # ---- per-bank tests (0=reference, 1..3 switched) ----
    for b in range(NBANKS):
        row = SCREEN + (2 + b) * 32
        if b:
            a.raw(0x86, b)                        # LDA #b
            a.imm16(b"\xB7", FF40)               # STA $FF40  (command bank via DATA)
            a.imm16(b"\x8E", 0x0200)            # LDX #$200 settle delay
            a.label("SD%d" % b)
            a.raw(0x30, 0x1F)                    # LEAX -1,X
            a.rel8(b"\x26", "SD%d" % b)          # BNE
        a.imm16(b"\xB6", SIG_ADDR)              # LDA SIG_ADDR
        a.ref(b"\xB7", "GOTSIG")                 # STA GOTSIG
        a.ref(b"\xBD", "CKSUM")                  # JSR CKSUM -> SUM
        # decide result -> RESPTR
        if b == 0:
            a.ref(b"\x8E", "S_REF"); a.ref(b"\xBF", "RESPTR")   # LDX #S_REF; STX RESPTR
        else:
            a.ref(b"\xB6", "GOTSIG"); a.ref(b"\xB1", "ESIG0")   # LDA GOTSIG; CMPA ESIG0
            a.rel8(b"\x27", "SWF%d" % b)                        # BEQ swfail (still bank0)
            a.ref(b"\xB6", "GOTSIG"); a.ref(b"\xB1", "ESIG%d" % b)
            a.rel8(b"\x26", "BAD%d" % b)                        # BNE bad
            a.ref(b"\xFC", "SUM"); a.ref(b"\x10\xB3", "ESUM%d" % b)  # LDD SUM; CMPD ESUMb
            a.rel8(b"\x26", "BAD%d" % b)
            a.ref(b"\x8E", "S_OK"); a.ref(b"\xBF", "RESPTR")
            a.rel8(b"\x20", "LN%d" % b)
            a.label("SWF%d" % b); a.ref(b"\x8E", "S_SWF"); a.ref(b"\xBF", "RESPTR")
            a.rel8(b"\x20", "LN%d" % b)
            a.label("BAD%d" % b); a.ref(b"\x8E", "S_BAD"); a.ref(b"\xBF", "RESPTR")
            a.label("LN%d" % b)
        # print: "BK<b> G=xx S=xxxx <res>"
        a.imm16(b"\x10\x8E", row)               # LDY #row
        a.ref(b"\x8E", "S_BK"); a.ref(b"\xBD", "PRINT")
        a.raw(0x86, 0x30 + b); a.raw(0xA7, 0xA0)          # LDA #'b'; STA ,Y+
        a.raw(0x86, 0x20); a.raw(0xA7, 0xA0)              # space
        a.ref(b"\x8E", "S_G"); a.ref(b"\xBD", "PRINT")
        a.ref(b"\xB6", "GOTSIG"); a.ref(b"\xBD", "PRHEX")
        a.raw(0x86, 0x20); a.raw(0xA7, 0xA0)
        a.ref(b"\x8E", "S_S"); a.ref(b"\xBD", "PRINT")
        a.ref(b"\xFC", "SUM"); a.ref(b"\xBD", "PRHX16")
        a.raw(0x86, 0x20); a.raw(0xA7, 0xA0)
        a.ref(b"\xBE", "RESPTR"); a.ref(b"\xBD", "PRINT")

    # ---- return-to-bank-0 ----
    a.raw(0x86, 0x00); a.imm16(b"\xB7", FF40)              # LDA #0; STA $FF40
    a.imm16(b"\x8E", 0x0200); a.label("SDR"); a.raw(0x30, 0x1F); a.rel8(b"\x26", "SDR")
    a.imm16(b"\xB6", SIG_ADDR); a.ref(b"\xB1", "ESIG0")   # LDA sig; CMPA ESIG0
    a.rel8(b"\x27", "RET_OK")
    a.ref(b"\x8E", "S_FAIL"); a.rel8(b"\x20", "RET_P")
    a.label("RET_OK"); a.ref(b"\x8E", "S_OK")
    a.label("RET_P"); a.ref(b"\xBF", "RESPTR")
    a.imm16(b"\x10\x8E", SCREEN + 7 * 32)
    a.ref(b"\x8E", "S_RET"); a.ref(b"\xBD", "PRINT")
    a.ref(b"\xBE", "RESPTR"); a.ref(b"\xBD", "PRINT")

    # ---- per-bank pattern integrity (banks 1..3) ----
    for b in (1, 2, 3):
        row = SCREEN + (7 + b) * 32
        a.raw(0x86, b); a.imm16(b"\xB7", FF40)
        a.imm16(b"\x8E", 0x0200); a.label("PD%d" % b); a.raw(0x30, 0x1F); a.rel8(b"\x26", "PD%d" % b)
        a.imm16(b"\x8E", WIN + HEADER_LEN)                 # LDX #$C007 (skip reset header)
        a.label("PL%d" % b)
        a.ref(b"\xBF", "TMP")                              # STX TMP
        a.ref(b"\xB6", "TMP")                              # LDA TMP (hi)
        a.ref(b"\xB8", "TMP1")                             # EORA TMP+1 (lo)  ($B8 ext)
        a.raw(0x88, SEED[b])                               # EORA #seed
        a.raw(0xA1, 0x80)                                  # CMPA ,X+
        a.rel8(b"\x26", "PM%d" % b)                        # BNE mismatch
        a.imm16(b"\x8C", TAG_ADDR); a.rel8(b"\x26", "PL%d" % b)  # scan pattern region only
        # all match
        a.imm16(b"\x10\x8E", row)
        a.ref(b"\x8E", "S_PAT"); a.ref(b"\xBD", "PRINT")
        a.raw(0x86, 0x30 + b); a.raw(0xA7, 0xA0)
        a.raw(0x86, 0x20); a.raw(0xA7, 0xA0)
        a.ref(b"\x8E", "S_OK"); a.ref(b"\xBD", "PRINT")
        a.rel8(b"\x20", "PE%d" % b)
        # mismatch: show address (X-1)
        a.label("PM%d" % b)
        a.raw(0x30, 0x1F)                                  # LEAX -1,X
        a.ref(b"\xBF", "TMP")                              # STX TMP
        a.imm16(b"\x10\x8E", row)
        a.ref(b"\x8E", "S_PAT"); a.ref(b"\xBD", "PRINT")
        a.raw(0x86, 0x30 + b); a.raw(0xA7, 0xA0)
        a.raw(0x86, 0x20); a.raw(0xA7, 0xA0)
        a.ref(b"\x8E", "S_MAT"); a.ref(b"\xBD", "PRINT")  # "M:"
        a.ref(b"\xFC", "TMP"); a.ref(b"\xBD", "PRHX16")
        a.label("PE%d" % b)

    # ---- aliasing check on bank 1 ($C100 vs $E100) ----
    a.raw(0x86, 0x01); a.imm16(b"\xB7", FF40)
    a.imm16(b"\x8E", 0x0200); a.label("AD"); a.raw(0x30, 0x1F); a.rel8(b"\x26", "AD")
    a.imm16(b"\xB6", 0xC100); a.ref(b"\xB7", "TMP")       # LDA $C100; STA TMP
    a.imm16(b"\xB6", 0xE100); a.ref(b"\xB1", "TMP")       # LDA $E100; CMPA TMP
    a.rel8(b"\x27", "AL_FAIL")                            # BEQ -> aliasing (bad)
    a.ref(b"\x8E", "S_OK"); a.rel8(b"\x20", "AL_P")
    a.label("AL_FAIL"); a.ref(b"\x8E", "S_FAIL")
    a.label("AL_P"); a.ref(b"\xBF", "RESPTR")
    a.imm16(b"\x10\x8E", SCREEN + 11 * 32)
    a.ref(b"\x8E", "S_ALI"); a.ref(b"\xBD", "PRINT")
    a.ref(b"\xBE", "RESPTR"); a.ref(b"\xBD", "PRINT")

    # ---- loopback stress: write a bank #, read that bank's signature
    # back from the cart, verify it matches. Thousands of iterations catch
    # intermittent write/switch glitches. ($FF40 isn't CPU-readable on this
    # board, so the "read-back" is the bank's signature byte at $F800 --
    # a stronger check: it confirms the served DATA actually switched.)
    a.imm16(b"\x8E", 0x0000); a.ref(b"\xBF", "FAILS")    # FAILS = 0
    a.imm16(b"\x10\x8E", 0x2000)                          # LDY #8192 iterations (~2.5 s)
    a.label("LB")
    a.raw(0x1F, 0x20)                                     # TFR Y,D
    a.raw(0xC4, 0x03)                                     # ANDB #3  (bank = Y & 3)
    a.imm16(b"\xF7", FF40)                                # STB $FF40  (write page reg)
    a.raw(0x34, 0x04)                                     # PSHS B
    a.imm16(b"\x8E", 0x0020)                              # LDX #$20 settle (~290us,
    a.label("LBD"); a.raw(0x30, 0x1F); a.rel8(b"\x26", "LBD")  # >>switch time: mismatch=real glitch)
    a.raw(0x35, 0x04)                                     # PULS B
    a.ref(b"\x8E", "ESIG0")                               # LDX #ESIG0
    a.raw(0xA6, 0x85)                                     # LDA B,X  (expected sig[bank])
    a.imm16(b"\xB1", SIG_ADDR)                            # CMPA $F800 (served sig)
    a.rel8(b"\x27", "LBok")
    a.ref(b"\xBE", "FAILS"); a.raw(0x30, 0x01); a.ref(b"\xBF", "FAILS")  # FAILS++
    a.label("LBok")
    a.raw(0x31, 0x3F)                                     # LEAY -1,Y (sets Z)
    a.rel8(b"\x26", "LB")
    # result -> RESPTR (LDD FAILS sets Z)
    a.ref(b"\xFC", "FAILS"); a.rel8(b"\x27", "LP_OK")
    a.ref(b"\x8E", "S_FAIL"); a.rel8(b"\x20", "LP_P")
    a.label("LP_OK"); a.ref(b"\x8E", "S_OK")
    a.label("LP_P"); a.ref(b"\xBF", "RESPTR")
    # print "LOOP f=xxxx <res>"
    a.imm16(b"\x10\x8E", SCREEN + 12 * 32)
    a.ref(b"\x8E", "S_LOOP"); a.ref(b"\xBD", "PRINT")
    a.ref(b"\xFC", "FAILS"); a.ref(b"\xBD", "PRHX16")
    a.raw(0x86, 0x20); a.raw(0xA7, 0xA0)
    a.ref(b"\xBE", "RESPTR"); a.ref(b"\xBD", "PRINT")

    # ---- done + halt ----
    a.imm16(b"\x10\x8E", SCREEN + 14 * 32)
    a.ref(b"\x8E", "S_DONE"); a.ref(b"\xBD", "PRINT")
    a.label("HALT"); a.rel8(b"\x20", "HALT")             # BRA self

    # ================= subroutines =================
    a.label("PRINT")                                     # X=str(0-term), Y=dest
    a.raw(0xA6, 0x80)                                    # LDA ,X+
    a.rel8(b"\x27", "PR_E")                              # BEQ
    a.raw(0xA7, 0xA0)                                    # STA ,Y+
    a.rel8(b"\x20", "PRINT")
    a.label("PR_E"); a.raw(0x39)                         # RTS

    a.label("PRHEX")                                     # A=byte -> 2 hex at ,Y+
    a.raw(0x34, 0x02)                                    # PSHS A
    a.raw(0x44); a.raw(0x44); a.raw(0x44); a.raw(0x44)  # LSRA x4
    a.rel8(b"\x8D", "PRNIB")                             # BSR PRNIB
    a.raw(0x35, 0x02)                                    # PULS A
    a.raw(0x84, 0x0F)                                    # ANDA #$0F
    a.rel8(b"\x8D", "PRNIB")
    a.raw(0x39)
    a.label("PRNIB")
    a.raw(0x81, 0x0A)                                    # CMPA #10
    a.rel8(b"\x25", "PN_D")                              # BLO dig
    a.raw(0x8B, 0xF7)                                    # ADDA #$F7 (A-F -> $01-$06)
    a.raw(0xA7, 0xA0); a.raw(0x39)
    a.label("PN_D")
    a.raw(0x8B, 0x30)                                    # ADDA #$30
    a.raw(0xA7, 0xA0); a.raw(0x39)

    a.label("PRHX16")                                    # D -> 4 hex at ,Y+
    a.raw(0x34, 0x04)                                    # PSHS B
    a.rel8(b"\x8D", "PRHEX")                             # print A (hi)
    a.raw(0x35, 0x02)                                    # PULS A (lo)
    a.rel8(b"\x8D", "PRHEX")
    a.raw(0x39)

    a.label("CKSUM")                                     # sum $C000..$FEFF -> SUM
    a.imm16(b"\x8E", WIN)                                # LDX #$C000
    a.raw(0xCC, 0x00, 0x00)                              # LDD #0
    a.ref(b"\xFD", "SUM")                                # STD SUM
    a.label("CK")
    a.ref(b"\xFC", "SUM")                                # LDD SUM
    a.raw(0xEB, 0x80)                                    # ADDB ,X+
    a.raw(0x89, 0x00)                                    # ADCA #0
    a.ref(b"\xFD", "SUM")                                # STD SUM
    a.imm16(b"\x8C", WIN_END)                            # CMPX #$FF00
    a.rel8(b"\x26", "CK")
    a.raw(0x39)

    # ================= strings =================
    def s(lbl, text):
        a.label(lbl); a.bytes_(vdg(text)); a.raw(0x00)
    s("S_TITLE", "MULTICART BANK DIAGNOSTIC")
    s("S_BK", "BK"); s("S_G", "G="); s("S_S", "S=")
    s("S_OK", "OK"); s("S_SWF", "SWFAIL"); s("S_BAD", "BAD"); s("S_REF", "REF")
    s("S_FAIL", "FAIL")
    s("S_RET", "RET0 "); s("S_PAT", "PAT"); s("S_MAT", "M:")   # avoid '@' (=VDG $00)
    s("S_ALI", "ALIAS "); s("S_LOOP", "LOOP F="); s("S_DONE", "DONE")

    # ================= tables + vars (in RAM after copy) =================
    a.label("ESIG0"); a.fcb(exp_sig(0))
    a.label("ESIG1"); a.fcb(exp_sig(1))
    a.label("ESIG2"); a.fcb(exp_sig(2))
    a.label("ESIG3"); a.fcb(exp_sig(3))
    a.label("ESUM1"); a.fdb(exp_sum(1))
    a.label("ESUM2"); a.fdb(exp_sum(2))
    a.label("ESUM3"); a.fdb(exp_sum(3))
    a.label("GOTSIG"); a.fcb(0)
    a.label("RESPTR"); a.fdb(0)
    a.label("FAILS"); a.fdb(0)
    a.label("SUM"); a.fdb(0)
    a.label("TMP"); a.fcb(0)          # 16-bit; TMP1 is TMP+1
    a.label("TMP1"); a.fcb(0)
    return a.assemble()


def build_bootstrap(blob_rom, blob_len):
    """Runs in place at $C002 (bank 0): select bank 0 (so a reset from any
    bank lands here), copy the blob to RAM, JMP to it. All ops fixed-size."""
    a = Asm(ENTRY)
    a.raw(0x86, 0x00)                          # LDA #0
    a.raw(0xB7, 0xFF, 0x40)                    # STA $FF40  (select bank 0)
    a.raw(0x1A, 0x50)                          # ORCC #$50 (mask I,F)
    # Clear the BASIC warm-start flag so the RESET button cold-boots and
    # re-runs the autostart (otherwise a warm start skips the "DK" check).
    a.raw(0x7F, 0x00, 0x71)                    # CLR $0071
    a.imm16(b"\x8E", blob_rom)                 # LDX #blob_rom (source)
    a.imm16(b"\x10\x8E", RAM_CODE)            # LDY #RAM_CODE (dest)
    a.label("BC")
    a.raw(0xA6, 0x80); a.raw(0xA7, 0xA0)      # LDA ,X+ ; STA ,Y+
    a.imm16(b"\x8C", blob_rom + blob_len)     # CMPX #BEND
    a.rel8(b"\x26", "BC")
    a.imm16(b"\x7E", RAM_CODE)                # JMP RAM_CODE
    return a.assemble()


def main():
    outp = sys.argv[1] if len(sys.argv) > 1 else "diag.rom"
    blob = build_blob()

    boot_len = len(build_bootstrap(0, 0))      # fixed-size; values don't matter
    blob_rom = ORG + 2 + boot_len
    bootb = build_bootstrap(blob_rom, len(blob))

    code_end = 2 + len(bootb) + len(blob)      # bank0 offset where fill begins
    assert SIG_ADDR - WIN >= code_end, "code overruns signature area"

    # ---- lay out 4 banks ----
    img = bytearray()
    for b in range(NBANKS):
        bank = bytearray(pattern(WIN + off, b) for off in range(BANK_BYTES))
        # per-bank tag page ($FE00-$FEFF) -> distinct checksum per bank
        for off in range(TAG_ADDR - WIN, WIN_END - WIN):
            bank[off] = TAG[b]
        if b == 0:
            bank[0:2] = b"DK"
            bank[2:2 + len(bootb)] = bootb
            bank[2 + len(bootb):2 + len(bootb) + len(blob)] = blob
            assert bytes(bank[0:HEADER_LEN]) == HEADER, "bank0 header mismatch"
        else:
            bank[0:HEADER_LEN] = HEADER      # reset-to-bank-0 header
        # offsets $3F00..$3FFF are unreadable ($FF00-$FFFF); leave as pattern/pad
        img += bank

    open(outp, "wb").write(img)
    print("mkdiag: -> %s" % outp)
    print("  boot %d B, blob %d B, code_end $%04X, sig@$%04X"
          % (len(bootb), len(blob), WIN + code_end, SIG_ADDR))
    for b in range(NBANKS):
        print("  bank %d: sig=$%02X  sum=$%04X%s"
              % (b, exp_sig(b), exp_sum(b), "  (ref/has code)" if b == 0 else ""))
    print("  image %d bytes (%d x 16K banks, BANK16K)" % (len(img), NBANKS))


if __name__ == "__main__":
    main()
