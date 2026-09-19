#!/usr/bin/env python3
"""Scramble a ROM image for the CoCo XT's hard-disk-controller BIOS socket.

The CoCo XT interface board inverts CoCo address lines A4, A5 and A6 on their
way to the controller's BIOS ROM socket (App Note 5 section 1.7: "several of
the address lines driving this EPROM are inverted by the CoCo XT interface
board").  A13 and below are otherwise straight through, so the whole transform
is a single XOR of $70 on the ROM's address:

    scrambled[j] = plain[j ^ 0x70]

which is its own inverse, so this script both scrambles and unscrambles.

Input may be a DECB binary (as produced by SAVESYS) or a raw image; DECB
headers are stripped automatically.  Output is a raw binary ready to burn.

    scramble-rom.py HYPERIO.BIN HYPERIO-SCRAMBLED.BIN
    scramble-rom.py --selftest plain.bin scrambled.bin   # verify the transform
"""
import argparse
import struct
import sys

MASK = 0x70          # A4, A5, A6 inverted


def scramble(data, mask=MASK):
    """Reorder bytes so the ROM presents `data` once the board inverts A4-A6."""
    if len(data) & (len(data) - 1):
        raise ValueError('image size %d is not a power of two' % len(data))
    out = bytearray(len(data))
    for j in range(len(data)):
        out[j] = data[j ^ mask]
    return bytes(out)


def read_decb(raw):
    """Return (load_addr, data, exec_addr) from a DECB binary, or None."""
    if not raw or raw[0] != 0x00:
        return None
    segments = []
    i = 0
    exec_addr = None
    while i < len(raw):
        kind = raw[i]
        if kind == 0x00:
            ln, ad = struct.unpack('>HH', raw[i + 1:i + 5])
            segments.append((ad, raw[i + 5:i + 5 + ln]))
            i += 5 + ln
        elif kind == 0xFF:
            exec_addr = struct.unpack('>H', raw[i + 3:i + 5])[0]
            break
        else:
            raise ValueError('bad DECB block type $%02X at offset %d' % (kind, i))
    if len(segments) != 1:
        raise ValueError('expected one data segment, got %d' % len(segments))
    return segments[0][0], segments[0][1], exec_addr


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('input')
    p.add_argument('output')
    p.add_argument('--size', type=lambda s: int(s, 0), default=16384,
                   help='EPROM size in bytes (default 16384 = 27128)')
    p.add_argument('--fill', type=lambda s: int(s, 0), default=0xFF,
                   help='pad byte for unused space (default $FF)')
    p.add_argument('--mask', type=lambda s: int(s, 0), default=MASK,
                   help='address bits to invert (default $70 = A4,A5,A6)')
    p.add_argument('--selftest', metavar='SCRAMBLED_REFERENCE',
                   help='check the transform reproduces a known scrambled ROM')
    args = p.parse_args()

    raw = open(args.input, 'rb').read()
    decb = read_decb(raw)
    if decb:
        load, data, exec_addr = decb
        print('input : DECB binary, loads $%04X-$%04X (%d bytes), exec $%04X'
              % (load, load + len(data) - 1, len(data), exec_addr))
    else:
        load, data, exec_addr = None, raw, None
        print('input : raw image, %d bytes' % len(data))

    if len(data) > args.size:
        sys.exit('error: %d bytes will not fit a %d byte EPROM'
                 % (len(data), args.size))

    plain = data + bytes([args.fill]) * (args.size - len(data))
    if len(plain) != len(data):
        print('pad   : %d bytes of $%02X appended to fill %dK'
              % (len(plain) - len(data), args.fill, args.size // 1024))

    out = scramble(plain, args.mask)

    # The transform must be an involution, and must round-trip the real image.
    assert scramble(out, args.mask) == plain, 'transform is not self-inverse'
    assert all(out[j] == plain[j ^ args.mask] for j in range(len(plain)))
    print('check : self-inverse OK, %d bytes remapped with XOR $%02X'
          % (len(out), args.mask))

    if load is not None:
        sig = bytes(out[args.mask:args.mask + 2])
        print('check : bytes at scrambled offset $%02X = %r  '
              '(what the CoCo reads at $%04X)' % (args.mask, sig, load))

    if args.selftest:
        ref = open(args.selftest, 'rb').read()
        if len(ref) != len(plain):
            print('selftest: size mismatch (%d vs %d), skipped'
                  % (len(ref), len(plain)))
        else:
            ok = scramble(plain, args.mask) == ref
            print('selftest: reproduces %s exactly: %s' % (args.selftest, ok))
            if not ok:
                sys.exit(1)

    open(args.output, 'wb').write(out)
    print('output: %s (%d bytes)' % (args.output, len(out)))


if __name__ == '__main__':
    main()
