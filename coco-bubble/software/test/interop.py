#!/usr/bin/env python3
"""
interop.py - does ToolShed's decb accept what the ROM will write?

The MOCK geometry the emulator tests use is deliberately tiny, so it
cannot double as a .DSK image.  This script builds a full 161,280 byte
DECB disk image by the same algorithms the ROM's default build uses -
DSKINI's allocation table with the dead zone marked $FE, the empty
directory, and a SAVEd file laid out the way FCBOPENO/FCBFLUSH lay one
out - then asks the reference implementation (ToolShed decb) to read
it back.

What this validates is the FORMAT design: that a standard tool
tolerates the dead-zone marker, agrees about where the directory and
FAT live, walks the granule chains, and extracts the same bytes that
were stored.  It cannot validate the 6809 code itself - that is what the
emulator suite and, ultimately, the hardware are for.

  ./interop.py /path/to/decb
"""

import os
import subprocess
import sys
import tempfile

# The ROM's default (hardware) geometry, from config.inc.
SECLEN = 256
SECTRK = 18
TRKMAX = 35
SECGRAN = 9
DIRTRK = 17
PHYSSEC = 512
GRANTRK = SECTRK // SECGRAN
GRANMX = (TRKMAX - 1) * GRANTRK
DEADGR = 0xFE

IMAGESIZE = TRKMAX * SECTRK * SECLEN          # 161280


def lba(track, sector):
    return track * SECTRK + (sector - 1)


def granule_sectors(g):
    """The (track, sector) pairs of granule g - the ROM's GRAN2TS."""
    track = g // GRANTRK
    if track >= DIRTRK:
        track += 1
    first = (g % GRANTRK) * SECGRAN + 1
    return [(track, first + i) for i in range(SECGRAN)]


def granule_backed(g):
    """The ROM's GRANOK: every sector of the granule inside PHYSSEC."""
    t, s = granule_sectors(g)[-1]
    return lba(t, s) < PHYSSEC


def dskini(image):
    """What the ROM's DSKINI leaves on the media."""
    fat = bytearray(SECLEN)
    for g in range(GRANMX):
        fat[g] = 0xFF if granule_backed(g) else DEADGR
    put_sector(image, DIRTRK, 2, fat)
    for s in range(3, 12):
        put_sector(image, DIRTRK, s, b"\xFF" * SECLEN)


def put_sector(image, track, sector, data):
    off = lba(track, sector) * SECLEN
    image[off:off + SECLEN] = data.ljust(SECLEN, b"\x00")


def get_fat(image):
    off = lba(DIRTRK, 2) * SECLEN
    return bytearray(image[off:off + SECLEN])


def save_file(image, name, ext, ftype, ascflag, content):
    """Create a file the way FCBOPENO + FCBFLUSH + FCBCLOSE do."""
    fat = get_fat(image)

    # split into sectors
    sectors = [content[i:i + SECLEN] for i in range(0, len(content), SECLEN)]
    if not sectors:
        sectors = [b""]
    tail = len(sectors[-1])
    if tail == 0 and len(content):
        tail = SECLEN

    # allocate granules first-free-from-zero, as GRANALLOC does
    need = (len(sectors) + SECGRAN - 1) // SECGRAN or 1
    grans = []
    g = 0
    while len(grans) < need:
        if g >= GRANMX:
            raise RuntimeError("image full")
        if fat[g] == 0xFF:
            grans.append(g)
        g += 1

    # write the data and link the chain
    si = 0
    for gi, g in enumerate(grans):
        spots = granule_sectors(g)
        used = 0
        for t, s in spots:
            if si >= len(sectors):
                break
            put_sector(image, t, s, sectors[si])
            si += 1
            used += 1
        if gi + 1 < len(grans):
            fat[g] = grans[gi + 1]
        else:
            fat[g] = 0xC0 | used
    put_sector(image, DIRTRK, 2, fat)

    # the directory entry, in the first never-used slot
    entry = (name.ljust(8)[:8] + ext.ljust(3)[:3]).encode("ascii")
    entry += bytes([ftype, ascflag, grans[0]])
    entry += (SECLEN if tail == SECLEN else tail).to_bytes(2, "big")
    entry = entry.ljust(32, b"\x00")
    for s in range(3, 12):
        off = lba(DIRTRK, s) * SECLEN
        for e in range(0, SECLEN, 32):
            b0 = image[off + e]
            if b0 in (0x00, 0xFF):
                image[off + e:off + e + 32] = entry
                return
    raise RuntimeError("directory full")


def run(decb, *args):
    r = subprocess.run([decb] + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    decb = sys.argv[1] if len(sys.argv) > 1 else "decb"
    failures = []

    image = bytearray(b"\x00" * IMAGESIZE)
    dskini(image)

    # a crunched-BASIC-shaped file: $FF, 2-byte length, then payload,
    # sized to land exactly on a granule boundary (the nasty case)
    payload = bytes(range(256)) * 9 * 2            # exactly 2 granules
    body = b"\xFF" + (len(payload)).to_bytes(2, "big") + payload
    body = body[:2304 * 2]                         # keep the boundary
    save_file(image, "BOUND", "BAS", 0, 0, body)

    # and an ordinary small ASCII file
    text = b"10 PRINT\"INTEROP\"\r"
    save_file(image, "SMALL", "BAS", 0, 0xFF, text)

    with tempfile.TemporaryDirectory() as td:
        dsk = os.path.join(td, "bubble.dsk")
        with open(dsk, "wb") as f:
            f.write(image)

        rc, out = run(decb, "dir", dsk)
        print("--- decb dir ---\n" + out.strip())
        if rc != 0 or "BOUND" not in out or "SMALL" not in out:
            failures.append("decb dir does not show the files")

        rc, out = run(decb, "free", dsk)
        print("--- decb free ---\n" + out.strip())
        # 54 usable granules minus 2 (BOUND) minus 1 (SMALL) = 51
        if "51" not in out:
            failures.append("decb free disagrees about free granules (want 51)")

        for fname, want in (("BOUND.BAS", body), ("SMALL.BAS", text)):
            outfile = os.path.join(td, "out.bin")
            rc, out = run(decb, "copy", "%s,%s" % (dsk, fname), outfile)
            if rc != 0:
                failures.append("decb copy %s failed: %s" % (fname, out.strip()))
                continue
            got = open(outfile, "rb").read()
            if got != want:
                failures.append("%s: read back %d bytes, wrote %d, %s"
                                % (fname, len(got), len(want),
                                   "content differs" if len(got) == len(want)
                                   else "length differs"))
            os.remove(outfile)

    print()
    if failures:
        for f in failures:
            print("FAILED:", f)
        return 1
    print("interop: ToolShed decb reads the bubble image correctly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
