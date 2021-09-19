#!/usr/bin/python

# Read Cartridge File from Stdin
# Write Bin file to stdout, loading at address 0x4000
# Scott Baker, http://www.smbaker.com/

import sys

data = sys.stdin.read()
l = len(data)

sys.stdout.write(chr(0))
sys.stdout.write(chr(l>>8))
sys.stdout.write(chr(l&0xFF))
sys.stdout.write(chr(0x40))
sys.stdout.write(chr(0x00))
sys.stdout.write(data)
sys.stdout.write(chr(0xFF))
sys.stdout.write(chr(0x00))
sys.stdout.write(chr(0x00))
sys.stdout.write(chr(0x40))
sys.stdout.write(chr(0x00))
