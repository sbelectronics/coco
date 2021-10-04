Colorware Realtalker Notes
Scott Baker, http://www.smbaker.com/

The "convert" subroutine:

63996 QH=65410:QI=PI*64-48:POKE65281,180:POKE65283,61:POKE65315,63:FORQJ=1TOLEN(QH$):QK=ASC(MID$(QH$,QJ,1)):POKEQH,QK+QI:NEXT:RETURN

REM Enable Cartridge to TV Sound
POKE 65281, 180
POKE 65283, 61
POKE 65315, 63

REM Output Phoneme
REM Phoneme is ASCII "0" to "q"
POKE 65410, Phoneme + Pitch*64 - 48

Pitch 0 is the lowest, Pitch 3 is the highest


COCO
1 NC
2 -> R6 -> J1 -> (SC-1, R3, CBIG)
3 -> D3>| -> SC-8 
4 NC
5 NC
6 -> J4 -> 75-6
7 -> J2 -> 27-11
8  NC
9 -> J3 -> (30-14, R4, R5, CAP, 27-14, 75-4, 75-5)
10 -> SC-14
11 -> SC-13
12 -> SC-12
13 -> SC-11
14 -> SC-10
15 -> SC-9
16 -> 75-2
17 -> 75-3
18 -> 27-13
19 NC
20 NC
21 NC
22 NC
23 NC
24 NC
25 -> 27-2
26 -> 75-7
27 -> 30-2
28 -> 30-3
29 -> 30-4
30 -> 30-5
31 -> 30-6
32 NC
33 NC
34 GND -> (30-7, CAP2, 27-4, 27-5, 27-7, R1, R2, C1, CBIG, SC-18, 30-12)
35 -> VIA -> (R1, SC-21, SC-22)
36 NC
37 -> 30-1
38 -> VIA -> 30-12
39 -> 30-11
40 NC

SC01
1 
2 -> 75-15
3 -> 75-16
4
5
6
7 -> (75-13, 27-8)
8 -> D3|
9
10
11
12
13
14
15 -> (SC-16, R3, C1)
16
17
18
19
20
21 -> (SC-22, R1)
22

LS30
8 - 27-1

LS27

LS75
8 - D
11 - D
15 - R5
16 - R4
