# -*- coding: utf-8 -*-
# Obtain the TEF668X firmware patch and emit the two files the ROM build
# needs.  Python 2.7 or 3.  The Makefile runs this automatically.
#
#   python tools/mkpatch.py rom/                 # fetch if needed
#   python tools/mkpatch.py rom/ path/PATCH.c    # use a local copy
#   python tools/mkpatch.py rom/ --version 205   # later silicon
#
# Writes  rom/tefpatch.bin  the patch blob followed by the lookup table
#         rom/tefpatch.inc  TEF_PATCH_SIZE and TEF_LUT_SIZE
# Both are gitignored and must not be committed.
#
# WHY THIS IS A SCRIPT AND NOT A CHECKED-IN BLOB
# ----------------------------------------------
# The patch is NXP / Catena copyrighted material.  The chip does not work
# without it: the User Manual calls the transfer "a required part of the
# V102 boot state device initialization", and until it is loaded the part
# answers its I2C address and then returns zero to every read.  NXP
# publishes it in that manual and ships it as PATCH.c with its PC GUI
# program.  This project does not redistribute it.
#
# Without a local PATCH.c the script fetches the same bytes from the
# public PE5PVB TEF6686_ESP32 driver source, and writes NXP's copyright
# notice into the generated file.
from __future__ import print_function
import sys, os, re, subprocess

# Upstream fallback.  Prefer a local PATCH.c from NXP if you have one.
FETCH_URL = ('https://raw.githubusercontent.com/PE5PVB/TEF6686_ESP32'
             '/master/src/Tuner_Patch_Lithio_%s.h')
VERSIONS = {'102': ('V102_p224', '102'), '205': ('V205_p512', '205')}

NOTICE = """; ---------------------------------------------------------------------
; Copyright Catena / NXP semiconductors
;
; TEF668X firmware patch, extracted by tools/mkpatch.py from
;   %s
; This data is NXP / Catena copyrighted material, reproduced here for use
; with the TEF668X device it belongs to.  It is NOT covered by this
; project's licence and MUST NOT be committed or redistributed.  Both
; tefpatch.bin and tefpatch.inc are gitignored for that reason.
; ---------------------------------------------------------------------
"""

LIMIT = 0xFF00 - 0xE000         # room between the blob's base and the I/O page


def fetch(url):
    """Return the body of url, using urllib or falling back to curl."""
    try:
        try:
            from urllib.request import urlopen      # Python 3
        except ImportError:
            from urllib2 import urlopen             # Python 2
        return urlopen(url, timeout=30).read()
    except Exception as e:
        sys.stderr.write('  urllib failed (%s), trying curl\n' % e)
    try:
        return subprocess.check_output(['curl', '-sL', '-m', '60', url])
    except Exception as e:
        sys.exit('could not fetch %s\n  %s\n\n'
                 'No network? Supply a local copy instead:\n'
                 '  python tools/mkpatch.py <outdir> path/to/PATCH.c\n'
                 % (url, e))


def arrays(text):
    """Pull every  <name>[] ... = { 0x.., ... };  out of a C source."""
    out = {}
    for m in re.finditer(r'(\w+)\s*\[\s*\]\s*[^=;{]*=\s*\{(.*?)\}\s*;',
                         text, re.S):
        vals = re.findall(r'0[xX]([0-9a-fA-F]{1,2})', m.group(2))
        if vals:
            out[m.group(1)] = bytearray(int(v, 16) for v in vals)
    return out


def pick(found, *stems):
    """First array whose name contains one of the stems, case-insensitively."""
    for stem in stems:
        for name, data in sorted(found.items()):
            if stem.lower() in name.lower():
                return name, data
    return None, None


def main():
    argv = [a for a in sys.argv[1:]]
    if not argv:
        sys.exit('usage: mkpatch.py <output dir> [PATCH.c] [--version 102|205]')
    outdir = argv.pop(0)

    version = '102'
    if '--version' in argv:
        i = argv.index('--version')
        version = argv[i + 1]
        del argv[i:i + 2]
    if version not in VERSIONS:
        sys.exit('unknown version %r; known: %s'
                 % (version, ', '.join(sorted(VERSIONS))))

    src = argv[0] if argv else None
    if src is None:
        for cand in ('PATCH.c', 'patch.c', 'Patch.c'):
            if os.path.exists(cand):
                src = cand
                break

    if src:
        print('reading %s' % src)
        text = open(src, 'r').read()
        origin = os.path.abspath(src)
    else:
        url = FETCH_URL % VERSIONS[version][0]
        print('no local PATCH.c; fetching version %s' % version)
        print('  %s' % url)
        text = fetch(url)
        if not isinstance(text, str):
            text = text.decode('latin-1')
        origin = url

    found = arrays(text)
    if not found:
        sys.exit('no hex byte arrays found in that source')

    pname, patch = pick(found, 'PatchByteValues', 'PatchBytes', 'Patch')
    lname, lut = pick(found, 'LutByteValues', 'LutBytes', 'Lut')
    if patch is None:
        sys.exit('could not find the patch array (saw: %s)'
                 % ', '.join(sorted(found)))
    if lut is None:
        sys.exit('could not find the lookup-table array (saw: %s)'
                 % ', '.join(sorted(found)))

    print('  patch  %5d bytes  (%s)' % (len(patch), pname))
    print('  lut    %5d bytes  (%s)' % (len(lut), lname))

    # The transfer may be split anywhere on a 16-bit word boundary but
    # never inside one, so an odd length means the extraction went wrong.
    for what, blob in (('patch', patch), ('lut', lut)):
        if len(blob) % 2:
            sys.exit('the %s blob is %d bytes, which is odd.  The device is '
                     'fed 16-bit words, so an odd length means this was '
                     'parsed wrongly.' % (what, len(blob)))
    if len(patch) < 1000 or len(lut) < 16:
        sys.exit('those sizes look wrong: expected roughly 6 KB of patch and '
                 'about 120 bytes of table')

    total = len(patch) + len(lut)
    if total > LIMIT:
        sys.exit('patch + table is %d bytes but only %d fit between $E000 '
                 'and $FF00' % (total, LIMIT))

    binpath = os.path.join(outdir, 'tefpatch.bin')
    incpath = os.path.join(outdir, 'tefpatch.inc')
    with open(binpath, 'wb') as f:
        f.write(bytes(patch))
        f.write(bytes(lut))
    with open(incpath, 'w') as f:
        f.write(NOTICE % origin)
        f.write('TEF_PATCH_SIZE  equ     %d\n' % len(patch))
        f.write('TEF_LUT_SIZE    equ     %d\n' % len(lut))

    print('\nwrote %s (%d bytes) and %s' % (binpath, total, incpath))
    print('%d bytes to spare below $FF00' % (LIMIT - total))
    print('\nnow run make')


if __name__ == '__main__':
    main()
