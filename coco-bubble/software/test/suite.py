#!/usr/bin/env python3
"""
suite.py - regression tests for the bubble cartridge ROM.

Each case types a little BASIC at the emulated machine and checks that
the screen ends up containing what it should.  Everything runs against
the MOCK image, whose RAM-backed pseudo-disk exercises every layer
above the 7220 driver itself.

  ./suite.py                 run everything
  ./suite.py dir save        run just the cases whose names match
"""

import subprocess
import sys
import os
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROM = os.path.join(HERE, "..", "build", "bubble-mock.rom")
COCOTEST = os.path.join(HERE, "cocotest.py")

# name, seconds to run, BASIC to type, list of strings that must appear,
# list of strings that must NOT appear
CASES = [
    ("banner", 4, "",
     ["BUBBLE EXTENDED COLOR BASIC", "128K BUBBLE READY"], ["ERROR"]),

    ("tokens", 6, '10 DIR:LOAD"X":DSKINI 0\r20 A=FREE(0)+LOC(1)\rLIST\r',
     ['10 DIR:LOAD"X":DSKINI 0', "20 A=FREE(0)+LOC(1)"], ["ERROR"]),

    # Extended BASIC parses the &H and &O constants in its expression
    # hook, RVEC15, and does not install that hook until L96EC runs -
    # which cold start calls after the hooks are in place.  So DVEC15
    # has to jump to XVEC15 outright; a saved-and-chained slot captures
    # the RTS Color BASIC left there and the constants disappear from
    # the language with no error to say so.  CLEAR is here because a
    # hex argument to it is how this first surfaced.
    ("hex-constants", 8,
     'PRINT &H10;&O17;HEX$(255)\rCLEAR 200,&H4FFF\rPRINT &H7FF\r',
     [" 16  15 FF", " 2047"], ["ERROR"]),

    # The RAM claim must leave BASIC's program text where Disk BASIC
    # leaves it, or a program that peeks TXTTAB disagrees with us.
    ("txttab-parity", 5, 'PRINT PEEK(25)*256+PEEK(26)\r',
     [" 9729"], ["ERROR"]),

    ("dskini-free", 7, "DSKINI0\rPRINT FREE(0)\r",
     [" 6"], ["ERROR"]),

    ("dir-empty", 7, "DSKINI0\rDIR\r",
     ["6 GRANULES FREE"], ["ERROR"]),

    # DSKINI destroys whatever is on the drive it lands on, so the
    # drive number is required, as in Disk BASIC.  DIR and UNLOAD keep
    # theirs optional.
    ("dskini-needs-drive", 6, "DSKINI\r",
     ["?DN ERROR"], []),

    ("save-dir", 9, 'DSKINI0\r10 PRINT"HI"\rSAVE"TEST"\rDIR\r',
     ["TEST     BAS 0 B 1", "5 GRANULES FREE"], ["ERROR"]),

    ("save-load", 10,
     'DSKINI0\r10 PRINT"HELLO"\r20 END\rSAVE"T"\rNEW\rLOAD"T"\rLIST\r',
     ['10 PRINT"HELLO"', "20 END"], ["ERROR"]),

    ("load-run", 10,
     'DSKINI0\r10 PRINT"RAN OK"\rSAVE"R"\rNEW\rLOAD"R",R\r',
     ["RAN OK"], ["ERROR"]),

    ("seq-io", 14,
     'DSKINI0\r10 OPEN"O",#1,"D"\r20 PRINT#1,"HI THERE"\r30 PRINT#1,42\r'
     '40 CLOSE#1\r50 OPEN"I",#1,"D"\r60 INPUT#1,A$,B\r70 CLOSE#1\r'
     '80 PRINT"[";A$;"][";B;"]"\rRUN\r',
     ["[HI THERE][ 42 ]"], ["ERROR"]),

    ("seq-eof", 16,
     'DSKINI0\r10 OPEN"O",#1,"E"\r20 FOR I=1 TO 5:PRINT#1,I:NEXT\r'
     '30 CLOSE#1\r40 OPEN"I",#1,"E"\r50 S=0\r'
     '60 IF EOF(1) THEN 90\r70 INPUT#1,V:S=S+V\r80 GOTO 60\r'
     '90 CLOSE#1:PRINT"SUM";S\rRUN\r',
     ["SUM 15"], ["ERROR"]),

    ("multi-sector", 18,
     'DSKINI0\r10 OPEN"O",#1,"BIG"\r20 FOR I=1 TO 60\r'
     '30 PRINT#1,"0123456789"\r40 NEXT:CLOSE#1\r'
     '50 OPEN"I",#1,"BIG":N=0\r60 IF EOF(1) THEN 90\r'
     '70 INPUT#1,A$:N=N+1\r80 GOTO 60\r90 CLOSE#1:PRINT"LINES";N\rRUN\r',
     ["LINES 60"], ["ERROR"]),

    ("kill", 10,
     'DSKINI0\r10 PRINT"X"\rSAVE"A"\rSAVE"B"\rKILL"A.BAS"\rDIR\r',
     ["B        BAS"], ["A        BAS", "ERROR"]),

    ("rename", 10,
     'DSKINI0\r10 PRINT"X"\rSAVE"OLD"\rRENAME"OLD.BAS" TO "NEW.BAS"\rDIR\r',
     ["NEW      BAS"], ["OLD      BAS", "ERROR"]),

    ("save-ascii", 10,
     'DSKINI0\r10 PRINT"HI"\rSAVE"A",A\rDIR\r',
     ["A        BAS 0 A"], ["ERROR"]),

    ("kill-frees", 10,
     'DSKINI0\r10 PRINT"X"\rSAVE"Z"\rKILL"Z.BAS"\rPRINT FREE(0)\r',
     [" 6"], ["ERROR"]),

    ("file-not-found", 7, 'DSKINI0\rLOAD"NOPE"\r',
     ["?NE ERROR"], []),

    ("dup-name", 9, 'DSKINI0\r10 PRINT"X"\rSAVE"D"\rRENAME"D.BAS" TO "D.BAS"\r',
     ["?AE ERROR"], []),

    ("copy", 11,
     'DSKINI0\r10 PRINT"COPYME"\rSAVE"S.BAS"\rCOPY"S.BAS" TO "T.BAS"\r'
     'NEW\rLOAD"T"\rLIST\r',
     ['10 PRINT"COPYME"'], ["ERROR"]),

    # A copy whose source spans more than one granule, so the chain is
    # walked and the destination's is built a granule at a time.  48
    # records of 32 bytes is 1536, exactly two MOCK granules.
    ("copy-multigran", 34,
     'CLEAR300\rDSKINI0\r'
     '10 OPEN"O",#1,"B":FOR I=1 TO 48:PRINT#1,STRING$(31,"X"):NEXT\r'
     '20 CLOSE#1:COPY"B.DAT" TO "C.DAT"\r'
     '30 OPEN"I",#1,"C.DAT":N=0\r40 IF EOF(1) THEN 70\r'
     '50 INPUT#1,A$:IF LEN(A$)=31 THEN N=N+1\r60 GOTO 40\r'
     '70 PRINT"N=";N;"LOF=";LOF(1):CLOSE#1\rRUN\r',
     ["N= 48 LOF= 6"], ["ERROR"]),

    # The same copy with less free memory than the file is long.  Disk
    # BASIC buffers whole sectors between the arrays and the stack and
    # reports ?OM past that; this copies a sector at a time through
    # DBUF0 and needs no free memory at all.  CLEAR leaves about 1300
    # bytes against a 1536 byte file.
    ("copy-no-memory", 30,
     'CLEAR300\rDSKINI0\r'
     '10 OPEN"O",#1,"B":FOR I=1 TO 48:PRINT#1,STRING$(31,"X"):NEXT\r'
     '20 CLOSE#1\rRUN\r'
     'CLEAR21500\rCOPY"B.DAT" TO "C.DAT"\rDIR\r',
     ["B        DAT 1 A 2", "C        DAT 1 A 2", "2 GRANULES FREE"],
     ["ERROR"]),

    # Copying a file onto itself would clear the destination before
    # reading the source, so it is refused rather than obeyed.
    ("copy-onto-self", 10,
     'DSKINI0\r10 PRINT"X"\rSAVE"S.BAS"\rCOPY"S.BAS" TO "S.BAS"\r',
     ["?AE ERROR"], []),

    ("savem-loadm", 13,
     'DSKINI0\rFOR I=0 TO 7:POKE 3584+I,I*3:NEXT\r'
     'SAVEM"M",3584,3591,3584\r'
     'FOR I=0 TO 7:POKE 3584+I,0:NEXT\r'
     'LOADM"M"\rPRINT PEEK(3584);PEEK(3587);PEEK(3591)\r',
     [" 0  9  21"], ["ERROR"]),

    # Two 128 character strings do not fit in BASIC's default 200
    # bytes of string space, so these have to CLEAR some room first.
    ("dski-dsko", 13,
     'CLEAR1000\rDSKINI0\rA$=STRING$(128,"A"):B$=STRING$(128,"B")\r'
     'DSKO$0,0,1,A$,B$\rDSKI$0,0,1,C$,D$\r'
     'PRINT LEFT$(C$,3);LEFT$(D$,3);LEN(C$)+LEN(D$)\r',
     ["AAABBB 256"], ["ERROR"]),

    # VERIFY ON reads every sector back after writing it and compares.
    # It is the cold start default, so the whole suite runs with it on;
    # this case exercises the switch itself and that a file written
    # either way reads back.  Twelve numbers sum to 78.
    ("verify", 20,
     'DSKINI0\rVERIFY ON\r'
     '10 OPEN"O",#1,"V":FOR I=1 TO 12:PRINT#1,I:NEXT:CLOSE#1\r'
     '20 VERIFY OFF\r'
     '30 OPEN"O",#1,"W":FOR I=1 TO 12:PRINT#1,I:NEXT:CLOSE#1\r'
     '40 VERIFY ON\r'
     '50 OPEN"I",#1,"V":S=0\r'
     '60 FOR I=1 TO 12:INPUT#1,N:S=S+N:NEXT:CLOSE#1\r'
     '70 PRINT"S=";S\rRUN\r',
     ["S= 78"], ["ERROR"]),

    ("dead-zone", 11,
     'CLEAR1000\rDSKINI0\rDSKI$0,2,10,A$,B$\rPRINT ASC(LEFT$(A$,1))\r',
     [" 0"], ["ERROR"]),

    ("dead-zone-write", 10,
     'CLEAR1000\rDSKINI0\rA$=STRING$(128,"A"):DSKO$0,2,10,A$,A$\r',
     ["?WP ERROR"], []),

    ("ascii-roundtrip", 13,
     'DSKINI0\r10 PRINT"ONE"\r20 PRINT"TWO"\rSAVE"A",A\rNEW\r'
     'LOAD"A"\rLIST\r',
     ['10 PRINT"ONE"', '20 PRINT"TWO"'], ["ERROR"]),

    # An ASCII listing longer than one sector, loaded twice.  Reading
    # must not write to the file.  A read path that did would step past
    # its own place in the buffer and, once the pointer wrapped, flush
    # the buffer back over the file - which only happens past 256 bytes,
    # and only shows on the second load, when the damaged file no longer
    # reads back the same.
    ("ascii-load-nondestructive", 34,
     'DSKINI0\r'
     '10 REM ' + 'A' * 44 + '\r'
     '20 REM ' + 'B' * 44 + '\r'
     '30 REM ' + 'C' * 44 + '\r'
     '40 REM ' + 'D' * 44 + '\r'
     '50 REM ' + 'E' * 44 + '\r'
     '60 REM ' + 'F' * 44 + '\r'
     'SAVE"L",A\rNEW\rLOAD"L"\rLOAD"L"\rLIST 20\r',
     ["20 REM " + "B" * 24], ["ERROR"]),

    # WRITE# quotes strings, so a value containing a comma survives
    # the round trip through INPUT#, which PRINT# would not manage.
    ("write-input", 15,
     'DSKINI0\r10 OPEN"O",#1,"W"\r20 WRITE#1,"A,B",42\r30 CLOSE#1\r'
     '40 OPEN"I",#1,"W"\r50 INPUT#1,A$,N\r60 CLOSE#1\r'
     '70 PRINT"[";A$;"][";N;"]"\rRUN\r',
     ["[A,B][ 42 ]"], ["ERROR"]),

    ("merge", 15,
     'DSKINI0\r20 PRINT"TWENTY"\rSAVE"M",A\rNEW\r'
     '10 PRINT"TEN"\rMERGE"M"\rLIST\r',
     ['10 PRINT"TEN"', '20 PRINT"TWENTY"'], ["ERROR"]),

    # A file that ends exactly on a granule boundary (24 lines of 32
    # bytes = 768 = one full MOCK granule) must occupy exactly one
    # granule and read back exactly 24 items.  Allocating the next
    # granule eagerly, when the previous one fills, chains on a phantom
    # granule full of garbage (FREE 4, LOF 4); this guards against it.
    ("granule-boundary", 20,
     'CLEAR300\rDSKINI0\r'
     '10 OPEN"O",#1,"B"\r20 FOR I=1 TO 24:PRINT#1,STRING$(31,"X"):NEXT\r'
     '30 CLOSE#1\r40 PRINT"FREE";FREE(0)\r'
     '50 OPEN"I",#1,"B":N=0\r60 IF EOF(1) THEN 90\r'
     '70 INPUT#1,A$:IF LEN(A$)=31 THEN N=N+1\r80 GOTO 60\r'
     '90 PRINT"N=";N;"LOF=";LOF(1):CLOSE#1\rRUN\r',
     ["FREE 5", "N= 24 LOF= 3"], ["ERROR"]),

    # A last sector that is exactly full (two 127-byte items + CRs =
    # 256) exercises the FCBLST=256 encoding.
    ("full-last-sector", 16,
     'CLEAR400\rDSKINI0\r'
     '10 OPEN"O",#1,"S"\r20 PRINT#1,STRING$(127,"Y")\r'
     '30 PRINT#1,STRING$(127,"Z")\r40 CLOSE#1\r'
     '50 OPEN"I",#1,"S"\r60 INPUT#1,A$,B$\r'
     '70 PRINT LEN(A$);LEN(B$);EOF(1):CLOSE#1\rRUN\r',
     [" 127  127 -1"], ["ERROR"]),

    # Two files open at once, writes interleaved: independent FCBs,
    # independent granule chains, shared FAT.
    ("two-files", 18,
     'DSKINI0\r'
     '10 OPEN"O",#1,"A":OPEN"O",#2,"B"\r'
     '20 FOR I=1 TO 9:PRINT#1,I:PRINT#2,I*10:NEXT\r'
     '30 CLOSE#1:CLOSE#2\r'
     '40 OPEN"I",#1,"A":OPEN"I",#2,"B":S=0\r'
     '50 FOR I=1 TO 9:INPUT#1,V:INPUT#2,W:S=S+V+W:NEXT\r'
     '60 CLOSE#1:CLOSE#2:PRINT"S=";S\rRUN\r',
     ["S= 495"], ["ERROR"]),

    # An empty file is legal and is at EOF from the first question.
    ("empty-file", 13,
     'DSKINI0\r10 OPEN"O",#1,"MT"\r20 CLOSE#1\r'
     '30 OPEN"I",#1,"MT"\r40 PRINT"EOF";EOF(1)\r50 CLOSE#1\rRUN\r',
     ["EOF-1"], ["ERROR"]),

    # The run flag must not leak out of LOAD ,R: the SAVE ",A" that
    # follows would spuriously re-run the program.  The program bumps
    # a counter in the cassette buffer (RAM no one else touches).
    ("runflag-leak", 16,
     'DSKINI0\r10 POKE474,PEEK(474)+1\rSAVE"R"\rPOKE474,0\r'
     'LOAD"R",R\rSAVE"S",A\rPRINT"C=";PEEK(474)\r',
     ["C= 1"], ["?"]),

    # RUN "file" - the RVEC18 hook.
    ("run-file", 13,
     'DSKINI0\r10 POKE474,111\rSAVE"P"\rNEW\rPOKE474,0\r'
     'RUN"P"\rPRINT"GOT";PEEK(474)\r',
     ["GOT 111"], ["ERROR"]),

    # A file that is open may not be killed or overwritten.
    ("kill-open", 13,
     'DSKINI0\r10 PRINT"X"\rSAVE"K"\rOPEN"I",#1,"K.BAS"\rKILL"K.BAS"\r',
     ["?AO ERROR"], []),

    ("save-over-open", 13,
     'DSKINI0\r10 PRINT"X"\rSAVE"K"\rOPEN"I",#1,"K.BAS"\rSAVE"K"\r',
     ["?AO ERROR"], []),

    # UNLOAD forgets the cached allocation table; everything after it
    # must come back from the media, which proves the write-back path.
    ("persistence", 16,
     'DSKINI0\r10 PRINT"PERSIST"\rSAVE"P"\rUNLOAD\r'
     'PRINT"FREE";FREE(0)\rNEW\rLOAD"P"\rLIST\r',
     ["FREE 5", '10 PRINT"PERSIST"'], ["ERROR"]),

    # Six one-granule files fill the MOCK bubble; the seventh save
    # must report disk full, and cleanly.
    ("disk-full", 22,
     'DSKINI0\r10 PRINT"A"\rSAVE"F1"\rSAVE"F2"\rSAVE"F3"\r'
     'SAVE"F4"\rSAVE"F5"\rSAVE"F6"\rSAVE"F7"\r',
     ["?DF ERROR"], []),

    # LOADM with an offset that lands in ROM: the store-verify path
    # must report ?IO instead of pretending.
    ("loadm-to-rom", 15,
     'DSKINI0\rFOR I=0 TO 7:POKE3584+I,I:NEXT\r'
     'SAVEM"M",3584,3591,3584\rLOADM"M",49152\r',
     ["?IO ERROR"], []),

    # WRITE# to a file opened for input is a mode error.
    ("write-to-input", 13,
     'DSKINI0\rOPEN"O",#1,"W"\rPRINT#1,"X"\rCLOSE#1\r'
     'OPEN"I",#1,"W"\rWRITE#1,"Y"\r',
     ["?FM ERROR"], []),

    # Asking a finished file for another item is input past the end.
    ("input-past-eof", 15,
     'DSKINI0\r10 OPEN"O",#1,"E"\r20 PRINT#1,"X"\r30 CLOSE#1\r'
     '40 OPEN"I",#1,"E"\r50 INPUT#1,A$\r60 INPUT#1,B$\rRUN\r',
     ["?IE ERROR IN 60"], []),

    # A ninth filename character is one too many.
    ("long-name", 8,
     'DSKINI0\rSAVE"ABCDEFGHI"\r',
     ["?FN ERROR"], []),

    # The PMODE interposer has to keep working with the RAM layout the
    # ROM claimed - a wrong GRPRAM turns graphics commands into crashes.
    ("pmode", 8,
     'PMODE4,1:PCLS:SCREEN0,0:PRINT"PM OK"\r',
     ["PM OK"], ["ERROR"]),

    # PCLEAR moves the program, by one 1536-byte graphics page per step,
    # off the same GRPRAM the PMODE interposer uses.  Cold start reserves
    # four pages and puts TXTTAB at 9729, so eight pages is 9729+4*1536
    # and one page is 9729-3*1536.  Getting this wrong walks the program
    # into the FCBs, which end near $0DDD - which is why one page, the
    # lowest the program can go, is part of the test.
    ("pclear", 12,
     '10 PRINT"P";PEEK(25)*256+PEEK(26)\r'
     'PCLEAR8\rRUN\rPCLEAR1\rRUN\r',
     ["P 15873", "P 5121"], ["ERROR"]),

    # --- FILES ---

    # FILES moves the FCBs and the record buffers, and everything above
    # them - graphics pages, program, variables - shifts to suit.  More
    # files means BASIC starts higher up.
    # The default is two files, ending the FCB area on $0E00 and
    # putting BASIC at $2601.  FILES 4 adds three more FCBs of 281
    # bytes, which rounds the end up to page $12 and carries BASIC four
    # pages up to $2A01.  FILES 0 leaves only the system FCB and brings
    # it two pages down to $2401.
    ("files-moves-txttab", 18,
     'PRINT"A";PEEK(25)*256+PEEK(26)\rFILES 4\r'
     'PRINT"B";PEEK(25)*256+PEEK(26)\rFILES 0\r'
     'PRINT"C";PEEK(25)*256+PEEK(26)\r',
     ["A 9729", "B 10753", "C 9217"], ["ERROR"]),

    # The program has to survive the move, or a FILES typed above a
    # listing would quietly destroy it.
    ("files-keeps-program", 16,
     '10 PRINT"KEPT"\r20 PRINT"TOO"\rFILES 5\rRUN\r',
     ["KEPT", "TOO"], ["ERROR"]),

    # And it has to survive being moved back down again.
    ("files-shrinks-back", 18,
     '10 PRINT"BACK"\rFILES 6\rFILES 2\r'
     'PRINT"T";PEEK(25)*256+PEEK(26)\rRUN\r',
     ["T 9729", "BACK"], ["ERROR"]),

    # Fifteen is the limit.
    ("files-too-many", 8, "FILES 16\r",
     ["?FC ERROR"], []),

    # The second argument is the record buffer space, and asking for
    # more of it makes room for a longer record.
    ("files-buffer-space", 16,
     'DSKINI0\rFILES 2,600\r'
     'OPEN"D",#1,"R",600\rCLOSE#1\rPRINT"OK"\r',
     ["OK"], ["ERROR"]),

    # --- direct access files ---

    # A direct open creates the file if it is not there, so this costs
    # a granule, and the entry has to survive the close.
    ("direct-open-creates", 11,
     'DSKINI0\rOPEN"D",#1,"R",32\rCLOSE#1\rDIR\r',
     ["R        DAT", "5 GRANULES FREE"], ["ERROR"]),

    # Opening one that does exist must not truncate it or take a
    # second granule.
    ("direct-open-existing", 13,
     'DSKINI0\rOPEN"D",#1,"R",32\rCLOSE#1\r'
     'OPEN"D",#1,"R",32\rCLOSE#1\rPRINT FREE(0)\r',
     [" 5"], ["ERROR"]),

    # Disk BASIC takes either letter for direct access.
    ("direct-mode-r", 10,
     'DSKINI0\rOPEN"R",#1,"R",32\rCLOSE#1\rPRINT FREE(0)\r',
     [" 5"], ["ERROR"]),

    # The record buffer area is one sector, so 256 fits exactly and
    # 257 does not.
    ("direct-buffer-limit", 12,
     'DSKINI0\rOPEN"D",#1,"A",256\rCLOSE#1\rPRINT"OK"\r'
     'OPEN"D",#1,"B",257\r',
     ["OK", "?OB ERROR"], []),

    # Closing a file has to give its buffer back, or the area is used
    # up after the first open and never recovers.
    ("direct-buffer-reuse", 16,
     'DSKINI0\r10 FOR I=1 TO 5:OPEN"D",#1,"R",256:CLOSE#1:NEXT\r'
     '20 PRINT"OK"\rRUN\r',
     ["OK"], ["ERROR"]),

    # Buffers are handed out in order but need not be closed in that
    # order: closing the lower one first has to slide the upper one
    # down, or the 200 byte record at the end will not fit.
    ("direct-buffer-compact", 18,
     'DSKINI0\r10 OPEN"D",#1,"A",100:OPEN"D",#2,"B",100\r'
     '20 CLOSE#1:CLOSE#2\r'
     '30 OPEN"D",#1,"C",200:CLOSE#1\r'
     '40 PRINT"OK"\rRUN\r',
     ["OK"], ["ERROR"]),

    # PRINT# and INPUT# on a direct file work within the record it is
    # holding: PRINT# fills it, PUT sends it, and INPUT# reads it back
    # out again.  This is the way a record carries several fields
    # without FIELD being involved at all.
    # The separator is written explicitly.  A comma in a PRINT# pads out
    # to the next 16 column tab field, exactly as Disk BASIC does, so it
    # is not a delimiter INPUT# can read back.
    # INPUT is not allowed outside a program, so this has to be one.
    ("record-print-input", 28,
     'CLEAR1000\rDSKINI0\r'
     '10 OPEN"D",#1,"R",32\r'
     '20 PRINT#1,"AB";",";42\r'
     '30 PUT#1,1:CLOSE#1\r'
     '40 OPEN"D",#1,"R",32\r'
     '50 GET#1,1\r'
     '60 INPUT#1,X$,N\r'
     '70 PRINT"P";X$;N:CLOSE#1\rRUN\r',
     ["PAB 42"], ["ERROR"]),

    # Printing more than the record holds has nowhere to put the rest.
    ("record-print-overflow", 14,
     'CLEAR1000\rDSKINI0\r'
     '10 OPEN"D",#1,"R",4:PRINT#1,"ABCDEFGH"\rRUN\r',
     ["?ER ERROR"], []),

    # Closing a direct file must not restate its type from the FCB:
    # offsets 9 and 10 hold the record length there, not the type and
    # ASCII flag, so writing them back would put the record length
    # into the directory entry as the file's type.  Read the entry
    # itself rather than trusting DIR's rendering of it - a wrong type
    # byte still prints as something.
    # GET/PUT on a direct file comes in through the CLS RAM hook,
    # because Extended BASIC's GET/PUT parser shares a code path with
    # CLS.  So the first thing to check is that CLS and graphics
    # GET/PUT still work: a hook that took those over would break
    # every graphics program on the machine.
    ("getput-graphics-intact", 16,
     '10 PMODE 4,1:DIM A(30)\r20 GET(0,0)-(1,1),A\r'
     '30 PUT(0,0)-(1,1),A\r40 CLS\r50 PRINT"OK"\rRUN\r',
     ["OK"], ["ERROR"]),

    # A PUT past the end of the file extends it, and the directory
    # entry has to end up saying how long it became.  DIRLST is bytes
    # 15 and 16 of the entry.
    ("getput-extends", 18,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",32\rPUT#1,1\rCLOSE#1\r'
     'DSKI$0,1,3,A$,B$\r'
     'PRINT"L";ASC(MID$(A$,15,1))*256+ASC(MID$(A$,16,1))\r',
     ["L 32"], ["ERROR"]),

    # Without a record number the file uses the one it is already at,
    # so a bare PUT writes the next record on.
    ("getput-next-record", 20,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",32\rPUT#1,1\rPUT#1\rCLOSE#1\r'
     'DSKI$0,1,3,A$,B$\r'
     'PRINT"L";ASC(MID$(A$,15,1))*256+ASC(MID$(A$,16,1))\r',
     ["L 64"], ["ERROR"]),

    # A record far past the end takes as many granules as it needs.
    # Record 4 of 256 bytes starts at sector 3, which on the mock
    # geometry is the second granule.
    ("getput-grows-granules", 16,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",256\rPUT#1,4\rCLOSE#1\rPRINT FREE(0)\r',
     [" 4"], ["ERROR"]),

    # A GET may not read what was never written, whether the granule
    # chain runs out or the record simply reaches past FCBLST.
    ("getput-past-end", 12,
     'DSKINI0\r10 OPEN"D",#1,"R",32:GET#1,50\rRUN\r',
     ["?IE ERROR"], []),
    ("getput-unwritten", 12,
     'DSKINI0\r10 OPEN"D",#1,"R",32:GET#1,1\rRUN\r',
     ["?IE ERROR"], []),

    # Records are numbered from 1.
    ("getput-record-zero", 12,
     'DSKINI0\r10 OPEN"D",#1,"R",32:GET#1,0\rRUN\r',
     ["?BR ERROR"], []),

    # Only a direct file has records, and it has to be open.
    ("getput-wrong-mode", 12,
     'DSKINI0\r10 OPEN"O",#1,"S":GET#1,1\rRUN\r',
     ["?FM ERROR"], []),
    ("getput-not-open", 10,
     'DSKINI0\r10 GET#1,1\rRUN\r',
     ["?NO ERROR"], []),

    # A record survives a PUT, a close and a reopen.  The buffer is
    # written and read here by POKE and PEEK rather than through a
    # FIELDed variable, because MID$ does not write through a FIELDed
    # descriptor - LSET and RSET are what do that, and they are not
    # built yet.  B is the buffer the next open will be given, which is
    # wherever RNBFAD points before it.
    ("record-roundtrip", 26,
     'CLEAR1000\rDSKINI0\rB=PEEK(2376)*256+PEEK(2377)\r'
     'OPEN"D",#1,"R",32\rPOKE B,65:POKE B+1,66:POKE B+31,90\r'
     'PUT#1,1\rCLOSE#1\r'
     'OPEN"D",#1,"R",32\rPOKE B,0:POKE B+1,0:POKE B+31,0\r'
     'GET#1,1\rPRINT"V";PEEK(B);PEEK(B+1);PEEK(B+31)\rCLOSE#1\r',
     ["V 65  66  90"], ["ERROR"]),

    # Two records in the same sector must not tread on each other: a
    # PUT reads the sector before writing it back, so record 2 leaves
    # record 1 alone.
    ("record-neighbours", 30,
     'CLEAR1000\rDSKINI0\rB=PEEK(2376)*256+PEEK(2377)\r'
     'OPEN"D",#1,"R",32\r'
     'POKE B,11\rPUT#1,1\rPOKE B,22\rPUT#1,2\r'
     'POKE B,0\rGET#1,1\rPRINT"R1";PEEK(B)\r'
     'POKE B,0\rGET#1,2\rPRINT"R2";PEEK(B)\rCLOSE#1\r',
     ["R1 11", "R2 22"], ["ERROR"]),

    # FIELD points a string variable straight at part of the record
    # buffer, so the variable is a window on the record: its length
    # comes from the field, not from any string space.
    ("field-lengths", 16,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",32\rFIELD#1,4 AS A$,8 AS B$\r'
     'PRINT"L";LEN(A$);LEN(B$)\rCLOSE#1\r',
     ["L 4  8"], ["ERROR"]),

    # A GET is visible through the FIELDed variable with no copying.
    ("field-sees-get", 24,
     'CLEAR1000\rDSKINI0\rB=PEEK(2376)*256+PEEK(2377)\r'
     'OPEN"D",#1,"R",32\rPOKE B,72:POKE B+1,73\rPUT#1,1\rCLOSE#1\r'
     'OPEN"D",#1,"R",32\rFIELD#1,2 AS A$\r'
     'POKE B,0:POKE B+1,0\rGET#1,1\rPRINT"F";A$\rCLOSE#1\r',
     ["FHI"], ["ERROR"]),

    # LSET and RSET are the only way to write a FIELDed variable, and
    # now the whole round trip goes through the record: field it, LSET
    # it, PUT it, then reopen, field it and GET it back.
    ("lset-roundtrip", 26,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",32\rFIELD#1,6 AS A$\r'
     'LSET A$="ABC"\rPUT#1,1\rCLOSE#1\r'
     'OPEN"D",#1,"R",32\rFIELD#1,6 AS B$\r'
     'GET#1,1\rPRINT"V<";B$;">"\rCLOSE#1\r',
     ["V<ABC   >"], ["ERROR"]),

    # RSET pushes the data to the right of the field; both blank fill
    # the rest of it.
    ("rset-justify", 18,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",32\rFIELD#1,6 AS A$\r'
     'RSET A$="ABC"\rPRINT"R<";A$;">"\r'
     'LSET A$="Z"\rPRINT"L<";A$;">"\rCLOSE#1\r',
     ["R<   ABC>", "L<Z     >"], ["ERROR"]),

    # Data at least as long as the field is truncated on the right,
    # whichever justification was asked for.
    ("lset-truncates", 16,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",32\rFIELD#1,4 AS A$\r'
     'RSET A$="ABCDEFG"\rPRINT"T<";A$;">"\rCLOSE#1\r',
     ["T<ABCD>"], ["ERROR"]),

    # Assigning a FIELDed variable to another one must copy the bytes
    # into string space, not hand over a pointer into the record.  If
    # it did, C$ would change underneath when the next GET arrived -
    # which is exactly what this checks.
    ("let-copies-field", 30,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",32\rFIELD#1,3 AS A$\r'
     'LSET A$="ONE"\rPUT#1,1\rLSET A$="TWO"\rPUT#1,2\r'
     'GET#1,1\rC$=A$\rGET#1,2\r'
     'PRINT"C";C$;" A";A$\rCLOSE#1\r',
     ["CONE ATWO"], ["ERROR"]),

    # A number goes into a record as the five bytes BASIC packs it
    # into, and comes back the same.  MKN$ of a value CVN cannot
    # recover would be useless, so the pair is tested together.
    ("mkn-cvn", 14,
     'CLEAR1000\rPRINT"N";LEN(MKN$(1234.5))\r'
     'PRINT"V";CVN(MKN$(1234.5))\rPRINT"W";CVN(MKN$(-0.125))\r',
     ["N 5", "V 1234.5", "W-.125"], ["ERROR"]),

    # CVN needs five bytes to work with.
    ("cvn-too-short", 10,
     'CLEAR1000\r10 PRINT CVN("AB")\rRUN\r',
     ["?FC ERROR"], []),

    # LOC on a direct file is the record it is at.  GET and PUT leave
    # the file positioned after the record they moved.
    ("loc-direct", 18,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",32\rFIELD#1,3 AS A$\r'
     'LSET A$="ABC"\rPUT#1,1\rPUT#1,5\r'
     'PRINT"L";LOC(1)\rCLOSE#1\r',
     ["L 6"], ["ERROR"]),

    # When a PUT moves the end of the file into a new sector, the count
    # of bytes used in the last sector has to start again for that
    # sector.  Record 8 of 32 fills sector 1 exactly (count 256); record
    # 9 opens sector 2 with 32 bytes.  If the old count survived, a GET
    # of record 10 - never written - would read stale bytes instead of
    # hitting the end of the file.
    ("getput-last-sector-resets", 16,
     'CLEAR1000\rDSKINI0\r'
     '10 OPEN"D",#1,"R",32:PUT#1,8:PUT#1,9:GET#1,10\rRUN\r',
     ["?IE ERROR"], []),

    # A record past the end of the drive is refused before anything is
    # allocated.  Without that a PUT to a wild record number would take
    # every free granule and only then fail.  The mock drive has 24
    # sectors; record 200 of 32 starts at sector 24.
    ("getput-record-too-far", 16,
     'CLEAR1000\rDSKINI0\r'
     '10 OPEN"D",#1,"R",32:PUT#1,200\rRUN\rPRINT"F";FREE(0)\r',
     ["?BR ERROR", "F 5"], []),

    # LOF on a direct file is the number of records in it, from the
    # bytes the file occupies over the record length.  Five records of
    # 32 in one sector is 160 bytes; record 9 opens a second sector.
    ("lof-direct", 22,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",32\rFOR I=1 TO 5:PUT#1,I:NEXT\r'
     'PRINT"L";LOF(1)\rPUT#1,9\rPRINT"M";LOF(1)\rCLOSE#1\r',
     ["L 5", "M 9"], ["ERROR"]),

    # A record that straddles a granule boundary.  On the mock geometry
    # a granule is three sectors, so record 8 of 100 bytes starts at
    # byte 700 - sector 2 of the first granule - and ends at 800, in the
    # first sector of the second.
    ("getput-cross-granule", 28,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",100\rFIELD#1,100 AS A$\r'
     'LSET A$="S"+STRING$(98,"M")+"E"\rPUT#1,8\rCLOSE#1\r'
     'OPEN"D",#1,"R",100\rFIELD#1,100 AS B$\rGET#1,8\r'
     'PRINT"X";LEFT$(B$,1);MID$(B$,50,1);RIGHT$(B$,1);LEN(B$)\r'
     'PRINT"F";FREE(0)\rCLOSE#1\r',
     ["XSME 100", "F 4"], ["ERROR"]),

    # A record longer than a sector, which needs FILES to make room for
    # it, and which spans three sectors and two granules from record 2.
    # All three fields are compared whole, in BASIC, and only the count
    # of matches is printed: the text screen has no lowercase, so a
    # scraped screen can never show one and the comparison has to happen
    # on the machine rather than here.
    ("record-multi-sector", 40,
     'CLEAR2000\rFILES 2,700\rDSKINI0\r'
     '10 OPEN"D",#1,"R",600\r'
     '20 FIELD#1,255 AS A$,255 AS B$,90 AS C$\r'
     '30 LSET A$="A"+STRING$(253,"a")+"1"\r'
     '40 LSET B$="B"+STRING$(253,"b")+"2"\r'
     '50 LSET C$="C"+STRING$(88,"c")+"3"\r'
     '60 PUT#1,2:CLOSE#1\r'
     '70 OPEN"D",#1,"R",600\r'
     '80 FIELD#1,255 AS D$,255 AS E$,90 AS F$\r'
     '90 GET#1,2:Q=0\r'
     '100 IF D$="A"+STRING$(253,"a")+"1" THEN Q=Q+1\r'
     '110 IF E$="B"+STRING$(253,"b")+"2" THEN Q=Q+2\r'
     '120 IF F$="C"+STRING$(88,"c")+"3" THEN Q=Q+4\r'
     '130 PRINT"Q";Q:CLOSE#1\rRUN\r',
     ["Q 7"], ["ERROR"]),

    # A variable that was never FIELDed has no slot in a record.
    ("lset-not-fielded", 12,
     'CLEAR1000\rDSKINI0\r10 A$="HI":LSET A$="X"\rRUN\r',
     ["?SE ERROR"], []),

    # The fields are cumulative and may not outgrow the record.
    ("field-overflow", 12,
     'DSKINI0\r10 OPEN"D",#1,"R",32:FIELD#1,20 AS A$,20 AS B$\rRUN\r',
     ["?FO ERROR"], []),

    # Only a direct file has a record buffer to divide up.
    ("field-wrong-mode", 12,
     'DSKINI0\r10 OPEN"O",#1,"S":FIELD#1,4 AS A$\rRUN\r',
     ["?FM ERROR"], []),

    ("direct-dir-type", 18,
     'CLEAR1000\rDSKINI0\r'
     'OPEN"D",#1,"R",32\rCLOSE#1\r'
     'OPEN"D",#1,"R",32\rCLOSE#1\r'
     'DSKI$0,1,3,A$,B$\r'
     'PRINT"T";ASC(MID$(A$,12,1));"F";ASC(MID$(A$,13,1))\r',
     ["T 1 F 255"], ["ERROR"]),
]


