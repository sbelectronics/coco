# Firmware tree layout

Three cartridges share one codebase where it makes sense to and split
where it doesn't:

```
firmware/
  common/       board-agnostic code — ~75% of the total
  multicart/    Pico 2 W, direct-GPIO bus
  videocart/    Pico 2 W, '153 mux bus + HDMI video/audio
  test/         host-side simulation tests (`make test`);
                test/results/ holds reviewed reference renders
  tools/        mklfs (baked-cart images), mkdiag (bank diagnostic cart),
                preflight (pre-flash gate)
  lib/littlefs/ vendored third-party
gateware/
  tang-videocart/  Tang Nano 9K — Verilog, not C [not written]
```

## The seam

**`common/bus.h` is the boundary.** It declares `bus_serve()`,
`bus_staging()`, `bus_idle()`, `bus_stub_arm/seen/disarm()` and the
bank-scheme enum. Nothing above that line knows how the bus is wired —
whether the address arrives on 14 direct GPIOs (multicart) or through a
4-phase 74HCT153 mux sweep (videocart). `select.c`, `romfs.c`, `ui.c`
and `web.c` all consume the interface without a board-specific
assumption.

A board therefore owns exactly four files:

| File | Why it's board-specific |
|---|---|
| `config.h` | pin map, I2C addresses, flash layout, timing constants |
| `bus.c` | the bus service implementation behind `bus.h` |
| `bus_pio.pio` | that board's PIO programs |
| `main.c` | boot order and board-specific init |

Everything else lives in `common/`. **Fix bugs there, not in a copy** —
the whole point of this split is that the shared code exists once.

## How the videocart uses the split

It is the worked example of the seam paying off: `videocart/` supplies
only `config.h`, `bus.c` (mux sweep + DMA rings instead of direct pins),
`bus_pio.pio`, `main.c`, plus its video and audio files (`hstx.c`,
`vdg.c`, `audio.c`, `video.h`, `audio.h`, `sweep.h`, `vdgfont.h`) — and
inherits menu, ROM storage, web UI, selection, OTA and CDC provisioning
from `common/` unmodified.

Two capabilities `common/` gained for it, both harmless on the multicart:

- `bus.h` declares optional full-bus accessors (`bus_mirror()`,
  `bus_write_count()`, `bus_capture_behind()`, `bus_scs_disagree()`).
  The multicart returns NULL/0 and shared code checks `bus_mirror()`
  before reporting them.
- `web.c` serves `GET /api/mirror?addr=<hex>&len=<dec>` — a hex window
  into the snooped 64K RAM shadow, 404 on boards without one. It is the
  fastest way to tell "capture is broken" from "renderer is broken".

## Note on the Tang board

`gateware/tang-videocart/` is deliberately *not* under `firmware/`: it is
Verilog for a Gowin toolchain plus a softcore, sharing concepts rather
than code with the Pico boards. The one thing it can reuse directly is
`common/stub6809.h` (the cold-boot stub bytes) and the ROM catalog
format. The existing `coco-hdmi` interposer project is the model for how
that tree should look, and its 6847 core is the renderer to port.
