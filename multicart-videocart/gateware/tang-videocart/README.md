# TANG-VIDEOCART gateware — not yet written

Target: Tang Nano 9K (Gowin GW1NR-9), Verilog.

This board is a Sipeed Tang Nano 9K (Gowin GW1NR-9), so it is Verilog +
a softcore rather than C — which is why it sits here and not under
`firmware/`. Expected shape, following the existing `coco-hdmi`
interposer project:

```
rtl/          bus capture, ROM serve engine, bank latch, 64K mirror
              (HyperRAM), VDG renderer, HDMI TMDS + audio islands
  *.cst       pin constraints — the header map is already worked out in
              the board notes, including a paste-ready stub
soft/         picorv32-class softcore firmware: menu, OLED, encoder,
              flash filesystem, UART upload
```

## What can be lifted rather than written

- **The 6847 renderer** from the coco-hdmi project's `rtl/` — that core
  already exists and is the single biggest piece of work here.
- **The HDMI section of `coco-hdmi.cst`** verbatim: it documents three
  gotchas that each silently produce a bitstream that builds and boots
  but emits no signal.
- **`firmware/common/stub6809.h`** — the cold-boot stub bytes are CPU
  code, identical on every board.
- **The ROM catalog format** (`file|title|autostart|scheme`) so a
  library is portable between cartridges.

## Reminder from the design pass

The 3.3V-bank header budget is exactly full (29 I/O). SCS* and Q are not
wired; CART*/RESET*/DIR are driven by 2N3904s from 1.8V-bank pins and
all three **invert**. See the schematic's pin tables before writing the
`.cst`.
