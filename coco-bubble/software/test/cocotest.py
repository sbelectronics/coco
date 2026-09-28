#!/usr/bin/env python3
"""
cocotest.py - drive the bubble cartridge ROM under XRoar and read the
emulated screen back, so that ROM behaviour can be checked from a script.

XRoar is started headless with its GDB target enabled.  The machine
boots (optionally with a line of BASIC typed at it); the script then
attaches over the GDB remote protocol, halts the CPU and reads the 32x16
text screen out of video RAM at $0400.

  ./cocotest.py --rom build/bubble.rom
  ./cocotest.py --rom build/bubble-mock.rom --type 'DIR\r' --wait 4
  ./cocotest.py --rom build/bubble.rom --peek 00F3 16
"""

import argparse
import os
import socket
import subprocess
import sys
import time

XROAR = os.path.expanduser("~/bin/xroar")
GDB_PORT = 65520
VIDRAM = 0x0400
SCREEN_W, SCREEN_H = 32, 16


class GdbClient:
    """Minimal GDB remote protocol client - enough to halt and peek."""

    def __init__(self, port, timeout=10.0):
        deadline = time.time() + timeout
        self.sock = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 1.0)
                break
            except OSError:
                time.sleep(0.2)
        if self.sock is None:
            raise RuntimeError("could not connect to the XRoar GDB target")
        self.sock.settimeout(5.0)

    def _send(self, body):
        csum = sum(body.encode()) & 0xFF
        self.sock.sendall(b"$" + body.encode() + b"#" + b"%02x" % csum)

    def _recv(self):
        # Skip acknowledgements, gather one $...#xx packet.
        buf = b""
        while True:
            ch = self.sock.recv(1)
            if not ch:
                raise RuntimeError("GDB target closed the connection")
            if ch in (b"+", b"-"):
                continue
            if ch == b"$":
                break
        while True:
            ch = self.sock.recv(1)
            if ch == b"#":
                self.sock.recv(2)          # discard the checksum
                break
            buf += ch
        self.sock.sendall(b"+")
        return buf.decode(errors="replace")

    def command(self, body):
        self._send(body)
        return self._recv()

    def interrupt(self):
        """Ask the CPU to stop.

        XRoar does not always send a stop reply, but its memory reads
        work either way, so a missing reply is not fatal.
        """
        self.sock.sendall(b"\x03")
        try:
            return self._recv()
        except (socket.timeout, TimeoutError):
            return ""

    def read_mem(self, addr, length):
        out = bytearray()
        while length:
            chunk = min(length, 128)
            reply = self.command("m%x,%x" % (addr, chunk))
            if reply.startswith("E") or not reply:
                raise RuntimeError("memory read failed at %04X: %r" % (addr, reply))
            out += bytes.fromhex(reply)
            addr += chunk
            length -= chunk
        return bytes(out)

    def close(self):
        try:
            self.command("D")           # detach so the machine keeps running
        except Exception:
            pass
        self.sock.close()


