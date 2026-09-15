# Bill of Materials — Multicart

A ROM cartridge for the TRS-80 Color Computer 1/2 built around a
Raspberry Pi Pico 2 W: one buffer, one I/O expander, a handful of
passives and a screen. The other board is the
[videocart](videocart.md), which adds HDMI out.

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
| D1 | 1 | Small-signal diode, DO-35 axial | `1N4148` | -- | **Do not omit this.** GP28 is an ADC pad and is *not* 5 V tolerant. Anode on the GP28 strobe net, cathode on the buffer output, so the 5 V side can only ever pull the pin down and R3 pulls it back up to 3.3 V. |
| D2 | 1 | Schottky diode, 40 V 1 A, DO-41 axial | `1N5819` | -- | Cart 5 V into the Pico's VSYS. The low forward drop matters, and it diode-ORs with USB so bench power and cart power can coexist. |
| IC1 | 1 | 8-bit I2C I/O expander, PDIP-16 | `PCF8574N` | `595-PCF8574N` | A2-A0 tied to GND, so it answers at 0x20. It must be a plain PCF8574 and **not** a PCF8574**A**, which answers at 0x38 instead. Carries the encoder and the two control-line enables. NXP's PCF8574P is the same part in the same package. |
| IC2 | 1 | Quad bus buffer, 3-state, PDIP-14 | `SN74HCT125N` | `595-SN74HCT125N` | Runs from cart 5 V. Three sections used: the CART* autostart gate, the SCS write strobe, and !RESET -- that last one driven open-drain style by enabling a section whose input is grounded, so the board can never fight the CoCo for the line. **HCT, not HC:** the Pico drives these inputs at 3.3 V, which only the TTL thresholds of an HCT part read cleanly from a 5 V supply. |

## Passives

| Ref | Qty | Description | Mfr part | Mouser | Notes |
|---|---|---|---|---|---|
| C1, C2 | 2 | 0.1 µF ceramic, 2.5 mm lead pitch, 50 V | any | -- | Decoupling: one on the 5 V rail at the buffer, one on the 3.3 V rail at the expander. Place each at the supply pin. |
| C3, C8 | 2 | 10 µF ceramic, 5 mm lead pitch, 16 V or higher | any | -- | Bulk on the cart 5 V rail and on VSYS. Ceramic keeps every capacitor on the board non-polarised, so there is nothing to fit backwards. |
| R1-R3 | 3 | 4.7 kΩ resistor, 1/4 W axial | any | -- | Two I2C pull-ups to 3.3 V, plus the strobe pull-up that holds GP28 high when the board is on USB power alone and the 5 V buffer is dead. |

## Connectors and mechanical

| Ref | Qty | Description | Mfr part | Mouser | Notes |
|---|---|---|---|---|---|
| J2 | 1 | 1x4 header, 2.54 mm | any | -- | OLED. Pin order **GND, VDD, SCK, SDA** -- see the OLED module entry below. |
| SW1 | 1 | Rotary encoder with push switch, 12 mm, through-hole | any | -- | ALPS EC12E footprint: a 3-pin quadrature side and a 2-pin switch side. The generic 5-pin encoders sold in bulk on eBay are the usual source and fit; check the pin arrangement and the mounting lugs against the footprint before buying a bagful. No external pull-ups -- the expander's internal ones serve. |

## Modules

| Ref | Qty | Description | Mfr part | Mouser | Notes |
|---|---|---|---|---|---|
| U1 | 1 | Raspberry Pi Pico 2 W (RP2350 + CYW43439) | `Raspberry Pi Pico 2 W` | -- | It must be a Pico 2 **W**: the RP2350 for the PIO and HSTX blocks, the radio for the web UI and firmware updates. A Pico 2 fits the footprint and boots, but loses every network feature. Solder the castellations or fit two 1x20 female strips -- see Sockets. |

## PCB features (no part to buy)

| Ref | Item | Notes |
|---|---|---|
| J1 | CoCo cartridge edge connector, 40 way | Etched into the board, not a part. Order the PCB **1.6 mm thick with a bevelled edge**, gold fingers preferred. Verify which physical side pins 1 and 2 land on against a real cartridge before committing a fab order. |
| SJ1, SJ2 | Solder jumper | **Must both be bridged.** They tie off the one unused buffer section: one grounds its input, the other holds its output enable high. Two CMOS inputs float if these ship open. |
| U$1 | Test point pad | A bare 5 V pad. The write strobe has no pad of its own -- probe it at `R3`, which sits on that net. Scoping it against SCS* activity is the first bring-up task. |

## Sockets

Sockets are an assembly choice, not a layout item, so they do not
appear in the source above. Socket every DIP: these boards get
plugged into a 40-year-old machine, and a chip you can swap in ten
seconds is worth the two millimetres of height.

| For | Qty | Part | Notes |
|---|---|---|---|
| U1 | 1 | Two 1x20 female header strips, 2.54 mm | For the Pico, if you do not want to solder it down permanently. Buy a 40-way strip and cut it in half. |
| IC1 | 1 | 16-pin DIP socket | For the PCF8574. |
| IC2 | 1 | 14-pin DIP socket | For the 74HCT125N. |

## Not on the board, but you still need them

| Item | Qty | Notes |
|---|---|---|
| SSD1306 OLED module, 128x64, I2C | 1 | GME12864-51 or any SSD1306 128x64 I2C module with the **GND, VDD, SCK, SDA** pin order. Other vendors swap GND and VDD; check the silkscreen on the module against the silkscreen on the board before plugging it in, because reversing them kills the module. |
| Micro-USB cable | 1 | First firmware flash only. Everything after that goes over WiFi from the web UI. |
| Cartridge shell | 1 set | **3D printed.** This board is not a standard cartridge outline, so a shell salvaged from a dead cartridge will not fit it. Print `coco-cartridge-multicart-top.stl` and `coco-cartridge-multicart-bottom.stl` from `3d-printer/`. Each half is about 90 x 108 mm in plan, so any common bed has room. |
| M3 heat-set threaded insert | 1 | Brass, knurled, melted into the printed boss with a soldering iron -- printed threads in PLA strip the second time you open the shell, these do not. Takes the M3 screw below. Not a distributor line: eBay or AliExpress, searching **M3 brass heat set threaded insert knurled**, usually sold as a mixed M2/M2.5/M3 assortment. Match the insert length to the depth of the boss. |
| M3 tapered machine screw | 1 | Tapered (countersunk) head, so it finishes flush in the printed counterbore, threading into the insert above. eBay again -- a small assorted box of black M3 countersunk screws costs less than one distributor line item. |
| Encoder knob | 1 | **3D printed:** `encoder-knob-coco-multicart.stl` in `3d-printer/`, the same knob on both boards. About 19 mm across. If it will not press onto the encoder shaft, **scale the whole model up 1-3%** and print it again -- that opens the bore without changing anything else. Go up in small steps; a knob that is loose cannot be un-printed. |

