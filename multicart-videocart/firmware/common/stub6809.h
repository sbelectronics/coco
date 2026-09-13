#ifndef MULTICART_STUB6809_H
#define MULTICART_STUB6809_H

#include <stdint.h>

// Hand-assembled image of stub6809.asm (origin $C000). Keep the two in
// sync; the .asm is the readable source of truth.
//
//  addr   bytes           mnemonic
//  C000   1A 50           ORCC #$50
//  C002   8E C0 15        LDX  #$C015        (TAIL)
//  C005   10 8E 06 00     LDY  #$0600
//  C009   C6 13           LDB  #19           (TAILLEN)
//  C00B   A6 80    COPY:  LDA  ,X+
//  C00D   A7 A0           STA  ,Y+
//  C00F   5A              DECB
//  C010   26 F9           BNE  COPY
//  C012   7E 06 00        JMP  $0600
//  C015   7F 00 71 TAIL:  CLR  $0071
//  C018   86 A5           LDA  #$A5
//  C01A   B7 FF 5F        STA  $FF5F
//  C01D   8E 40 00        LDX  #$4000        (~145 ms pre-jump delay)
//  C020   30 1F    DLY:   LEAX -1,X
//  C022   26 FC           BNE  DLY
//  C024   6E 9F FF FE     JMP  [$FFFE]
//  C028   (end, 40 bytes)

static const uint8_t stub6809[] = {
    0x1A, 0x50,
    0x8E, 0xC0, 0x15,
    0x10, 0x8E, 0x06, 0x00,
    0xC6, 0x13,
    0xA6, 0x80,
    0xA7, 0xA0,
    0x5A,
    0x26, 0xF9,
    0x7E, 0x06, 0x00,
    0x7F, 0x00, 0x71,
    0x86, 0xA5,
    0xB7, 0xFF, 0x5F,
    0x8E, 0x40, 0x00,
    0x30, 0x1F,
    0x26, 0xFC,
    0x6E, 0x9F, 0xFF, 0xFE,
};

#define STUB6809_LEN  (sizeof(stub6809))

#endif
