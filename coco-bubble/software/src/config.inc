*******************************************************************
* config.inc - build-time configuration for the bubble ROM
*
* Geometry defaults are byte-for-byte DECB parity.
* The code is always DECB-shaped; only these constants change.
*
* Define MOCK=1 on the lwasm command line to build the emulator
* test image (RAM-backed storage, shrunken geometry).
*******************************************************************

                ifndef  MOCK
MOCK            equ     0
                endc

* ---------------------------------------------------------------
* Multi-Pak Interface
* ---------------------------------------------------------------
* Multi-Pak support is not a build option: mpi.asm detects at cold
* start whether an MPI is present and which slots hold the bubble
* and a floppy controller, so one ROM covers a bare cartridge port
* and every MPI arrangement.
*
* SCS* has to be routed because it reaches only one slot: with the
* switch on the slot holding the ROM, a bubble board in a different
* slot is not addressable.  CTS* and SCS* are independent nibbles
* of the latch, so CTS* stays where it is - the ROM keeps serving
* and CART* keeps its autostart - and SCS* moves to whichever slot
* the device is in.  A floppy controller is reached the same way:
* SCS* moves to its slot for the operation and back afterwards.
*
* The 26-3024 service manual puts the slot select latch at $FF9F;
* essentially all CoCo software uses $FF7F, and on a CoCo 3 $FF9F
* collides with the GIME, which is very likely why it moved.
* Change this if a particular MPI disagrees.
MPISLOT         equ     $FF7F           slot select latch

* ---------------------------------------------------------------
* Build stamp
* ---------------------------------------------------------------
* The Makefile hands in a 16 bit checksum of the sources, which the
* boot screen prints as "BUILD xxxx".  Assembling by hand, without
* the Makefile, leaves it zero.
                ifndef  BUILDID
BUILDID         equ     0
                endc

* ---------------------------------------------------------------
* Geometry
* ---------------------------------------------------------------
SECLEN          equ     256             logical sector size (bytes)

                if      MOCK
* --- MOCK profile: RAM-backed pseudo-disk for emulator testing ---
* Small enough to sit in a 32K machine below the BASIC workspace,
* but with a dead zone and several granules so that allocation,
* chaining and the dead zone all get exercised.
SECTRK          equ     12              sectors per track
TRKMAX          equ     3               number of tracks
SECGRAN         equ     3               sectors per granule
DIRTRK          equ     1               directory/FAT track
DIRSEC1         equ     3               first directory sector
DIRSECN         equ     6               last directory sector
PHYSSEC         equ     30              backed sectors; 30..35 are dead
MOCKBUF         equ     $5000           RAM storage base
MOCKSIZE        equ     TRKMAX*SECTRK*SECLEN
                else
* --- DEFAULT profile: DECB byte-for-byte parity ---
SECTRK          equ     18              sectors per track
TRKMAX          equ     35              number of tracks
SECGRAN         equ     9               sectors per granule
DIRTRK          equ     17              directory/FAT track
DIRSEC1         equ     3               first directory sector
DIRSECN         equ     11              last directory sector
PHYSSEC         equ     512             128KB / 256 = 512 sectors backed
                endc

* ---------------------------------------------------------------
* Derived geometry (do not edit)
* ---------------------------------------------------------------
GRANTRK         equ     SECTRK/SECGRAN          granules per track (2)
GRANMX          equ     (TRKMAX-1)*GRANTRK      total granules (68)
FATCON          equ     6                       FAT control bytes before data
FATLEN          equ     FATCON+GRANMX           FAT image length (74)
DIRLEN          equ     32                      directory entry length
DIRENTS         equ     (DIRSECN-DIRSEC1+1)*(SECLEN/DIRLEN)  max entries (72)
FCBLEN          equ     SECLEN+25               FCB length (281)

* Dead-zone marker: a FAT entry that is neither free ($FF) nor a
* legal link (0..GRANMX-1) nor a legal terminator ($C1..$C9).
DEADGR          equ     $FE

