# How it works

An architectural guide to the CoCo multicart and videocart firmware, for
someone who wants to change it.

This is not the README — it assumes you have the thing built and running,
and want to know *why* the code is shaped the way it is before you start
moving it around. Most of the design is dictated by two unforgiving sets
of constraints pressed against each other: a 6809E bus that will not wait,
and an RP2350 whose useful tricks all come with sharp edges. Nearly every
structure in this codebase exists because one of those two pushed back.

Read sections 2 and 3 first even if you only care about one board. Almost
every "why on earth is it done like *that*" in sections 4–8 is answered
there.

**Contents**

1. [The two boards at a glance](#1-the-two-boards-at-a-glance)
2. [The CoCo side](#2-the-coco-side)
3. [The RP2350 facts that shape everything](#3-the-rp2350-facts-that-shape-everything)
4. [The multicart bus engine](#4-the-multicart-bus-engine)
5. [The videocart bus engine](#5-the-videocart-bus-engine)
6. [Video: mirror to HDMI](#6-video-mirror-to-hdmi)
7. [Audio: PIA writes to HDMI data islands](#7-audio-pia-writes-to-hdmi-data-islands)
8. [Shared subsystems](#8-shared-subsystems)
9. [Modifying it](#9-modifying-it)
10. [Appendix: quick reference](#10-appendix-quick-reference)

---

## 1. The two boards at a glance

Both boards are a Raspberry Pi Pico 2 W (RP2350) in a CoCo cartridge
shell, with an SSD1306 OLED, a rotary encoder behind a PCF8574 I/O
expander, and a 74HCT125 that lets the Pico pull the CoCo's `!RESET` and
drive its `CART*` autostart line. Everything above the bus is shared code.

|  | multicart | videocart |
|---|---|---|
| Sees the address bus | directly, A0–A13 on GP0–13 | through three 74HCT153 muxes, swept in 4 phases |
| Serves ROM bytes | core-1 CPU loop | PIO + DMA, no CPU |
| Sees writes | a hardware strobe on `$FF40–$5F` only | every bus cycle |
| RAM mirror | no | 64 KB |
| Video / audio out | no | HDMI 640×480@60 + 48 kHz PCM |
| Max image | 128 KB (8 banks) | 32 KB (2 banks) |
| Behind a Multi-Pak | works | **no** — see below |

The videocart's whole premise is snooping, and most Multi-Pak Interface
implementations buffer the cartridge bus so a slot only sees cycles
addressed to it. Serving still works there; mirroring does not. Plug the
videocart straight into the machine.

```
                    ┌──────────────────────── Pico 2 W (RP2350) ───────┐
   CoCo cartridge   │                                                  │
   port             │   core 1: bus service (RAM-resident, IRQs off)   │
   ───────────────► │   core 0: everything else                        │
   A0..A15  D0..D7  │                                                  │
   E  Q  R/W        │   PIO0  reserved for the CYW43 WiFi driver       │
   CTS* SCS* CART*  │   PIO1  \  bus engine                            │
   !RESET SND       │   PIO2  /                                        │
                    │   DMA   snoop rings, serve lookup, HDMI scanout  │
                    │   HSTX  GP12..19 ──────────────────► HDMI (vc)   │
                    │   I2C   ──► SSD1306 OLED + PCF8574 encoder       │
                    │   CYW43 ──► WiFi: web UI, OTA                    │
                    │   QSPI  ──► 4 MB flash: firmware A/B + LittleFS  │
                    └──────────────────────────────────────────────────┘
```

---

## 2. The CoCo side

### 2.1 What a cartridge sees

The CoCo's cartridge port carries the full 6809E bus: A0–A15, D0–D7, the
`E` and `Q` clocks, `R/W`, `+5V`, and four signals that matter more than
the rest:

| Signal | Direction | Meaning |
|---|---|---|
| `CTS*` | in | The SAM asserts this for reads of `$C000–$FEFF`. "The cartridge ROM window is being read." |
| `SCS*` | in | Asserted for `$FF40–$FF5F`. The disk-controller / cartridge I/O range. |
| `CART*` | out | The cart pulls this to raise a FIRQ. This is how a cartridge announces itself. |
| `!RESET` | bidir, open drain | Pull it to reset the machine. |

Both boards drive `CART*` and `!RESET` through a 74HCT125 whose enables
come from the PCF8574 (`MC_XP_CART_EN`, `MC_XP_RESET_EN`, both active
low). That is the entire mechanism by which selecting a game in a web
browser reboots a 1981 computer.

### 2.2 The bus cycle

Everything on both boards is timed against this. At 0.895 MHz the cycle
is **1117 ns**:

```
        │◄────────────── one bus cycle, 1117 ns ──────────────►│
        │                                                       │
   E ───┐                          ┌────────────────────────────┐
        │                          │                            │
        └──────────────────────────┘                            └────
        0                        558                          1117
        │                          │                            │
        │  E low: the SAM owns the │  E high: the CPU owns the  │
        │  bus for its VDG fetch.  │  bus. A cartridge read is  │
        │  Address for the CPU's   │  answered in here.         │
        │  cycle becomes valid.    │                            │
        │                          │                            │
   addr ·······<valid>─────────────────────────────────────────────
                                                    ▲
   READ:  our data must be on the pins by ──────────┘ ~1037 ns
          (80 ns of 6809E data setup before E falls)

   WRITE: the CPU's data is valid from ~E-rise+225 ns
          and is latched by the CoCo on the falling edge of E
```

Two deadlines fall out of that, and they are the hinges of both designs:

- **Serving a read:** the byte must be driven before ~fall+1037 ns.
- **Capturing a write:** sample D0–D7 somewhere after ~fall+780 ns and
  before E falls. Both boards use a configurable delay (`strobe_delay_ns`
  in cfg) so this can be scope-tuned; the default is 600 ns after the
  strobe on the multicart, 400 ns after E-rise on the videocart.

### 2.3 The SAM, and the CTS\* approximation

The 6883 SAM generates `CTS*`/`SCS*` and is also the video address
generator. Two of its behaviours leak into the firmware:

- **The display window.** SAM registers `$FFC0–$FFD3` are *address-pair*
  registers: writing to the even address of a pair clears that bit,
  writing to the odd address sets it. The data byte is meaningless. V0–V2
  select the scan geometry; F0–F6 are the display offset in 512-byte
  units. The videocart's renderer decodes these by address.
- **Map type.** `$FFDE` selects map type 0 (ROM visible); `$FFDF` selects
  type 1, where `$C000–$FEFF` becomes RAM and the SAM stops asserting
  `CTS*` at all. The multicart sees this correctly because it gates on
  the real `CTS*` pin. The videocart *approximates* `CTS*` with
  `A14 & A15`, because `CTS*` is E-qualified and its sampler runs during
  E-low — so in map type 1 the videocart would keep driving into the
  CoCo's own RAM reads. It tracks the TY bit and reports it on the
  diagnostics panel, but deliberately does **not** act on it (§5.8).

### 2.4 How a cartridge starts

There are two entirely different paths, and the difference is the single
most productive thing to understand about this firmware.

```
  RESET PATH                          INSERTION PATH
  ──────────                          ──────────────
  machine resets                      BASIC boots all the way to READY
  BASIC's cold init runs partway      user pushes a cart in
  it checks $C000 for "DK"            CART* falls -> FIRQ
  LBEQ $C002                          ROM handler at $A0F6:
                                        CLR <$71
  cart starts with BASIC              JMP $C000
  HALF INITIALISED                    cart starts with BASIC
                                      FULLY INITIALISED
```

- An autostart cart **must** begin with the bytes `44 4B` ("DK") at
  `$C000`; its entry point is `$C002`.
- On the insertion path the ROM handler jumps to `$C000`, not `$C002` —
  the "DK" bytes are *executed*. `$44` is `LSRA` and `$4B` is harmless,
  so control falls through into the real entry. That is by design; do not
  "fix" it.
- **The warm-start flag at `$0071`.** Pulling `!RESET` is electrically the
  reset button, i.e. a *warm* start, and a warm start skips the "DK"
  check entirely. So selecting a cart has to force a cold boot. That is
  what the 6809 stub exists for (§8.5).

Cassette games converted to cartridges were written for the machine a
`CLOADM` leaves behind, and several only work via the insertion path.
Hence the per-ROM `insert` flag and the `SEL_WAIT_BASIC` state in §8.4.

### 2.5 Bank switching

The window `$C000–$FEFF` is 16128 usable bytes (`$FF00–$FFFF` is I/O).
The near-universal scheme for larger images — CoCoFLASH, FlashPak, our
`BUS_SCHEME_BANK16K` — is: **write the bank number as data to `$FF40`**.
Low bits select the bank.

Two consequences the firmware has to live with:

- The switching code **cannot run from `$C000`**, because the window (and
  therefore the code) swaps underneath it. Every banked image here copies
  itself to RAM first, or relocates a small switcher.
- Any program that writes `$FF40–$5F` for other reasons (a disk register
  reset, say) will move the bank. Harmless for a RAM-resident game, but
  worth remembering as a class of hazard.

---

## 3. The RP2350 facts that shape everything

If you only skim one section, skim this one.

### 3.1 Two cores, and the core-1 contract

Core 0 does everything a normal embedded program does: config, LittleFS,
the OLED and encoder, WiFi and lwIP, the HTTP server, the USB console,
the selection state machine, and (on the videocart) the renderer.

Core 1 does exactly one thing: **the bus**. Its contract is absolute:

> Core 1 runs with interrupts disabled forever, and every instruction it
> executes and every byte it touches lives in SRAM. It never reads flash.

That is what lets core 0 erase and program flash — for a ROM upload, a
config save, an OTA — **without pausing core 1**. A running game does not
die because somebody uploaded a ROM from a browser.

The enforcement is `__not_in_flash_func` / `__no_inline_not_in_flash_func`
plus static arrays, and it is **verified on the linked image** by
`make audit`, not trusted from the source. This matters:

- `__not_in_flash_func` does *not* stop GCC inlining a static function
  into a flash-resident caller, which silently relocates the whole thing
  back into flash. Use `__no_inline_not_in_flash_func` for anything that
  must stay put.
- GCC will rewrite a hand-written byte-copy loop into a call to `memcpy`
  — which is in flash. A `volatile` destination pointer blocks the
  pattern match. `ota.c`'s slot-B-to-A copier needs both tricks; it
  erases the very flash it was called from, so a single flash reference
  would hard-fault mid-update and brick the board.
- `flash_safe_execute()` is unusable here. It parks the other core
  through a lockout IRQ, and our core 1 has interrupts off forever, so it
  could never answer. `cfg.c` and `romfs.c` use plain
  `save_and_disable_interrupts()` + `flash_range_*` on core 0 instead,
  which is sufficient *precisely because* core 1 is provably XIP-free.

### 3.2 SRAM, striping, and the scratch banks

520 KB: eight 64 KB banks that are interleaved so that different bus
masters usually land in different banks and do not serialise, plus two
4 KB scratch banks (`SCRATCH_X`, `SCRATCH_Y`) that sit outside the
stripe and arbitrate separately.

The SDK exposes the scratch banks as `__scratch_x("name")` /
`__scratch_y("name")`. They are the right home for something **small and
read on every single lap**; they are the wrong home for anything that
wants to be *big*. The videocart learned this the expensive way: its
snoop rings started in scratch to keep their DMA writes away from the
serve chain, but 4 KB capped them at 64 bus cycles of history, and a few
milliseconds of core-0 SRAM hammering overflowed them. They moved to
striped SRAM at 256 cycles deep, and the 1 KB address-transpose LUTs —
small, hot, read twice per lap — took their place in scratch.

Videocart static footprint, roughly:

```
   s_main    serve tables + image staging   96 KB   (32 KB aligned!)
   s_fb      framebuffer + inline commands  128 KB
   s_mirror  64 K shadow of CoCo RAM         64 KB
   s_stub    cold-boot stub window           16 KB
   rings     snoop + video + audio           17 KB
   s_desc    HDMI descriptor table           10 KB
   island slots, audio logs, PCM capture     20 KB
   lwIP pools, stacks, misc                 ~95 KB
                                            ──────
                                            ~460 KB of 520 KB
```

There is not much room. `MC_IMAGE_BUF_BYTES` is the lever if you need
some: it was cut from 128 KB to 96 KB to pay for the audio pipeline,
which is why the videocart tops out at two banks.

### 3.3 Bus fabric priority

`bus_ctrl_hw->priority` has one bit each for PROC0, PROC1, DMA_R and
DMA_W. Setting a bit puts that master in the high-priority round-robin
group; everything else yields to it.

Both boards elevate **PROC1**, so WiFi DMA bursts cannot stall core 1's
SRAM reads past the CoCo's deadline. The videocart also elevates DMA_R
and DMA_W for the HDMI scanout FIFO.

```c
/* videocart/hstx.c */  bus_ctrl_hw->priority |= DMA_W | DMA_R;
/* videocart/bus.c  */  bus_ctrl_hw->priority |= PROC1;
```

**Both use `|=`, and that is load-bearing.** `hstx_init()` runs first and
`bus_init()` runs later; a plain `=` in either one silently wipes the
other's bits. The symptom is intermittent HDMI sparkle that gets worse
when the CoCo is busy — i.e. it looks exactly like a TMDS routing or
connector fault, and you will chase it for a day.

One more trap: **the priority covers SRAM and bus-fabric accesses, not
APB reads.** A PIO FIFO read is an APB read. That single fact is why both
boards drain their PIO FIFOs with DMA into SRAM and have their hot loops
read SRAM instead (§4.4, §5.7).

### 3.4 PIO

Three blocks (`pio0`, `pio1`, `pio2`), four state machines each, and —
the one that bites — **32 instruction words per block, shared by every
program in it**. `pio_add_program()` *panics at runtime* when a program
does not fit; the assembler only range-checks each program on its own, so
you get a dead board rather than a build error.

The allocation is therefore deliberate:

| Block | Contents | Words |
|---|---|---|
| `pio0` | **left empty** | 0 |
| `pio1` | `mux_sampler` alone (videocart) | 24 / 32 |
| `pio2` | `sweep_follower` 14 + `wr_capture` 8 + `bus_serve` 9 | 31 / 32 |

`pio0` is kept free because `cyw43_arch_init()` claims the first free
state machine it finds. Leaving it a whole block keeps WiFi's DMA bursts
off the same PIO slave port as the bus engine. For the same reason both
boards call `pio_sm_claim()` and `dma_claim_unused_channel()` **before**
WiFi comes up — otherwise the driver can take a state machine out from
under you and the failure is silent.

`make preflight` runs the assembler and fails the build if the budget
above stops holding. Check it before you add an instruction.

Two encoding details that constrain the videocart's sampler:

- A PIO instruction's top 5 bits are shared between the **side-set value
  and the delay**. `mux_sampler` side-sets 2 bits (the mux SEL lines), so
  only 3 bits are left for delay — a maximum of 7. `SAMP_SETUP0` is
  already 7. **The sweep's timing constants cannot be scaled**, which is
  why raising the system clock required pinning the PIO clock instead
  (§3.7).
- `in osr, n` copies OSR's low bits *without consuming them*;
  `out null, n` consumes without storing. The sampler uses that pair to
  snapshot the pins once per phase and then mine the snapshot, so the
  mux SEL lines can flip on the very next instruction and the '153 settle
  time overlaps the bit extraction instead of costing dead time.

### 3.5 DMA

Sixteen channels. The features this firmware leans on:

- **Chaining.** A channel can start another when it completes. Two
  channels chained to each other run forever.
- **Rings.** A channel's address wraps within a power-of-two region —
  but a channel has **one** ring, on the read side *or* the write side,
  not both. `channel_config_set_ring()` overwrites. The HDMI scanout
  needs its ring on the write side (to keep hitting the same four
  registers), so the read-side rewind needs a whole third channel.
- **Trigger aliases.** Writing a channel's `al3_read_addr_trig` sets its
  read address *and* starts it. This is how the videocart turns a PIO
  push into a table lookup with no CPU (§5.5).
- **`TRANS_COUNT` modes** (an RP2350 addition): `NORMAL`, `TRIGGER_SELF`,
  `ENDLESS`. An `ENDLESS` channel never decrements and never completes.
- **`IRQ_QUIET`.** Per-channel. The SDK's default config leaves it
  *clear*, so every completing transfer raises the channel interrupt.
  On the videocart only the ~80 data-island descriptors per frame may
  interrupt; forgetting `IRQ_QUIET` on the other ~570 turns a 3.3 kHz
  refill into a 31.5 kHz one that rewrites island buffers mid-scan. The
  observed result was garbage islands (so the sink never entered HDMI
  mode and drew the guard bands as a bright column down the left edge),
  audio drained nine times too fast, and core 0 so starved that the web
  server stopped answering while ICMP still replied.

Channel budget on the videocart: 2 snoop pairs × 2 + 2 serve + 3 scanout
= **9 of 16**, before the CYW43 driver takes its own.

### 3.6 HSTX

The RP2350's High-Speed Serial Transmit block, fixed to GP12–19. You push
32-bit words at it; it shifts them out across up to 8 pins, and it can
optionally run a TMDS encoder and a "command expander" on the way.

The videocart's configuration:

- `CSR`: `CLKDIV=5`, `N_SHIFTS=5`, `SHIFT=2`. Five shifts of two bits =
  10 bits per item, DDR, so `clk_hstx` 125 MHz → 250 Mbps per lane →
  **25 MHz pixel clock**, which is exactly what 640×480p60 wants.
- `EXPAND_SHIFT`: `ENC_N_SHIFTS=4, ENC_SHIFT=8` — four RGB332 pixels per
  32-bit word; `RAW_N_SHIFTS=1, RAW_SHIFT=0` — a raw item is one whole
  word.
- `EXPAND_TMDS`: lane 2 (red) takes bits [7:5] (`ROT=0`), lane 1 (green)
  [4:2] (`ROT=29`), lane 0 (blue) [1:0] (`ROT=26`). The `RGB332()` macro
  in `video.h` must agree with this or red and blue swap on the display.
- The **bit crossbar** maps HSTX output bits to pins, with an invert flag
  per bit. This board's connector runs negative-leg-first
  (GP12 = TXC−, GP13 = TXC+, …), so the even bit of each pair carries
  `INV`. The pico-examples DVI Sock ordering is different — do not copy
  its table.

The command stream is what makes a whole video frame possible with no
CPU. A word's top nibble is a command:

| Command | Meaning |
|---|---|
| `RAW` | the next *n* words are emitted literally, one per clock |
| `RAW_REPEAT` | the next *one* word is emitted *n* times |
| `TMDS` | the next *n/4* words are RGB332 pixels, TMDS-encoded |
| `NOP` | consumes no pixel clocks |

So a scanline is a short list of `RAW_REPEAT` runs (front porch, sync,
back porch) followed by a `TMDS` run of 640 pixels — and a whole frame is
just a long enough list of those.

### 3.7 Clocks: why 250 MHz, and what is pinned

**Both boards run `clk_sys` at 250 MHz, and on both it is a requirement
rather than a tweak.**

- *Multicart.* The core-1 serve loop samples `!CTS` once per lap.
  Instruction counting predicted 150–250 ns; the measured lap at 150 MHz
  was **374 ns**, against a `!CTS` window of only ~636 ns. We drive
  ~160 ns after the sample that sees `!CTS` low and the CoCo latches
  ~478 ns after `!CTS` asserts, so a sample must land in roughly the
  first 318 ns — at 374 ns spacing that is luck, about 85 % of the time.
  A bare CoCo's margin hides it. Behind a Multi-Pak, `!CTS` arrives
  ~40 ns late through a '367 and a '139 and it stops hiding: intermittent
  `$FF` reads, immune to drive strength and to WiFi. At 250 MHz the lap
  is ~224 ns, *shorter than the window*, so a sample lands in time on
  every access instead of most of them.
- *Videocart.* The core-1 snoop loop has to finish inside the 1117 ns bus
  cycle. At 125 MHz the worst measured body was 1336 ns; it overran, lost
  the lap, and with it that cycle's screen byte or latched register write
  — thousands of times a minute, for months, with no error anywhere. At
  250 MHz the same work is ~668 ns.

Because the videocart's PIO timing is expressed in cycles that were
proven against real 6809 address setup, **nothing timing-critical is
allowed to follow `clk_sys`**:

```
   clk_sys ──── 250 MHz ──┬── core 0, core 1, DMA, SRAM
                          │
                          ├── clk_hstx  pinned to 125 MHz  (÷2)
                          │   └─ ÷5 in HSTX = 25 MHz pixel clock
                          │      (the divider field is 2 bits: ÷1, ÷2, ÷3 only)
                          │
                          └── PIO SMs   pinned to MC_PIO_REF_KHZ = 125 MHz
                              via bus_pio_div() = clk_sys / PIO_REF
                              (must be an INTEGER — a fractional divider
                               dithers the cycle length and the whole
                               point of the sweep is four exact cycles)
```

`config.h` static-asserts both relationships and `test_sampler_timing`
calls the firmware's own `bus_pio_div()` at several clock rates and
checks the answer. If you change `MC_SYS_CLK_KHZ`, that test is the thing
that will tell you what you broke.

### 3.8 Pad rules

- **Erratum E9.** The RP2350 resets with the **pull-down enabled on every
  pad**, and `gpio_init()` does not clear it. A pad held high by a weak
  source (a 4.7 kΩ pull-up, say) against an enabled pull-down can latch.
  Every bus pin, the I²C pins and the mux inputs get an explicit
  `gpio_disable_pulls()`. Do not remove those calls.
- **5 V.** The board design relies on the digital pads (GP0–25) tolerating
  the CoCo's 5 V levels; GP26–28 are the ADC-capable pads and carry only
  3.3 V signals the Pico itself drives (the mux selects and I²C).
- **Drive strength.** D0–D7 are set to 8 mA / fast slew, applied *after*
  `pio_gpio_init()` has taken the pins. This is the value validated on
  real hardware; raising it on an unbuffered, unterminated cartridge bus
  trades hypothetical setup margin for real reflections and ground
  bounce.

---

## 4. The multicart bus engine

### 4.1 Shape

```
   A0..A13 ─────► GP0..13  ┐
   !CTS    ─────► GP22     ├─► core 1 hot loop ──► GP14..21 = D0..D7
   D0..D7  ◄────► GP14..21 ┘         ▲
                                     │ reads s_strobe (SRAM)
   STROBE  ─────► GP28 ──► PIO ──► DMA ──► s_strobe
   ('125 + diode: falls only on a write to $FF40-$5F)
```

### 4.2 The serve loop

`multicart/bus.c`'s `bus_loop()` is the PicoROM recipe adapted to
bufferless drive. Every lap, unconditionally:

```c
uint32_t in = gpio_get_all();                       /* one read: addr + !CTS */

/* ... bank/stub decode from s_strobe, on change only ... */

uint32_t d = base[bank_base + (in & MC_ADDR_MASK)]; /* SRAM lookup   */
gpio_put_masked(MC_DATA_MASK, d << MC_PIN_D0);      /* drive value   */
gpio_set_dir_masked(MC_DATA_MASK,                   /* drive dirs    */
                    (in & (1u << MC_PIN_CTS)) ? 0u : MC_DATA_MASK);
```

Three things are worth noticing:

1. **It is continuous-refresh, not edge-triggered.** There is no waiting
   for `!CTS`; the loop re-drives every lap and the direction follows the
   live `!CTS` level. A stale address self-corrects on the next lap.
2. **The CPU owns both the value and the direction.** A pin's function
   mux selects a single source for both, so PIO cannot own `pindirs`
   while the CPU owns the data. This is why the data bus is SIO-driven
   on this board and PIO-driven on the videocart — each board picks one
   owner and sticks to it.
3. **Nothing in the loop touches the APB.** See §4.4.

### 4.3 The timing argument, and how to redo it

The census that produced the 250 MHz decision is worth repeating if you
change the loop: instrument the loop to count how many times it samples a
given access, and compare the lap time against the `!CTS` window. The
failure mode is not a crash — it is a cart that boots on your bench and
fails in somebody's Multi-Pak. `-O3` is forced on `bus.c` in CMake for
the same reason.

### 4.4 Bank-strobe capture

`$FF40` writes are detected in *hardware*: a 74HCT125 used as a gate plus
a diode produces a `STROBE` pulse on GP28 that falls only on a genuine
write to `$FF40–$5F`. The `bank_strobe` PIO program (10 instructions)
waits for that edge, delays a clock-divider-scaled interval (the
scope-tunable `strobe_delay_ns`), samples GP0–21 in one `in pins, 22`,
and pushes:

```
   bit  22      sequence bit — FLIPS on every capture
   bits 21..14  D0..D7   (the bank number)
   bits 13..0   A0..A13  (which register was written)
```

The serve loop reads that word from **SRAM**, kept fresh by a
free-running DMA, and acts only when it *changes*:

```
   PIO RX FIFO ──DREQ-paced DMA──► s_strobe (SRAM) ──► serve loop
   (no read/write increment: s_strobe always holds the latest word)
```

Two subtleties that are easy to destroy:

- **The sequence bit exists because the loop acts on change.** Two
  identical bus writes in a row would otherwise be indistinguishable from
  one, and the second silently ignored — which is exactly what a banked
  cart does when it re-selects the bank it already asked for. Flipping
  bit 22 every capture makes every write a distinct word. The decoders
  mask it away, so it costs nothing, but do not reuse it for anything.
- **The FIFO has exactly one consumer: the DMA.** Never add a CPU-side
  drain alongside it. The DMA is DREQ-credit paced; a CPU pop steals a
  word the DMA holds a credit for, the DMA then reads an *empty* FIFO and
  latches undefined data into `s_strobe` — which the every-lap decode
  holds as the current bank. One steal is a wrong bank until the next
  real write.

### 4.5 The WiFi story

Worth knowing because it explains both boards' architecture. Flat ROMs
were always WiFi-immune; banked ROMs glitched whenever the radio was on,
at any strobe delay. The cause was that banked serving polled the PIO RX
FIFO every lap, and a PIO FIFO read is an **APB** read, which the PROC1
fabric priority does *not* cover. CYW43's DMA bursts stalled that read,
stretched the lap, and the CoCo latched a byte mid-transition.

The fix — move the hot loop off the APB entirely by having DMA drain the
FIFO into SRAM — is the same idea the videocart uses at much larger
scale, and the reason its core-1 loop touches no PIO register per lap.

---

## 5. The videocart bus engine

### 5.1 The problem

This board does not see the address bus. A0–A15, `R/W`, `CTS*` and `SCS*`
arrive through three 74HCT153 4-to-1 muxes, and the Pico must drive the
select lines and sweep all four phases to reconstruct one address:

```
   phase │  M0    M1    M2    M3   │  M4
   ──────┼─────────────────────────┼────────
     0   │  A0    A4    A8    A12  │  R/W
     1   │  A1    A5    A9    A13  │  CTS*
     2   │  A2    A6    A10   A14  │  SCS*
     3   │  A3    A7    A11   A15  │  CTS*   (duplicated)
         │  GP9   GP10  GP11  GP22 │  GP20
   SEL0 = GP26, SEL1 = GP27   (PIO side-set pair, consecutive by design)
```

So the address is not knowable until the sweep finishes, ~490 ns after E
falls. The data must be on the pins by ~1037 ns. That leaves ~550 ns to
mux-decode 16 scattered bits, look up a byte and drive it.

### 5.2 Why there is no CPU in the serve path

Every CPU-based design missed that window. Reassembling a 4×4 bit-matrix
transpose, masking the gates, indexing and driving took 450–500 ns of the
550 ns available, and jitter alone was ~100 ns. It never fit, in any
arrangement. PIO + DMA closes the same path in ~150 ns, deterministically.

The trick that makes it possible: **never un-scramble the address at
all.** The sampler shifts the bits into its ISR in whatever order the
phases deliver them, and the ROM image is permuted once at mount time so
that the scrambled index is already the right one.

### 5.3 The engine

```
         ┌────────────── pio1 ──────────────┐
  E ────►│ mux_sampler                      │
  mux   ►│  drives SEL0/SEL1, 4 phases      │
  pins   │  builds  table_base | idx*2      │
         │  gates on A14 & A15              │
         └──────────────┬───────────────────┘
                        │ push (noblock)
                        ▼  RX FIFO
              ┌──── CH_ADDR (ENDLESS, DREQ-paced) ────┐
              │  copies the pushed word into          │
              │  CH_DATA.al3_read_addr_trig           │
              └──────────────┬────────────────────────┘
                             ▼  (that write also STARTS CH_DATA)
              ┌──── CH_DATA (1 transfer, 16-bit) ─────┐
              │  *(uint16_t *)word  ──► serve TX FIFO │
              └──────────────┬────────────────────────┘
                             ▼
         ┌────────────── pio2 ──────────────┐
         │ bus_serve   drives D0..D7        │──► D0..D7
         │ wr_capture  samples D0..D7       │──┐
         │ sweep_follower  passive sweep    │──┤
         └──────────────────────────────────┘  │
                                               ▼ DMA rings ──► core 1 snoop
```

Note what is *not* in that diagram: a CPU. Core 1 never participates in
serving a byte. It only pairs snooped writes with their addresses.

### 5.4 The sampler and the permuted table

Per phase the sampler snapshots the pins once (`mov osr, pins`), flips
SEL for the *next* phase on the very next instruction — so the '153
settle overlaps the bit extraction — and then mines the snapshot with
`in osr, 3` / `out null, 13` / `in osr, 1`.

The ISR starts at `X = table_base >> 15` and 15 bits shift in on top:

```
   ISR after the sweep, MSB..LSB:

   [ table_base >> 15 ][ A8 A4 A0 │ A12 │ A9 A5 A1 │ A13 │ A10 A6 A2 │ A11 A7 A3 │ 0 ]
                        └──────────── 14 address bits ────────────────┘  └ the ×2

   pushed word  =  table_base | (perm_idx(A0..A13) * 2)
                =  the byte address of a uint16_t table entry
```

The DMA needs no arithmetic at all — the pushed word *is* a pointer.
`perm_idx()` in `sweep.h` produces exactly that bit order, and
`perm_build()` fills each bank's 32 KB table with

```c
table[perm_idx(a)] = (dirs << 8) | rom[a];   /* dirs = 0xFF, or 0x00 for $FFxx */
```

Four things about this that you must not break:

1. **32 KB alignment.** The sampler builds the address by `OR`, not `+`.
   Any set bit below bit 15 of the base corrupts every lookup. A
   `_Static_assert` cannot check placement, so `bus_init()` checks it at
   runtime and panics.
2. **The gates come last.** A14 and A15 are tested *after* the final
   sample, so a failing address cannot change SEL timing. The follower's
   fixed delays assume the sweep is cycle-invariant on **every** cycle,
   including writes and non-window reads.
3. **`R/W` is deliberately not gated in the sampler.** Branching on it at
   phase 0 would make SEL timing data-dependent and break the follower on
   exactly the write cycles the snoop needs. The serve SM tests the live
   `R/W` pin instead; a write costs one wasted lookup and drives nothing.
4. **`$FF00–$FFFF` is excluded in the data, not in logic.** It passes the
   A14/A15 gate, but the 6809 fetches its vectors there and the SAM
   answers them, so those entries carry `dirs = 0x00` and the serve SM
   leaves the bus tristated. That is why entries are 16 bits.

`push noblock`, always. A blocking push would stall the SM whenever the
serve DMA is idle or paused — freezing SEL, which silently kills the
*snoop*, because the follower depends on the sweep running every cycle.
Liveness of capture outranks one serve word. A dropped word still sets
`FDEBUG_RXSTALL`, which `bus_debug()` turns into the panel's `svstall`
counter and which the acceptance criteria require to be zero.

### 5.5 Banks

`bus_serve()` builds one 32 KB table per 16 KB bank in `s_main`, and a
bank switch is just repointing the sampler's `X` at another table.

That repoint may **only** happen while the SM is parked. The sampler
parks at `wait 0 gpio 8` for all of E-high; `sampler_retable_parked()`
waits for a *fresh* E rise and then execs two instructions (~130 ns
against ≥550 ns of park). Exec'ing into a running sweep clobbers OSR —
the live pin snapshot — and corrupts that cycle's address and its A14/A15
gates.

The E wait is **bounded** (`E_PARK_SPINS`). `bus_serve()` is called from
core 0 at mount, and E is dead whenever the CoCo is unpowered — which is
the normal state while flashing on the bench. An unbounded spin there
hangs core 0 before the OLED or web server ever come up, and the board
looks bricked.

### 5.6 The serve SM's four guarantees

`bus_serve` is nine instructions and each guarantee is structural, not
timing-based:

| # | Guarantee | Mechanism |
|---|---|---|
| 1 | Never drives during E-low | `wait 1 gpio 8` precedes any `pindirs` write, so the SAM's VDG fetch can never be contended |
| 2 | Never drives a write cycle | `JMP_PIN` is GP20 = M4, which carries live `R/W` while SEL rests at 0 — i.e. throughout E-high |
| 3 | Never drives outside the window | A14/A15 gated in the sampler; `$FFxx` gated by `dirs = 0x00` in the table |
| 4 | Never serves a stale byte | the word arrives by DMA inside the same E-low its address was sampled in; there is no CPU to be late |

`out pins, 8` writes the data register *before* `out pindirs, 8` enables
the drivers, so enabling never emits a stale byte first.

### 5.7 The snoop

Two more state machines watch the same bus passively:

- **`sweep_follower`** samples the same four phases as the sampler. Its
  four `in pins, 14` must land on *exactly* the same cycles as the
  sampler's four `mov osr, pins` (R+14, R+25, R+36, R+47).
  `test_sampler_timing` decodes the assembled programs and asserts this;
  if they ever drift, the snoop mirrors bytes against the wrong address
  and nothing else notices.
- **`wr_capture`** waits for E to rise, delays the configured interval,
  and samples D0–D7. It pushes `(1 << 8) | data` — the validity bit
  matters, because a DMA ring slot must distinguish "written" from
  "empty" and a captured `$00` is legal.

Both streams are drained by chained DMA pairs into rings, and here is the
idea that removed the last deadline in the design:

> **Pair by index, not by deadline.** The capture SM pushes exactly one
> word per bus cycle; the follower pushes exactly four. Both DMAs
> preserve order. So capture *k* belongs with follower group *k*, no
> matter how far behind core 1 is running. A delayed capture is harmless.
> Only a dropped one is a defect.

```
   capture ring   [ k-2 ][ k-1 ][  k  ][ ... ]      1 word  per bus cycle
                                  │
                                  │  index pairing
                                  ▼
   follower ring  [ .. ][ 4(k-1).. ][ 4k+0 4k+1 4k+2 4k+3 ][ .. ]
                                      tag0  tag1  tag2  tag3
                                      └──── must read 0,1,2,3 ────┘
```

What remains is not a deadline but a **throughput** requirement: the
average lap must beat 1117 ns. The lap-anatomy instrument publishes the
margin every 4096 laps.

**The anchor is the load-bearing part.** `fol_rd` walks the follower ring
`+= 4` per capture, and it is anchored to the DMA's *live* write pointer:

- Anchor **only at a fresh blocking arrival with zero backlog**. At that
  instant the follower has finished this cycle's group and has not
  started the next, so the write pointer sits exactly four words past the
  group that pairs with the capture in hand. An arrival that was already
  waiting proves nothing about phase — consuming it can happen anywhere
  in the bus cycle, including mid-sweep.
- Read the pointer **at spin exit**, not after the bookkeeping. The
  bookkeeping costs 200–240 ns, which lands the read straddling the next
  group's first DMA write — and because this loop is phase-locked to the
  bus, a read that lands late once lands late on *every* retry.
- Do **not** compute the group slot arithmetically from a cycle count.
  That silently assumes group boundaries sit at word-phase 0 forever; one
  word-level slip shifts the ring's phase and the arithmetic can never
  land on a group again.
- `snoop_pair_pos()` checks **both** halves' busy bits and retries the
  brief handover instant. Falling back to channel B's `write_addr` when A
  is idle reads the *configured ring base* and anchors at garbage.

Two guards, because they catch different faults: the **tag guard** (the
four tags must read 0,1,2,3) catches word-level slips, and a **periodic
phase-pinned distance check** catches whole-group slips, which keep the
tags correct and mirror one-cycle-stale addresses forever. Pressing the
CoCo's physical RESET button causes exactly the latter, and unlike a cart
change it bumps no epoch — which is also why an epoch change drops the
anchor unconditionally.

### 5.8 The core-1 loop

```c
for (;;) {
    /* close the previous lap's BODY measurement; publish every 4096 laps */
    /* pace: block until capture k lands (this is the WAIT half)          */
    /* read the follower write pointer AT SPIN EXIT if anchoring/checking */
    /* (re-)anchor, or consume-without-mirroring toward a re-anchor       */
    /* read follower group k; tag guard; reassemble addr via the LUTs     */
    /* epoch change? re-read serve/scheme/bank_mask, drop the anchor      */
    /* read cycle?  -> reset-vector detect, window_reads, 1-in-64 echo    */
    /* write cycle? -> SAM TY, bank/stub decode, MIRROR, register rings   */
}
```

Points of interest:

- **The loop measures itself.** Instruction counting was wrong on every
  core-1 loop this project built, six times over. SysTick splits each lap
  into WAIT (blocked on the capture) and BODY (the part we pay for).
  `body >> wait` means compute- or contention-bound; `wait` dominant with
  a lap still over 1117 ns means captures are not arriving once per cycle
  and no loop tuning will help. Nanoseconds-per-tick is derived from
  `clock_get_hz()` **outside** the loop — the loop is RAM-resident and
  must not call into flash, and hardcoding 8 ns made every panel figure
  read double after the clock change.
- **The mirror lives on core 1.** One `strb` costs tens of nanoseconds on
  a quarter of laps. Moved to core 0 it inherits every core-0 stall — an
  OLED page flush blocks ~90 ms over 100 kHz I²C — and real screen bytes
  are lost to a display update.
- **The serve echo** is the only direct measurement of serve correctness:
  the capture SM samples D0–D7 *inside* the window the serve SM drives,
  so on a served read the captured byte **is** the byte the CoCo got.
  It is compared against the table entry the DMA looked up, 1 in 64
  (reading the same table the serve DMA is reading is not free). Read it
  alongside `tagslip` — a mis-paired capture reports here as a wrong
  served byte that was never wrong on the bus.
- **SAM map type is observed only.** Muting the serve SM on TY was tried
  and broke both serving and capture: TY is sticky (one `$FFDF` with no
  matching `$FFDE`, entirely plausible during BASIC's RAM sizing, mutes
  serving permanently), and PIO enable/disable/restart sequences are long
  APB operations that stall the lap and back the snoop ring up. If you
  want to revisit it, it has to be cheap and self-recovering.
- **A reset is `$FFFE` *then* `$FFFF` on consecutive cycles.** Counting
  bare `$FFFE` reads reported 911,418 "resets" in a minute — the 6809
  drives high addresses during its internal dead cycles.

### 5.9 Handing off to core 0

Core 1 keeps what must land in-cycle and passes the rest over rings:

| Ring | Producer | Consumer | Carries |
|---|---|---|---|
| `s_pair` (2048) | core 1 | `bus_pump()` on core 0 | `$FF22` + SAM writes, packed `[23:16] data, [15:0] addr` |
| `s_aud` (1024) | core 1 | the HSTX island IRQ | audio register writes, `lap<<12 \| reg<<8 \| data` |

They are separate on purpose. Audio is a far higher event rate than mode
changes — a DAC playback loop runs 8–22 k writes/s against a handful per
mode change — and it must never be able to evict a `$FF22`/SAM write,
because those latch display state and a lost one is a permanently wrong
screen.

`video_note_reg()` applies register writes **immediately**; there is no
queue between `bus_pump()` and the renderer, and there must not be. Both
ends are on core 0, so a queue buys nothing, and any queue that can drop
will eventually drop a SAM page flip. Dragon Fire writes `$FF22` ~6300
times a second from a per-scanline raster loop; that overflowed a
256-slot ring and took page flips with it, parking the renderer a third
of a screen away from the page the game was drawing.

---

## 6. Video: mirror to HDMI

### 6.1 Scanout with no CPU and no interrupts

The multicart's flash rule collides with any IRQ-fed scanout: core 0
disables interrupts for a whole flash sector erase (~45 ms) on every ROM
upload. So the frame is a **static descriptor list in SRAM walked by
DMA**, and a flash erase is invisible to the display.

```
   ┌─────────────┐  4-word control blocks   ┌──────────────┐
   │  CH_CTRL    │ ───────────────────────► │   CH_DATA    │
   │ reads s_desc│  {read, write, count,    │ pushes words │──► HSTX FIFO
   │ 16-byte     │   ctrl}  into CH_DATA's  │ paced by     │
   │ WRITE ring  │  alias-0 registers       │ DREQ_HSTX    │
   └─────────────┘  (writing ctrl TRIGGERS) └──────┬───────┘
          ▲                                        │ chains back
          │                                        │
          └────────────────────────────────────────┘

   the LAST block instead chains CH_DATA to:
   ┌─────────────┐
   │  CH_RESET   │  writes s_desc_base into CH_CTRL.al3_read_addr_trig
   │  1 word     │  → rewinds AND restarts the frame, forever
   └─────────────┘
```

`CH_RESET` exists only because a DMA channel has **one** ring.
`CH_CTRL` needs its ring on the write side to keep hitting the same four
registers, so the read-side rewind has to come from somewhere else.

### 6.2 The framebuffer

```
   s_fb[192][171 words]        one row = 11 command words + 160 pixel words
   ┌──────────────────────────────────────────────────────────┐
   │ RAW_REPEAT 16  SYNC_V1_H1                                │  front porch
   │ RAW_REPEAT 96  SYNC_V1_H0                                │  hsync
   │ RAW_REPEAT 48  SYNC_V1_H1   (or 38 + preamble + guard    │  back porch
   │ NOP NOP NOP NOP              in HDMI mode)               │
   │ TMDS 640                                                 │
   │ ─────────── 160 words of RGB332 pixels ───────────────── │
   └──────────────────────────────────────────────────────────┘
```

Commands live *inline in the framebuffer* so one display line is a single
descriptor. The 256×192 VDG image is doubled horizontally into 512 pixels
with 64-pixel borders baked in (the HSTX expander unpacks *several*
pixels per word — it has no "repeat each sample" mode, so hardware pixel
doubling is not available). Vertical doubling is free: two descriptors
point at the same row, which is why only 192 rows are stored.

Frame: 45 blanking lines, 48 black border lines, 384 image lines
(192 × 2), 48 black border lines = 525 lines of 800 clocks.

`test_hstx_frame` walks the real descriptor table into a model of the
HSTX front end and slices it into scanlines, asserting every line is
exactly 800 clocks with hsync at 16 for 96, two vsync lines at 10–11, 480
active lines of exactly 640 pixels, every row scanned exactly twice, and
the chain rewinding. It is the cheapest way to check a change to the
scanout.

### 6.3 The VDG renderer

`vdg.c` is a software MC6847 reading the 64 K mirror. Mode state comes
from `$FF22` (A/G, GM2–0, CSS) and the SAM V/F registers; rendering runs
on core 0 at ~60 Hz, and tearing against the free-running scanout is
accepted.

Things that are easy to get wrong and are pinned by `test_vdg`:

- **Inverse video.** Bit 6 of a character sets INV. Anchor this to the
  real machine, not a datasheet reading: BASIC clears the screen to `$60`
  (a space with bit 6 set) and a blank CoCo screen is bright green, so a
  `$60` cell must render *fully lit*. A test that only checks the two
  forms are complements passes just as happily with the whole screen
  rendered as a photographic negative.
- **Borders.** Black **only** in the alphanumeric modes. Every graphics
  mode takes the colour-set colour — green (CSS=0) or buff (CSS=1).
  Zaxxon's white surround is border, not content.
- **SAM half-height scans.** The SAM's video counter, not the VDG mode,
  decides how many *distinct* rows are fetched; when it scans fewer, the
  VDG repeats each row. Monster Maze runs a full-height RG6 display out
  of 3072 bytes that way. But `s_sam_v` comes from captured bus writes,
  so the override is **guarded hard**: the SAM may only win when it and
  the VDG agree on bytes per row *and* the SAM's row count is the mode's
  own or exactly half. A stale V otherwise rewrites the geometry for
  every cart, and because the panel renders the mirror, that looks
  exactly like "capture is broken".
- **PMODE 4 artifact colour.** The 6847 makes no colour at all here; it
  emits dots at two per NTSC subcarrier cycle and the *monitor* invents
  the colour. The model is chroma = a 4-tap boxcar of `s[n]·(−1)ⁿ`, luma
  = the dot itself, mixed with the chroma magnitude de-emphasised as `a²`
  so an isolated dot renders nearly luma-pure. Since the output depends
  only on the 4-dot window and the phase, a 32-entry LUT **is** the exact
  continuous model. Colouring pixel *pairs* instead quantises luma to 128
  cells and turns glyph stems into fringed blocks. The phase is arbitrary
  on real hardware — which is why so many CoCo games tell you to reset
  until the colours look right — so it is a live runtime setting.

---

## 7. Audio: PIA writes to HDMI data islands

### 7.1 Where CoCo sound comes from

The CoCo 2 has no sound chip. Audio is CPU-generated through PIA
registers, and every one of those writes crosses the cartridge port:

| Register | Role |
|---|---|
| `$FF20` bits 7:2 | the 6-bit DAC ladder — **only when `$FF21` bit 2 is set** |
| `$FF22` bit 1 | the single-bit sound output |
| `$FF23` bits 5:3 = 111 | the sound gate (PIA1 CB2) |
| `$FF01`/`$FF03` bit 3 | the analogue mux: 00 = DAC, 01 = cassette, 10 = cart SND, 11 = none |

Four facts that the synth must honour, each of which was audible when it
did not:

- **A PIA data register is only the data register when the control
  register says so.** With `$FF21` bit 2 clear, a write to `$FF20` goes
  to the *data direction* register and never reaches the ladder.
  Latching it injects a spurious level. Clearing the bit does not zero
  the ladder either — reading the DAC as 0 there produces a full-scale
  *negative* excursion, because 0 is the bottom of the range and 32 is
  the midpoint. The same rule applies to `$FF22`/`$FF23` bit 2, and there
  it also affects **video**: the diagnostic cart's sound routine is
  `EORA 2,U`, which can only ever flip PB1, yet thousands of `$05`/`$07`
  writes appear on the bus — direction writes, which thrashed the
  renderer into alphanumeric mode until they were gated.
- **The PIAs are mirrored every 4 bytes** (`$FF00–$FF1F`, `$FF20–$FF3F`),
  because only the low two address lines reach them. Matching the
  canonical addresses only meant programs using a mirror — and plenty
  do — had their writes silently ignored.
- **The single-bit output is not behind the sound gate.** The enable
  switch sits in the DAC/mux path; PB1 sums into the audio node past it.
  Gating it made the diagnostic cart's 1.3 s single-bit sweep silent.
- **A mux away from the DAC must be silent, not held.** BASIC's `JOYIN`
  sweeps the mux while doing successive approximation on `$FF20`; holding
  the last value turns every joystick read into a buzz instead of the
  soft click a real CoCo makes.

### 7.2 The timebase

Core 1's **lap counter** is the audio clock. It advances once per E
cycle, so it is already locked to the CoCo's 894886.25 Hz. One sample at
48 kHz is 18.6434635 laps, kept as 16.16 fixed point
(`LAPS_PER_SAMPLE_Q16`). Events carry a 20-bit lap stamp that the
consumer unwraps against its own running count.

The CoCo's crystal and the RP2350's are independent, so audio time walks.
A servo snaps it when the backlog exceeds ±4 ms — roughly once a minute
at ~50 ppm, moving a held level by ≤4 ms, which is a shift rather than a
click. **Do not servo the ACR instead**: a sink's PLL is far less
tolerant of a moving CTS than the ear is of a 4 ms step.

### 7.3 The synth

```
   events ──► apply_event ──► current_pcm ──► box filter over the window
                                                   │
                             ┌─────────────────────┘
                             ▼
   DC blocker (~7.5 Hz)  ──► gate × mux weighting ──► + single-bit
   ON THE SOURCE               (by duration, not          (own DC blocker)
   not the gated output         snapped to a boundary)         │
                                                               ▼
                                            2-pole low-pass (6k / 4k / 2.5k)
                                                               │
                                                        clamp + meter
```

- **Box filter, not point sampling.** Each sample is the time-average of
  the ladder across its window. Events are stamped to 1.117 µs; point
  sampling snaps every edge to the nearest 20.8 µs boundary, which on a
  square wave is period jitter — about 1 % at 1 kHz — and is heard as
  roughness on exactly the material the CoCo produces. (This is why the
  synthetic test tone always sounded cleaner than the CoCo: it is
  generated in the sample domain and has no edges between samples.)
- **Block DC on the source, before the gate.** Zaxxon mutes the gate,
  reads the joystick, leaves the ladder at 0, unmutes, then sits still
  for 66 ms. A real CoCo is silent throughout — a motionless ladder is DC
  and DC never reaches the speaker. Blocking DC on the *gated* output
  turns that into a 15 Hz square wave at half full scale, which is
  plainly audible as ticking and which no packet, timing or rate test can
  see.
- **The corner is a compromise, in both directions.** At ~15 Hz every
  0.69 ms plateau of a PCM sample stream droops 6 %; below ~5 Hz the
  residual left after Zaxxon's 66 ms park is big enough for the gate to
  chop and the tick returns. ~7.5 Hz is the middle.
- **Two cascaded poles, not one.** A single pole rolls off at
  6 dB/octave, so no corner separates the buzz from the notes — by the
  time the harmonics are down the fundamentals are too. Test the *order*
  on the step response (the fraction of the step landing in the first
  sample), not on square-wave levels, which plateau at the rails either
  way and cannot tell the orders apart.
- **Emit only what is DUE.** A frame has `ISLAND_LINES × AUD_PACKETS` =
  320 packet slots carrying up to four samples each, against the 806.4
  samples a 59.524 Hz frame owes at 48 kHz. That headroom is deliberate —
  it is what absorbs an island giving a slot up to an InfoFrame — so
  filling every slot regardless would emit far more samples than the sink
  asked for. The ACR tells it to regenerate exactly 48.000 kHz, so its
  FIFO overruns, while on our side audio time outruns the events until
  the servo snaps. Both are audible as periodic static, and neither shows
  up in a packet-shape test: the packets are perfectly well formed, there
  are just too many samples in them.

The output is a **model** of the CoCo's analogue path, not a recording of
it. The cartridge port carries no analogue audio (the `SND` pin is an
input *into* the CoCo), so everything downstream of the DAC — the
coupling capacitor, the amplifier's bandwidth, the resistor that makes
PB1 quieter than a full ladder swing — has to be simulated. That is the
ceiling on fidelity, and it is why the filter and single-bit level are
runtime settings judged by ear.

### 7.4 Data islands

HDMI carries non-video data in "islands" inside blanking intervals,
encoded in TERC4 rather than TMDS. The packet layer here is ported from
the hdl-util/hdmi RTL — the thing known to make this display report
"PCM 48 kHz" — rather than written from the spec. Where the two could
differ, the RTL wins.

An island is a fixed 271 words on an active line (268 on a blanking
line):

```
   ┌─ 4 clocks control ─┬─ 8 clocks island preamble ─┬─ 2 guard ─┐
   │                                                             │
   ├─ packet 0 ─┬─ packet 1 ─┬─ packet 2 ─┬─ packet 3 ─┐        │
   │  32 clocks each: ch0 = {1, header bit, V, H}                │
   │                  ch1 = bit 2k of the four subpackets        │
   │                  ch2 = bit 2k+1                             │
   └─ 2 guard ─┬─ 6 control ─┬─ 8 video preamble ─┬─ 2 guard ─┐ │
                                                   TMDS 640 ───┘
```

Each packet is one of: an audio sample packet (0–4 samples, the header
says how many), an ACR packet (N=6144, CTS=25000 — exact at a 25.000 MHz
pixel clock, so no fractional-CTS error to chase), a Null, or one of the
three InfoFrames. Header and subpackets carry BCH parity, computed from a
256-byte table proven against the bit-serial generator by `test_audio`.

Two placement rules that cost real debugging to find, both of which are
invisible to any packet-shape test:

- **Islands are spread over all 525 lines, blanking included.** Confining
  them to the 480 active lines leaves a 1.44 ms hole in every frame: the
  sink has to coast ~69 samples on its own buffer, once per frame, and it
  buzzes at exactly the 59.52 Hz frame rate. A conforming source uses
  every blanking interval, and the vertical one is the largest. The
  islands are spread by Bresenham and deferred off the two vsync lines
  (island content is built without knowing which line it lands on, so V
  must be inactive for all of them; the 33-line back porch always has
  room).
- **One InfoFrame per island, on three consecutive islands.** Stacking
  AVI + Audio + SPD into island 0 steals three of its four slots, so it
  carries at most 4 samples where ~13 are due — a ~9-sample hole once per
  frame that takes seven islands to make back, heard as crackle.

Only the ~80 island descriptors clear `IRQ_QUIET`, so the refill handler
runs at ~3.3 kHz. It fills the slot `AUD_SLOTS` islands ahead, and it
**anchors to the control channel's read address** rather than trusting a
free-running count: each position carries its own shape, and being off by
one writes a 268-word blanking island into a descriptor that reads 271,
after which the frame desynchronises. `ISLAND_LINES` must stay a multiple
of `AUD_SLOTS` for the same reason.

The handler runs at **low** IRQ priority (0xC0). It was 0 — the highest —
on the reasoning that nothing else on core 0 uses interrupts. The CYW43
driver does, and a handler that blocks it for tens of microseconds at
3.3 kHz takes the web server down while ICMP still answers from a path
that does not need it. The refill has ~1.1 ms of lead time, so being
preempted costs nothing; `alate` on the panel would say if it ever did.

---

## 8. Shared subsystems

Everything in `common/` is board-agnostic and sits above `common/bus.h`,
which is **the seam**. Nothing above that line knows whether an address
arrives on 14 direct GPIOs or through a mux sweep.

### 8.1 Flash

```
   0x000000  ┌────────────────────────┐
             │ firmware slot A 768 KB │  running image
   0x0C0000  ├────────────────────────┤
             │ firmware slot B 768 KB │  OTA staging (+ header in last sector)
   0x180000  ├────────────────────────┤
             │ LittleFS  2556 KB      │  ROM images + catalog.txt
   0x3FF000  ├────────────────────────┤
             │ cfg  4 KB              │  magic "COCO", version, payload, CRC32
   0x400000  └────────────────────────┘
```

The layout was fixed on day one so nothing ever moves. The cart-image
build (`mklfs`) targets only the LittleFS partition, so flashing a new
set of ROMs never disturbs the firmware or your WiFi credentials.

### 8.2 cfg

One 256-byte page in the last sector: magic, version, payload, CRC32 at
page−4. Single-buffered — a power loss mid-save reverts to defaults on
the next boot.

**The migration rule is the important part.** A page saved by an *older*
firmware must keep every value it stored, and every field that version
predates must come back at its **default**, never at whatever bytes
happened to follow the old struct. Getting this wrong is silent and
expensive: two fields were once added without bumping `CFG_VERSION`, the
stale page was overlaid wholesale, `hdmi_audio` read back as 0, and four
consecutive builds shipped with the entire audio path compiled in and
never once executed.

So: **bump `CFG_VERSION` whenever you add a field**, add a
`if (h->version < N)` restore block, and extend `test_cfg`.

### 8.3 romfs and the catalog

LittleFS on the data partition, one file per ROM stored verbatim, plus a
text catalog:

```
   filename|title|autostart|scheme|insert
```

Missing entries derive sensible defaults (title from the filename,
autostart on, scheme by size). Filenames are filtered rather than
allowlisted — real cartridge names contain `&`, `+`, `(`, `)` and spaces
— so only what actually breaks something is rejected: path traversal,
control characters, `"` (the JSON is emitted unescaped) and `|` (the
catalog delimiter).

Flash discipline: erase and program run from RAM with interrupts off for
exactly **one sector or page each**, so lwIP sees a sub-100 ms hiccup and
core 1 keeps serving throughout.

### 8.4 The selection sequencer

`select.c` is the only place that orchestrates the CoCo, and it is a
non-blocking state machine pumped from the core-0 loop — a blocking
`sleep_ms()` anywhere in here would stall the OLED, lwIP and any
in-flight upload.

```
   select_rom()
        │  assert !RESET; build the stub in its own buffer; serve it;
        │  CART gate ON
        ▼
   SEL_RESET_HOLD ──── load the real image into staging (safe: the STUB
        │              is what's being served) and bus_serve_prepare()
        │              while the CoCo is still held in reset
        │  after MC_RESET_PULSE_MS: bus_stub_arm(); release !RESET
        ▼
   SEL_WAIT_MAGIC ──── the stub runs, clears $71, writes $FF5F
        │                                                │
        │ magic seen                       1.5 s timeout │
        ▼                                                ▼
   swap_to_real()                              SEL_RESET2 (second pulse)
        │                                                │
        ├─ insert=0 ─► serve + gate ─► SEL_DONE           │
        │                                                 │
        └─ insert=1 ─► bus_idle(), gate OFF ◄─────────────┘
                          │
                          ▼
                   SEL_WAIT_BASIC ── 2.2 s for BASIC to reach READY
                          │          then serve + raise CART* (FIRQ)
                          ▼
                      SEL_DONE
```

Invariants this flow exists to maintain, all of which have been broken at
least once:

- **The stub buffer is only written while `!RESET` is asserted**, and the
  main buffer is only written while the stub (or nothing) is being
  served. The fixed-role two-buffer layout makes the hazard impossible by
  construction; a ping-pong only avoided it by ordering luck.
- **Staging is bounded by `bus_staging_bytes()`, not by the buffer
  size.** On the videocart the image buffer is *also* the serve-table
  space, and the slot above the images holds the stub's table — which the
  CoCo is fetching instructions from for the whole of `SEL_RESET_HOLD`.
  Clearing the whole buffer left the stub reading `$FF`, so it never ran,
  the magic never arrived, and every selection limped in through the
  timeout without its cold boot. (`strobes: 0` on the panel was the tell.)
- **Idling does not void a prepared table build.** The insertion path
  idles the slot on purpose and serves the *same* image 2.2 s later; that
  serve has to stay a pointer flip. Rebuilding there puts milliseconds of
  32 KB-strided SRAM traffic microseconds before the FIRQ that starts the
  game, starving the snoop across exactly the `$FF22`/SAM writes a cart
  makes once and never repeats.
- **A failed load idles the slot and drops the gate.** Otherwise the CoCo
  boot-loops the stub forever.
- **Selection and OTA mutually exclude.** An OTA's flash erases can stall
  the pump past the stub's pre-jump delay.

### 8.5 The 6809 stub

Forty bytes, source in `common/stub6809.asm`, hand-assembled bytes in
`stub6809.h` so the build needs no assembler.

```
   $C000  ORCC #$50           mask interrupts
          LDX  #TAIL          copy the tail to $0600 — the ROM is about
          LDY  #$0600         to change underneath us
          LDB  #19
          ...copy loop...
          JMP  $0600
   ─────────────── everything below runs from RAM ───────────────
   $0600  CLR  $0071          kill the warm-start flag -> COLD boot
          LDA  #$A5
          STA  $FF5F          the "swap me now" signal the Pico watches
          LDX  #$4000         ~145 ms
   DLY:   LEAX -1,X           (generous: the Pico's pump can stall ~45 ms
          BNE  DLY             behind a flash sector erase)
          JMP  [$FFFE]        reset vector -> guaranteed cold boot
```

The firmware detects the `$FF5F` write by **address only**, never by the
data byte — address lines are valid at any sample delay, so the cold-boot
handshake works before the strobe delay has ever been scope-tuned. The
multicart matches `$3F5F` (its A0–A13 view); the videocart matches the
full `$FF5F`.

`test_stub6809` runs the stub on a small 6809 interpreter whose memory
model clobbers the entire cartridge window the instant `$FF5F` is
written, so if any instruction after that point still executes from ROM,
the test fails loudly.

### 8.6 Web, OLED, OTA

- **Web.** A minimal HTTP/1.1 server on raw lwIP. The threading rule: the
  lwIP callbacks (background context) do nothing but queue pbufs and set
  flags; all parsing, all flash access and all `tcp_write` happen in
  `web_pump()` on the core-0 loop. That keeps multi-millisecond flash
  erases out of interrupt context, and TCP backpressure falls out
  naturally because `tcp_recved()` is only called once bytes have
  actually been consumed. One shared response buffer, owned by one
  connection from build to close.
- **The UI page** is a single C string in `webui.h`, no external assets —
  the cart may be on a network with no internet. Rows are built with
  `createElement` and closures, so no filename is ever interpolated into
  markup.
- **OLED.** 100 kHz I²C (the PCF8574's ceiling), so a full 8-page repaint
  is ~100 ms. The driver tracks dirty pages and pushes **one page per
  pump call**; the menu moves the cursor by repainting two rows when the
  window does not scroll.
- **OTA.** The uploaded `.bin` goes to slot B with a header (magic,
  length, CRC32). It is CRC-checked, read back out of flash and
  re-CRC'd (a flash that did not take otherwise looks like a clean
  upload), and sanity-checked for this board's hostname string — so a
  multicart image cannot be applied to a videocart. On the next boot a
  RAM-resident copier moves B→A and reboots. The honest risk: that copy
  takes 10–15 s with interrupts off, and power loss inside that window
  needs BOOTSEL recovery.

---

## 9. Modifying it

### 9.1 Where things live

```
   firmware/
     common/         board-agnostic: everything above common/bus.h
     multicart/      config.h, bus.c, bus_pio.pio, main.c
     videocart/      the same four + hstx.c, vdg.c, audio.c and headers
     test/           host-side simulation suite
     tools/          mklfs, mkdiag, preflight
```

A board owns exactly four files. **Fix bugs in `common/`, not in a copy.**

### 9.2 Recipes

**Add a per-ROM flag.** `rom_entry_t` in `romfs.h`; parse and emit it in
`apply_catalog()`/`write_catalog()`; add it to `romfs_set_meta()`'s
signature (all callers); expose it in `api_meta()` and `api_roms()`; add
a row to the per-ROM OLED settings screen; teach `mklfs.c` the new
catalog field. `test_romfs` and `test_ui` will tell you what you missed.

**Add a device setting.** Add the field to `cfg_t`, **bump
`CFG_VERSION`**, add its default in `cfg_load_defaults()`, add a
`if (h->version < N)` restore block in `cfg_init()`, extend `test_cfg`.
Then add it to `api_settings_get`/`_post`, to `webui.h`, and to the
`dev_item_t` enum plus `devset_text()` and `do_short_press()` in `ui.c`.
Decide **live vs staged**: live settings must be applied to the hardware
as they are turned *and* un-applied by "Back (no save)"; staged ones get
a `*` marker. `test_ui_vc` checks both, and that every label fits 21
columns.

**Add a web API endpoint.** Add the handler, route it in `route()`, and
remember the response buffer is shared — build the whole body, then call
`resp_finish()`. Parameters come from the **query string**; the API never
reads the request body except for uploads.

**Change the VDG renderer.** Work in `vdg.c`, run `test_vdg`, and *look
at the PPMs it writes*. If you touch the artifact model, remember the
tests assert model *properties* (a solid run carries no chroma, a
sustained alternating pattern saturates, an isolated dot stays
luma-pure), not a specific palette.

**Change the audio chain.** `audio.c`. `test_audio` pins the packet layer
byte-for-byte by decoding packets back out of the island words, so
transport changes will fail loudly. For the synth, the tests that matter
are the ones about *shape*: box-filtered edges, DC blocked before the
gate, the ungated single-bit output, the DAC latch honouring CRA bit 2,
and the filter order.

**Port to a third board.** Write `config.h`, `bus.c`, `bus_pio.pio` and
`main.c`; implement every function in `common/bus.h`, returning 0/NULL
for the diagnostics your board cannot produce; copy a `CMakeLists.txt`
and keep the board directory first on the include path so `#include
"config.h"` resolves to yours.

### 9.3 Invariants — the checklist

Before you flash anything:

- [ ] `make test` is green.
- [ ] `make audit` passes (after any change to `bus.c` or `ota.c`).
- [ ] `make preflight` passes (PIO budget, table alignment, call-free
      core-1 loop).
- [ ] Core 1 still touches no flash and no APB register per lap.
- [ ] `bus_ctrl_hw->priority` is still `|=`, never `=`.
- [ ] The videocart's serve table is still 32 KB aligned, and staging is
      still bounded by `bus_staging_bytes()`.
- [ ] Any new PIO instruction still fits the 32-word block budget.
- [ ] Any new DMA descriptor that must not interrupt has `IRQ_QUIET`.
- [ ] `CFG_VERSION` bumped if `cfg_t` changed.
- [ ] Only one behavioural change per round. Two at once and the
      attribution goes wrong, which is worse than the bug.

### 9.4 Debugging on real hardware

There is no debugger in the loop and no serial console worth relying on —
**every instrument surfaces on the web panel or the OLED**. The panel's
counters are designed to bisect, so read them in this order:

| Line | Says |
|---|---|
| `expander` | the PCF8574 answers. If not, nothing downstream can work: no reset, no gate. |
| `lap` | ~1117 ns = core 1 is keeping up with the bus. Higher = cycles being missed. |
| `lap split` | where the lap goes. `body` near 1117 = compute/contention bound. |
| `serving` / `win reads` | an image is mounted, and the CoCo is actually reading it. |
| `dropped` | `svstall` / `pairdrop` / `tagslip` must all be 0. Each names a different pipeline. |
| `snoop lag`, `lag now` | how deep into the 256-cycle ring the consumer ran. Near depth = overflow. |
| `svc fifo` | 2+ means the serve SM latched one behind and is serving stale bytes forever. |
| `tbl check` | the serve tables verified against their source on the device. |
| `cold boot` | any timeout means selections are **not** getting a guaranteed cold boot. |
| `coco reset` | `$FFFE`/`$FFFF` pairs. Separates "the machine reset" from "the program returned to its menu". |
| `svc/cap echo` | serve correctness — but cross-check `tagslip` first; a mis-paired capture reports here as a wrong byte. |
| `video` | the renderer's **decoded** mode and display base. Compare against what the cart's ROM programs. |
| `audio` | measured sample rate (must be ~48000), island count, and the drop counters. |

And the dumps: `/api/mirror` shows the snooped RAM, `/api/screen` the
exact bytes the last frame rendered, `/api/audiolog` the register writes
with the state each one produced, `/api/pcm` the samples actually sent.

Two habits that shortened everything:

1. **Disassemble the cart before theorising.** More than one "crash" was
   a display-base change, and one game turned out to be a tokenised BASIC
   program in a cartridge.
2. **Distrust the instrument first.** A meter that counted calls instead
   of time blamed the joystick for chopping audio with zero select-line
   edges on the bus; a log that replayed state from power-on defaults
   confidently reported the sound gate was off when it was on. An
   instrument that states the opposite of the truth is worse than none.

---

## 10. Appendix: quick reference

### Pin maps

**Multicart**

| GPIO | Function |
|---|---|
| GP0–13 | A0–A13 |
| GP14–21 | D0–D7 |
| GP22 | `!CTS` |
| GP26/27 | I²C1 SDA/SCL (OLED + PCF8574) |
| GP28 | `STROBE` (SCS write, via '125 + diode) |

**Videocart**

| GPIO | Function |
|---|---|
| GP0–7 | D0–D7 |
| GP8 | `E` |
| GP9/10/11/22/20 | mux outputs M0/M1/M2/M3/M4 |
| GP12–19 | HSTX (TXC−/+, TX0−/+, TX1−/+, TX2−/+) |
| GP21/28 | I²C0 SCL/SDA |
| GP26/27 | mux selects SEL0/SEL1 |

**PCF8574 (both boards)**

| Bit | Function |
|---|---|
| P0/P1/P2 | encoder A / B / switch (inputs) |
| P5 | low = assert `!RESET` |
| P6 | low = CART autostart gate on |

### CoCo registers the firmware decodes

| Address | Meaning |
|---|---|
| `$FF01`/`$FF03` bit 3 | analogue mux select (SEL1/SEL2) |
| `$FF20` | PIA1 PA — 6-bit DAC in bits 7:2 (data reg only when `$FF21` bit 2 set) |
| `$FF21` bit 2 | selects `$FF20` = data register vs DDR |
| `$FF22` | PIA1 PB — VDG mode bits 7:3, single-bit sound bit 1 (data reg only when `$FF23` bit 2 set) |
| `$FF23` bit 2 | selects `$FF22` = data register vs DDR |
| `$FF23` bits 5:3 | `111` = sound gate on (CB2) |
| `$FF40–$FF5F` | cartridge/disk I/O: bank register, and the stub's `$FF5F` magic |
| `$FFC0–$FFD3` | SAM V0–V2 and F0–F6, as clear/set **address pairs** |
| `$FFDE`/`$FFDF` | SAM map type 0 / 1 |
| `$FFFE`/`$FFFF` | 6809 reset vector |
| `$0071` | BASIC's warm-start flag |
| `$C000` | `"DK"` autostart signature; entry at `$C002` |

### Key constants

| Constant | Value | Where |
|---|---|---|
| bus cycle | 1117 ns (0.895 MHz) | the CoCo |
| `MC_SYS_CLK_KHZ` | 250000 | both boards |
| `MC_HSTX_CLK_KHZ` / `MC_PIO_REF_KHZ` | 125000 | videocart |
| `MC_BANK_BYTES` | 16 KB | both |
| `MC_IMAGE_BUF_BYTES` | 128 KB / 96 KB | multicart / videocart |
| `MC_MIRROR_BYTES` | 64 KB | videocart |
| `PERM_TABLE_BYTES` | 32 KB | videocart |
| `FOL_RING_WORDS` / `CAP_RING_WORDS` | 1024 / 256 (= 256 cycles) | videocart |
| `ISLAND_LINES` / `AUD_SLOTS` / `AUD_PACKETS` | 80 / 4 / 4 | videocart |
| `AUD_ISLAND_WORDS` | 271 (268 on a blanking line) | videocart |
| `MC_RESET_PULSE_MS` | 120 | both |
| `MC_STUB_TIMEOUT_MS` | 1500 | both |
| `MC_BASIC_BOOT_MS` | 2200 | both |
