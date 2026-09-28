# Setting up the emulator test environment

How the XRoar-based test environment was built on a Linux host (Ubuntu
24.04, without root - everything goes in `~/bin` and `~/.xroar`), so it
can be reproduced from scratch.

Prerequisites: `gcc`, `make`, `curl`, `python3`.

## 1. lwasm (lwtools 4.22)

```
cd /tmp
curl -sL -o lwtools.tar.gz http://www.lwtools.ca/releases/lwtools/lwtools-4.22.tar.gz
tar xzf lwtools.tar.gz
cd lwtools-4.22
make -j4
mkdir -p ~/bin
cp lwasm/lwasm lwlink/lwlink lwar/lwar ~/bin/
~/bin/lwasm --version        # "lwasm from lwtools 4.22"
```

## 2. XRoar 1.8.2, headless with the GDB target

```
cd /tmp
curl -sL -o xroar.tar.gz https://www.6809.org.uk/xroar/dl/xroar-1.8.2.tar.gz
tar xzf xroar.tar.gz
cd xroar-1.8.2
./configure --prefix=$HOME \
    --without-gtk2 --without-gtk3 \
    --without-alsa --without-pulse --without-oss --without-sndfile \
    --enable-gdb-target
make -j4
cp src/xroar ~/bin/
```

Notes:
- `--enable-gdb-target` is the whole point: the test harness reads
  the emulated machine's memory over the GDB remote protocol.
- Without SDL2 installed, configure finds no video/audio backends
  beyond the null modules - which is exactly what headless use needs.
  (`--without-sdl` is not a recognised option; harmless to omit.)
- Do not probe options with `xroar -ui help` style invocations - some
  of them start the emulator instead of printing a list and sit there
  forever.  `xroar -h` prints everything safely.

## 3. The BASIC ROMs

XRoar needs the real Color BASIC ROMs.  A ToolShed source checkout
carries byte-exact reassemblies in its `cocoroms/` directory (CB 1.3,
ECB 1.1, DECB 1.1):

```
mkdir -p ~/.xroar/roms
cp <toolshed>/cocoroms/{bas13.rom,extbas11.rom,disk11.rom} ~/.xroar/roms/
```

At startup XRoar checksums them; a good install logs
`Colour BASIC CRC32 valid` and `Extended Colour BASIC CRC32 valid`.
The MOCK suite needs only the first two.  `disk11.rom` is what
`--cart-type rsdos` loads for the floppy tests, and what a
comparison run against real Disk BASIC uses.

## 4. How the harness invokes it

`test/cocotest.py` does all of this itself; for reference, the shape
of a headless run is:

```
xroar -machine coco2b -ui null -ao null \
      -cart-rom build/bubble-mock.rom -cart-autorun \
      -gdb -gdb-port 65520 \
      -type 'DSKINI0\rDIR\r'         # keystrokes typed into BASIC
```

- `-type` injects keystrokes; the harness reads results back by
  halting over GDB and dumping video RAM at $0400 (32x16 screen).
- The GDB stub answers memory reads even when the interrupt request
  gets no stop-reply - the harness treats a missing reply as fine.
  Register packet layout ('g') is CC A B DP X Y U S PC, followed by
  'x' characters for registers it does not report.
- **A killed emulator keeps port 65520**, which makes the next run
  fail mysteriously.  `cocotest.py` kills stray xroar processes and
  waits for the port before starting; if driving XRoar by hand, do
  the same with `pkill -9 -x xroar`.  Match on the process name, not
  the command line: over ssh a `pkill -f` pattern also matches the
  remote shell that is running it, and kills the session.
- Never run two emulator sessions (or the suite plus a manual run)
  at once - they fight over the port and kill each other.

## 5. Verify

```
cd software
PATH=$HOME/bin:$PATH make
python3 test/suite.py            # expect: 90 of 90 passed (~15 min)
```

If the source tree is on a network share whose clock differs from the
build host's, make may print clock-skew warnings (harmless) or say
"Nothing to be done" after an edit; `touch src/*` forces a rebuild.

## 6. Optional: ToolShed decb for the interop test

`test/interop.py` checks the media format against ToolShed's `decb`.
From a ToolShed source checkout:

```
cd <toolshed>/build/unix/decb
make -j4
cd <this repo>/software
python3 test/interop.py <toolshed>/build/unix/decb/decb
```

If the checkout is in `/tmp` the build disappears on reboot; copy
`decb` into `~/bin` to keep it.