* ---------------------------------------------------------------
* Build-time sanity asserts
*
* lwasm reads '>' as its force-direct-page prefix and has no
* comparison operators, so "a greater than b" is written as the
* integer division a/(b+1), which is zero exactly when a <= b.
* ---------------------------------------------------------------
* The GAT link encoding allows up to 192 granules, but the FAT is
* indexed with an accumulator offset, which the 6809 treats as
* signed, so the practical ceiling is 127.  DECB's 68 is well under
* either limit.
                if      GRANMX/128
                error   "GRANMX exceeds 127 - accumulator offsets are signed"
                endc
                if      SECGRAN/16
                error   "SECGRAN exceeds 15 - breaks $C0+n terminator encoding"
                endc
                if      SECTRK%SECGRAN
                error   "SECGRAN must divide SECTRK evenly"
                endc
                if      DIRTRK/TRKMAX
                error   "DIRTRK outside the geometry"
                endc
                if      PHYSSEC/(TRKMAX*SECTRK+1)
                error   "PHYSSEC larger than the virtual geometry"
                endc
                if      MOCK
                else
* The 7110 has 2048 pages; at 4 pages per sector the bubble address
* register can only span 512 sectors, and BBPARM does not mask.
                if      PHYSSEC/513
                error   "PHYSSEC exceeds the 7110's 2048 pages"
                endc
                endc

* ---------------------------------------------------------------
* Hardware
* ---------------------------------------------------------------
* PIA0 control register B.  Bit 0 enables the 60Hz VSYNC interrupt,
* which is what keeps TIMER running; cold start has to set it.
PIA0CRB         equ     $FF03

BBDATA          equ     $FF40           7220 data FIFO
BBSTAT          equ     $FF41           7220 command (w) / status (r)

* Tandy floppy controller.  Note DSKREG shares $FF40 with the
* bubble's data port - they are never both reachable, because SCS*
* goes to one slot at a time and that is what fdc.asm routes.
DSKREG          equ     $FF40           control latch, write only
FDCCMD          equ     $FF48           command (w) / status (r)
FDCTRK          equ     $FF49           track register
FDCSECR         equ     $FF4A           sector register
FDCDATA         equ     $FF4B           data register

* DSKCON status byte, shaped like the WD1793 status that machine
* language utilities expect to find there.
ST_NOTRDY       equ     $80             drive not ready
ST_WP           equ     $40             write protected
ST_RNF          equ     $10             record not found
ST_CRC          equ     $08             data error
* A verify mismatch is not a WD1793 status at all - it is the result
* of a comparison this ROM makes.  Bit 1 is the controller's DRQ,
* which is transient and never survives into DCSTA, so it is free.
ST_VFY          equ     $02             read back did not match

* Drive numbering.  Drive 0 is the bubble; floppies are 1 upward.
* Four FAT images exist, so the range is 0-3: three floppies.
BUBDRV          equ     0               the bubble
FDCDRVS         equ     3               floppies: drives 1..3

* 7220 commands
BBC_ABORT       equ     $19
BBC_FIFORST     equ     $1D
BBC_INIT        equ     $11
BBC_RDDATA      equ     $12
BBC_WRDATA      equ     $13
* (Other Table 4 commands - seeks, bootloop access, MBM purge,
* software reset - are not used by this driver.)

* 7220 register access code: set register address counter to RAC
BBR_BLRLSB      equ     $0B             block length register LSB address

* 7220 parametric values
BBV_BLRMSB      equ     $10             NFC=0001: two FSA channels (one
*                                       7110 through its 7242); low
*                                       nibble = page count bits 11-8
BBV_ENABLE      equ     $40             ICD (error correction) only:
*                                       no parity/IRQ/DMA interrupts
* Reads add MFBTR (bit 3).  Datasheet table 8: with MFBTR=0 a one
* bubble Read requires the host to drain the FIFO at 50K bytes/sec
* on the last page - 20us/byte, which a 0.89MHz 6809 polling loop
* cannot do.  MFBTR=1 slows the FSA-to-BMC burst to 12.5K bytes/sec
* (80us/byte), which it comfortably can.  Intel's note: MFBTR must
* be 0 for every command except Read Bubble Data.
*   On a pre-1983 7220 (not the 7220-1) bit 3 was "Low Freq" and
* meant something else; if reads fail on such a chip set this to
* $40, at which point reads need a faster host than a 6809.
BBV_ENABRD      equ     $48             ICD + MFBTR, for reads only

* 7220 status bits
BBS_BUSY        equ     $80
BBS_OPC         equ     $40             opcode complete
BBS_FIFO        equ     $01             FIFO ready

* Bubble page size and pages per logical sector
BBPAGE          equ     64
BBPPS           equ     SECLEN/BBPAGE   pages per sector (4)
