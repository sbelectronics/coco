#!/usr/bin/env python3
"""Pull an MSA out of a CoCo XT drive image as an ordinary .DSK file.

Input is the sector dump that mfm_util produces from a Gesswein .emu image:

    mfm_util -m drive.emu --format Intel_iSBC_214_512B --sectors 17,0 \\
      --heads 4 --cylinders 615 --header_crc 0xffff,0x1021,16,0 \\
      --data_crc 0xffffffff,0x140a0445,32,6 --sector_length 512 \\
      --extracted_data_file sectors.bin

Then:

    extract-msa.py sectors.bin --list
    extract-msa.py sectors.bin --msa H0A --out H0A.DSK

Geometry notes: the WD controller formats 17 physical 512-byte sectors per
track, but the B&B driver uses only 16 of them (32 logical 256-byte sectors,
per the device descriptor), packing two logical sectors into each physical
one.  Sector 16 of every track is never touched.
"""
import argparse
import sys

SPT_PHYS = 17     # what the controller formatted
HEADS = 4
SPT_DRV = 16      # what the driver actually uses


class Drive:
    def __init__(self, path):
        self.f = open(path, 'rb').read()

    def lsn(self, start, count=1):
        out = bytearray()
        for k in range(count):
            idx = start + k
            phys, half = divmod(idx, 2)
            track, sec = divmod(phys, SPT_DRV)
            cyl, head = divmod(track, HEADS)
            off = (cyl * SPT_PHYS * HEADS + head * SPT_PHYS + sec) * 512 + half * 256
            chunk = self.f[off:off + 256]
            if len(chunk) != 256:
                raise IOError('LSN %d is past the end of the image' % idx)
            out += chunk
        return bytes(out)


def u24(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


def u32(b, o):
    return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]


def seglist(fd):
    """Segment list from an RBF file descriptor: [(start_lsn, sectors), ...]"""
    segs = []
    for i in range(48):
        o = 16 + i * 5
        if o + 5 > len(fd):
            break
        start = u24(fd, o)
        n = (fd[o + 3] << 8) | fd[o + 4]
        if start == 0 and n == 0:
            break
        segs.append((start, n))
    return segs


def rbf_name(entry):
    s = ''
    for c in entry[:29]:
        s += chr(c & 0x7F)
        if c & 0x80:
            break
    return s.strip()


def walk_dir(drive, fd_lsn):
    """Yield (name, fd_lsn) for each entry of a directory.

    Only the first `size` bytes of the directory file hold real entries; the
    rest of the allocated sectors is still low-level format filler ($E6), so
    honour the size field or you "find" garbage files.
    """
    fd = drive.lsn(fd_lsn)
    remaining = u32(fd, 9)
    for start, n in seglist(fd):
        if remaining <= 0:
            break
        data = drive.lsn(start, n)[:remaining]
        remaining -= len(data)
        for i in range(0, len(data) - 31, 32):
            ent = data[i:i + 32]
            if ent[0] == 0:
                continue
            name = rbf_name(ent)
            child = u24(ent, 29)
            if child and name not in ('.', '..'):
                yield name, child


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('sectors', help='sector dump from mfm_util')
    p.add_argument('--msa', help='MSA file name, e.g. H0A or H0A.MSA')
    p.add_argument('--out', help='output .DSK path')
    p.add_argument('--list', action='store_true', help='list the drive contents')
    args = p.parse_args()

    d = Drive(args.sectors)
    id0 = d.lsn(0)
    root = u24(id0, 8)
    vol = ''
    for c in id0[31:63]:
        vol += chr(c & 0x7F)
        if c & 0x80:
            break
    print('volume %r, %d logical sectors, root dir FD at LSN %d'
          % (vol.strip(), u24(id0, 0), root))
    if id0[13] != 0xFF or (d.lsn(root)[0] & 0x80) == 0:
        print('warning: LSN 0 or the root FD does not look like an RBF volume',
              file=sys.stderr)

    files = {}
    for name, fd_lsn in walk_dir(d, root):
        fd = d.lsn(fd_lsn)
        if fd[0] & 0x80:                       # directory -> descend one level
            for sub, sub_lsn in walk_dir(d, fd_lsn):
                files['%s/%s' % (name, sub)] = sub_lsn
        else:
            files[name] = fd_lsn

    if args.list or not args.msa:
        print()
        for name in sorted(files):
            fd = d.lsn(files[name])
            print('  %-16s FD@%-6d %8d bytes  segs=%s'
                  % (name, files[name], u32(fd, 9), seglist(fd)))
        if not args.msa:
            return

    want = args.msa.upper()
    if not want.endswith('.MSA'):
        want += '.MSA'
    match = [k for k in files if k.upper().endswith(want)]
    if not match:
        sys.exit('error: %s not found (try --list)' % want)
    key = match[0]
    fd = d.lsn(files[key])
    size = u32(fd, 9)
    data = bytearray()
    for start, n in seglist(fd):
        data += d.lsn(start, n)
    data = bytes(data[:size])
    out = args.out or (want.replace('.MSA', '.DSK'))
    open(out, 'wb').write(data)
    print()
    print('extracted %s -> %s (%d bytes, %d sectors, %d tracks of 18)'
          % (key, out, len(data), len(data) // 256, len(data) // 256 // 18))


if __name__ == '__main__':
    main()
