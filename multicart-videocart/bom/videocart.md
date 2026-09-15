# Bill of Materials — Videocart

The [multicart](multicart.md) plus HDMI video and audio. The parts
it adds are the three multiplexers that squeeze a 16-bit address
into the pins HSTX leaves over, and the breakout that carries the
HDMI connector itself.

This file is generated from the part list exported from the
fabricated board, so the reference designators and quantities
cannot drift away from what is actually on the PCB. Do not edit
it by hand.

**The whole board is through-hole.** No surface-mount parts, by
design, apart from the Pico module itself.

Manufacturer part numbers are authoritative and searchable at any
distributor. Mouser numbers are a convenience -- worth a glance at
order time, since distributor numbers change more often than MPNs
do. Where a row says `any`, it means any: the value is what
matters, not the maker.

## Semiconductors

| Ref | Qty | Description | Mfr part | Mouser | Notes |
|---|---|---|---|---|---|
| D2 | 1 | Schottky diode, 40 V 1 A, DO-41 axial | `1N5819` | -- | Cart 5 V into the Pico's VSYS. The low forward drop matters, and it diode-ORs with USB so bench power and cart power can coexist. |
| IC1 | 1 | 8-bit I2C I/O expander, PDIP-16 | `PCF8574N` | `595-PCF8574N` | A2-A0 tied to GND, so it answers at 0x20. It must be a plain PCF8574 and **not** a PCF8574**A**, which answers at 0x38 instead. Carries the encoder and the two control-line enables. NXP's PCF8574P is the same part in the same package. |
| U3 | 1 | Quad bus buffer, 3-state, PDIP-14 | `SN74HCT125N` | `595-SN74HCT125N` | Runs from cart 5 V. Two sections used -- the CART* autostart gate and !RESET -- and the other two are tied off by the solder jumpers. **HCT, not HC:** the Pico drives these inputs at 3.3 V, which only the TTL thresholds of an HCT part read cleanly from a 5 V supply. |
| U4-U6 | 3 | Dual 4-input multiplexer, PDIP-16 | `SN74HCT153N` | `595-SN74HCT153N` | Five of the six sections carry A0-A15, R/W, CTS* and SCS* to the Pico in four swept phases; this is how a 16-bit address reaches a board with no pins left for it. **HCT, not HC:** the Pico drives these inputs at 3.3 V, which only the TTL thresholds of an HCT part read cleanly from a 5 V supply. Their propagation delay is inside the serve deadline, so do not substitute a slower family. |

## Passives

| Ref | Qty | Description | Mfr part | Mouser | Notes |
|---|---|---|---|---|---|
| C1, C2, C4-C6 | 5 | 0.1 µF ceramic, 2.5 mm lead pitch, 50 V | any | -- | Decoupling: one per 5 V logic IC, plus one on the 3.3 V rail at the expander. Place each at the supply pin. |
| C3, C8 | 2 | 10 µF ceramic, 5 mm lead pitch, 16 V or higher | any | -- | Bulk on the cart 5 V rail and on VSYS. Ceramic keeps every capacitor on the board non-polarised, so there is nothing to fit backwards. |
| F4 | 1 | Pico fuse, 750 mA fast-blow, axial | `0251.750NRT1L` | `576-0251.750NRT1L` | In series with the whole board's 5 V feed -- **it protects the CoCo, not the cartridge.** It is here because the HDMI connector is wired by hand and cart 5 V runs out to it, and a hand-wired 5 V lead is exactly the thing that shorts. Its ~0.15 Ω still leaves about 4.9 V at the HDMI 5 V pin. 1 A of the same series is fine if you want more headroom for the radio. The multicart has no equivalent part, and needs none -- no 5 V leaves that board. |
| R1, R2 | 2 | 4.7 kΩ resistor, 1/4 W axial | any | -- | I2C pull-ups to 3.3 V. The mux outputs get none -- they are continuously driven push-pull. |

## Connectors and mechanical

| Ref | Qty | Description | Mfr part | Mouser | Notes |
|---|---|---|---|---|---|
| J2 | 1 | 1x4 header, 2.54 mm | any | -- | OLED. Pin order **GND, VDD, SCK, SDA** -- see the OLED module entry below. |
| J4 | 1 | 1x11 header, 2.54 mm | any | -- | Mates the DVI breakout's 11-way row: four TMDS pairs with interleaved grounds. Female strip on this board and male pins on the breakout, or the reverse -- whichever keeps the stack inside the shell. |
| SW1 | 1 | Rotary encoder with push switch, 12 mm, through-hole | any | -- | ALPS EC12E footprint: a 3-pin quadrature side and a 2-pin switch side. The generic 5-pin encoders sold in bulk on eBay are the usual source and fit; check the pin arrangement and the mounting lugs against the footprint before buying a bagful. No external pull-ups -- the expander's internal ones serve. |

## Modules

