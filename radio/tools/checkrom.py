# -*- coding: utf-8 -*-
# Layout and consistency checks for the CoCo radio cartridge ROM image.
#
#   python tools/checkrom.py build/radio.bin
#
# Python 2.7 or 3.  Everything checked here assembles cleanly and fails
# only on real hardware.
from __future__ import print_function
import sys, os

IMAGE_LEN = 16128               # $C000-$FEFF, chip $0000-$3EFF
STORE_OFF = 0x1F00              # $DF00
PATCH_OFF = 0x2000              # $E000
PAGELEN = 64
RECLEN = 16
NRECORD = 12
RECBASE = 0x40
ROMF_DEVMPI = 0x01

path = sys.argv[1]
data = bytearray(open(path, 'rb').read())
errors = []
notes = []


def err(msg):
    errors.append(msg)


def note(msg):
    notes.append(msg)


# ---- 1. size -------------------------------------------------------
# The window is 16,128 bytes, not 16 K.  Chip $3F00-$3FFF would appear at
# $FF00-$FFFF, the I/O page, so it is unreachable and must not be used.
if len(data) != IMAGE_LEN:
    err('image is %d bytes, must be exactly %d ($C000-$FEFF)'
        % (len(data), IMAGE_LEN))
    sys.exit('\n'.join(errors))

# ---- 2. entry header -----------------------------------------------
# The image must not begin "DK" or Disk BASIC claims it.  JMP is $7E.
if data[0] != 0x7E:
    err('byte 0 is $%02X, must be $7E (JMP).  A cartridge beginning "DK" '
        'is claimed by Disk BASIC.' % data[0])
if bytes(data[3:12]) != b'COCORADIO':
    err('signature at offset 3 is %r, must be "COCORADIO"' % bytes(data[3:12]))
if data[14] != 1:
    err('board byte at offset 14 is %d, must be 1' % data[14])
note('version %d.%d, board %d' % (data[12], data[13], data[14]))

# Header flags, offset 15.  A development image must never be mistaken
# for one that can go into a cartridge.
if data[15] & ROMF_DEVMPI:
    note('*** MULTI-PAK DEVELOPMENT BUILD ***  SCS is forced to a fixed '
         'slot and the preset store is RAM-only, so nothing is ever '
         'written to an EEPROM.  Do not program this into a cartridge.')

# ---- 3. where the code ends -----------------------------------------
end = STORE_OFF
while end > 0 and data[end - 1] == 0xFF:
    end -= 1
note('code ends at image $%04X (CPU $%04X), %d bytes free below the store'
     % (end, end + 0xC000, STORE_OFF - end))

# ---- 4. preset store -------------------------------------------------
# Every block carries a checksum that makes it sum to zero, so a freshly
# programmed cartridge must not report PRESETS RESET on its first boot.
if data[STORE_OFF] != 0x52 or data[STORE_OFF + 1] != 0x44:
    err('store magic at $DF00 is $%02X$%02X, must be $52$44 ("RD")'
        % (data[STORE_OFF], data[STORE_OFF + 1]))
if data[STORE_OFF + 2] != 1:
    err('store version is %d, must be 1' % data[STORE_OFF + 2])

hdrsum = sum(data[STORE_OFF:STORE_OFF + PAGELEN]) & 0xFF
if hdrsum != 0:
    err('default header page sums to $%02X, must be $00.  Note this '
        'assembler concatenates character literals joined with "+", so a '
        'checksum expression containing \'R\'+\'D\' silently computes the '
        'wrong value.' % hdrsum)

for n in range(NRECORD):
    off = STORE_OFF + RECBASE + n * RECLEN
    s = sum(data[off:off + RECLEN]) & 0xFF
    if s != 0:
        err('default preset record %d sums to $%02X, must be $00' % (n + 1, s))
    sta = (data[off] << 8) | data[off + 1]
    if sta != 0:
        err('default preset record %d is not empty (station $%04X)'
            % (n + 1, sta))

# ---- 5. the firmware patch region -----------------------------------
if any(b != 0xFF for b in data[PATCH_OFF:PATCH_OFF + 256]):
    pend = len(data)
    while pend > PATCH_OFF and data[pend - 1] == 0xFF:
        pend -= 1
    note('firmware patch present, about %d bytes at $E000' % (pend - PATCH_OFF))
else:
    note('no firmware patch in this image (TEF_PATCH=0).  The ROM will '
         'run the digital bring-up, but the module will stay in boot state '
         'and will not tune.')

# ---- report ----------------------------------------------------------
print('checkrom: %s' % os.path.basename(path))
for n in notes:
    print('  note:  %s' % n)
for e in errors:
    print('  ERROR: %s' % e)
if errors:
    print('\n%d problem(s); do not program this image.' % len(errors))
    sys.exit(1)
print('  image OK')