def run_once(name, wait, typing):
    cmd = [sys.executable, COCOTEST, "--rom", ROM, "--wait", str(wait)]
    if typing:
        cmd += ["--type", typing]
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              timeout=wait + 60).stdout
    except subprocess.TimeoutExpired:
        return ""


def run_case(name, wait, typing, expect, forbid):
    # The emulator occasionally fails to come up or to answer the debug
    # connection, which produces no screen at all.  That is nothing to
    # do with the ROM, so it gets more than one chance, with a pause
    # between: whatever is holding the machine up - a slow start under
    # load, a port not yet released - needs a moment to clear, and
    # retrying instantly tends to hit it again.  A run that reports a
    # failure nobody can reproduce is worse than a slow one.
    #
    # The test for "a screen came back" is the frame cocotest draws
    # around it, not anything in the screen itself: a longer case
    # scrolls the banner away.
    for attempt in range(3):
        if attempt:
            time.sleep(2)
        out = run_once(name, wait, typing)
        if out.count("+---") >= 2:
            break
    else:
        return False, "the emulator produced no screen at all", out
    flat = "\n".join(line.strip("|").rstrip() for line in out.splitlines())
    for want in expect:
        if want not in flat:
            return False, "missing %r" % want, out
    for bad in forbid:
        if bad in flat:
            return False, "unexpected %r" % bad, out
    return True, "", out


def main():
    wanted = sys.argv[1:]
    cases = [c for c in CASES if not wanted or any(w in c[0] for w in wanted)]
    failures = []
    for name, wait, typing, expect, forbid in cases:
        ok, why, out = run_case(name, wait, typing, expect, forbid)
        print("%-16s %s%s" % (name, "ok" if ok else "FAILED",
                              "" if ok else "  " + why), flush=True)
        if not ok:
            failures.append((name, out))
    print("\n%d of %d passed" % (len(cases) - len(failures), len(cases)))
    for name, out in failures:
        print("\n--- %s ---\n%s" % (name, out))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