| Ref | Qty | Description | Mfr part | Mouser | Notes |
|---|---|---|---|---|---|
| U1 | 1 | Raspberry Pi Pico 2 W (RP2350 + CYW43439) | `Raspberry Pi Pico 2 W` | -- | It must be a Pico 2 **W**: the RP2350 for the PIO and HSTX blocks, the radio for the web UI and firmware updates. A Pico 2 fits the footprint and boots, but loses every network feature. Solder the castellations or fit two 1x20 female strips -- see Sockets. |

## PCB features (no part to buy)

| Ref | Item | Notes |
|---|---|---|
| J1 | CoCo cartridge edge connector, 40 way | Etched into the board, not a part. Order the PCB **1.6 mm thick with a bevelled edge**, gold fingers preferred. |
| J5, TP_E, TP_M4, U$1 | Test point pad | Bare pads. `TP_E` is the E clock and `TP_M4` one mux output -- scope those two together to confirm the write-capture delay. `J5` and `U$1` are 5 V; the DVI breakout takes its 5 V from one of them by wire, and it needs it, because a sink will not assert hot-plug detect without it. |
| SJ1-SJ4 | Solder jumper | **Must all four be bridged.** They tie off the two unused buffer sections: two ground the spare inputs, two hold the spare output enables high. Four CMOS inputs float if these ship open. |

## Sockets

Sockets are an assembly choice, not a layout item, so they do not
appear in the source above. Socket every DIP: these boards get
plugged into a 40-year-old machine, and a chip you can swap in ten
seconds is worth the two millimetres of height.

| For | Qty | Part | Notes |
|---|---|---|---|
| U1 | 1 | Two 1x20 female header strips, 2.54 mm | For the Pico, if you do not want to solder it down permanently. Buy a 40-way strip and cut it in half. |
| IC1 | 1 | 16-pin DIP socket | For the PCF8574. |
| U3 | 1 | 14-pin DIP socket | For the 74HCT125N. |
| U4-U6 | 3 | 16-pin DIP socket | For the 74HCT153Ns. |

## Not on the board, but you still need them

| Item | Qty | Notes |
|---|---|---|
| Adafruit DVI Breakout Board #4984 | 1 | The HDMI connector, and **the 220 Ω series resistors on all eight TMDS conductors**. This board deliberately carries none of its own, so the breakout is not a convenience -- driving a sink without those resistors is out of spec. Adafruit, or any of their distributors. |
| SSD1306 OLED module, 128x64, I2C | 1 | GME12864-51 or any SSD1306 128x64 I2C module with the **GND, VDD, SCK, SDA** pin order. Other vendors swap GND and VDD; check the silkscreen on the module against the silkscreen on the board before plugging it in, because reversing them kills the module. |
| HDMI cable | 1 | Any. The link is DVI-compatible, so a plain HDMI-to-DVI cable drives an old monitor too -- without audio, which DVI has no way to carry. |
| Micro-USB cable | 1 | First firmware flash only. Everything after that goes over WiFi from the web UI. |
| Cartridge shell | 1 set | **3D printed**, like the multicart's -- neither board is a standard cartridge outline -- and this one also has to clear the DVI breakout and give the HDMI connector a way out. From `3d-printer/`, print `coco-cartridge-videocart-top.stl`, plus **the bottom that matches your board revision**: `coco-cartridge-videocart-bottom-cutouts.stl` for hardware **v0.2**, or `coco-cartridge-videocart-bottom.stl` for **v0.3**. They are otherwise the same part: v0.2 had a few footprints sitting close to the board edge and the shell needed openings to clear them, which v0.3 moved inboard. Each half is about 100 x 108 mm in plan. |
| M3 heat-set threaded insert | 3 | Brass, knurled, melted into the printed boss with a soldering iron -- printed threads in PLA strip the second time you open the shell, these do not. Takes the M3 screw below. Not a distributor line: eBay or AliExpress, searching **M3 brass heat set threaded insert knurled**, usually sold as a mixed M2/M2.5/M3 assortment. Match the insert length to the depth of the boss. |
| M3 tapered machine screw | 3 | Tapered (countersunk) head, so it finishes flush in the printed counterbore, threading into the insert above. eBay again -- a small assorted box of black M3 countersunk screws costs less than one distributor line item. |
| Encoder knob | 1 | **3D printed:** `encoder-knob-coco-multicart.stl` in `3d-printer/`, the same knob on both boards. About 19 mm across. If it will not press onto the encoder shaft, **scale the whole model up 1-3%** and print it again -- that opens the bore without changing anything else. Go up in small steps; a knob that is loose cannot be un-printed. |

## Ordering the logic

Every 5 V part here reads inputs the Pico drives at 3.3 V, so all of it
is HCT rather than HC: an HC part wants 3.5 V for a logic 1 and reads
those as marginal. The schematic symbols were drawn from the 74HC
library, so if you ever work from the raw part list rather than from
this file, the **value** field is what to order, not the device name.