def decode_screen(data):
    """Turn CoCo video RAM into text.

    $00-$1F are '@' and the upper case letters, $20-$3F are ASCII as-is,
    bit 6 marks inverse video (same glyph), bit 7 marks semigraphics.
    """
    lines = []
    for row in range(SCREEN_H):
        chars = []
        for col in range(SCREEN_W):
            b = data[row * SCREEN_W + col]
            if b & 0x80:
                chars.append("#")           # semigraphics block
                continue
            c = b & 0x3F
            chars.append(chr(c + 0x40) if c < 0x20 else chr(c))
        lines.append("".join(chars).rstrip())
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True, help="cartridge ROM image")
    ap.add_argument("--machine", default="coco2b")
    ap.add_argument("--type", dest="typing", default=None,
                    help=r"BASIC to type, e.g. 'DIR\r'")
    ap.add_argument("--wait", type=float, default=3.0,
                    help="seconds to run before reading the screen")
    ap.add_argument("--peek", nargs=2, metavar=("ADDR", "LEN"), default=None,
                    help="also dump LEN bytes of memory from hex ADDR")
    ap.add_argument("--regs", action="store_true",
                    help="also show the CPU registers where it was halted")
    ap.add_argument("--mpi", type=int, default=0, metavar="SLOT",
                    help="put the ROM in Multi-Pak slot SLOT (1-4) instead "
                         "of straight on the cartridge port")
    ap.add_argument("--mpi-cart", action="append", default=[],
                    metavar="SLOT=TYPE",
                    help="also fill an MPI slot, e.g. 4=rsdos for a floppy "
                         "controller.  Repeatable; needs --mpi")
    ap.add_argument("--fd", action="append", default=[], metavar="IMAGE",
                    help="insert a disk image into floppy drive 0")
    ap.add_argument("--cart-type", dest="cart_type", default="rom",
                    help="xroar cartridge type; rsdos gives the ROM a "
                         "Tandy floppy controller in the same cartridge")
    ap.add_argument("--typefile", default=None, metavar="FILE",
                    help="type a whole BASIC listing from FILE before "
                         "anything given with --type")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    # A previous run that was killed can leave an emulator holding the
    # port, which makes the next run mysteriously fail; clear it out and
    # wait for the port to actually come free before starting.
    subprocess.call(["pkill", "-9", "-f", "bin/xroar"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        probe = socket.socket()
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(("127.0.0.1", GDB_PORT))
            probe.close()
            break
        except OSError:
            probe.close()
            time.sleep(0.25)

    cmd = [XROAR, "-machine", args.machine, "-ui", "null", "-ao", "null"]
    if args.mpi:
        # Put the ROM in an MPI slot instead of straight on the port, so
        # the slot detection in mpi.asm has something to find.  XRoar
        # numbers slots 0-3; --mpi takes the 1-4 the hardware is labelled
        # with, to match the boot message and the service manual.
        cmd += ["-cart", "bub", "-cart-type", "rom",
                "-cart-rom", os.path.abspath(args.rom), "-cart-autorun",
                "-cart", "mpi0", "-cart-type", "mpi",
                "-mpi-load-cart", "%d=bub" % (args.mpi - 1)]
        for spec in args.mpi_cart:
            slot, _, name = spec.partition("=")
            cmd += ["-mpi-load-cart", "%d=%s" % (int(slot) - 1, name)]
        # Select the ROM's slot at power-up, the way you would set the
        # front panel switch, so CTS*/CART* land on it and the ROM
        # autostarts.  mpi.asm then moves SCS* on its own.
        cmd += ["-mpi-slot", str(args.mpi - 1), "-machine-cart", "mpi0"]
    else:
        # --cart-type rsdos gives the ROM the Tandy floppy controller
        # hardware in the same cartridge: WD1793 registers, DSKREG and
        # the HALT/NMI transfer path, with no MPI in the way.
        cmd += ["-cart-type", args.cart_type,
                "-cart-rom", os.path.abspath(args.rom), "-cart-autorun"]
    cmd += ["-gdb", "-gdb-port", str(GDB_PORT)]
    for n, image in enumerate(args.fd):
        cmd += ["-load-fd%d" % n, os.path.abspath(image)]
    typing = args.typing.replace("\\r", "\r") if args.typing else ""
    if args.typefile:
        # Feed a whole BASIC listing in.  Blank lines would be bare
        # returns at the prompt, which is harmless but slow, so drop
        # them; everything else goes in verbatim, RUN included.
        with open(args.typefile) as fh:
            lines = [ln.rstrip("\n") for ln in fh if ln.strip()]
        typing = "\r".join(lines) + "\r" + typing
    if typing:
        cmd += ["-type", typing]

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        time.sleep(args.wait)
        gdb = GdbClient(GDB_PORT)
        gdb.interrupt()
        screen = gdb.read_mem(VIDRAM, SCREEN_W * SCREEN_H)
        extra = None
        if args.peek:
            extra = (int(args.peek[0], 16),
                     gdb.read_mem(int(args.peek[0], 16), int(args.peek[1])))
        regs = gdb.command("g") if args.regs else None
        gdb.close()
    finally:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass

    lines = decode_screen(screen)
    if not args.quiet:
        print("+" + "-" * SCREEN_W + "+")
        for line in lines:
            print("|" + line.ljust(SCREEN_W) + "|")
        print("+" + "-" * SCREEN_W + "+")
    if extra:
        addr, data = extra
        for off in range(0, len(data), 16):
            row = data[off:off + 16]
            print("%04X  %s  %s" % (
                addr + off,
                " ".join("%02X" % b for b in row),
                "".join(chr(b) if 32 <= b < 127 else "." for b in row)))
    if regs:
        # XRoar's 6809 'g' packet: CC A B DP X Y U S PC, then some
        # registers it reports as 'x' (unavailable) - trim those off.
        hexpart = "".join(c for c in regs if c in "0123456789abcdefABCDEF")
        raw = bytes.fromhex(hexpart[:28])
        if len(raw) >= 14:
            cc, a, b, dp = raw[0], raw[1], raw[2], raw[3]
            x, y, u, s, pc = (int.from_bytes(raw[i:i + 2], "big")
                              for i in (4, 6, 8, 10, 12))
            print("CC=%02X A=%02X B=%02X DP=%02X X=%04X Y=%04X U=%04X "
                  "S=%04X PC=%04X" % (cc, a, b, dp, x, y, u, s, pc))
        else:
            print("registers: %s" % regs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
