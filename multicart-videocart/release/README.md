# Prebuilt firmware

So you do not have to install the Pico SDK. Two files per board, because
the two ways of flashing take different containers — same firmware
inside.

| Board | First flash | Update over WiFi |
|---|---|---|
| Multicart | `coco-multicart-0.5.1.uf2` | `coco-multicart-0.5.1.bin` |
| Videocart | `coco-videocart-0.58.0.uf2` | `coco-videocart-0.58.0.bin` |

Both boards use a **Raspberry Pi Pico 2 W**. A plain Pico 2 will take
the firmware and boot, but has no radio, so the web UI and over-the-air
updates are gone. A Pico W (RP2040) will not run this at all.

## First flash — the `.uf2`

Hold BOOTSEL while plugging the Pico into USB and copy the `.uf2` to the
`RPI-RP2350` drive that appears. Or, with the board in the same state:

```sh
picotool load coco-multicart-0.5.1.uf2 -f -x
```

A brand-new board has no WiFi credentials. Connect to its USB serial
port, press any key for the configuration menu, then `SET SSID`,
`SET PSK`, `SAVE`, `REBOOT`. The main README covers this in full.

## Later updates — the `.bin`

The board's web page has a **Firmware** section that takes the `.bin`.
It stages the image in the second flash slot, verifies it, reboots, and
copies it into place.

**Leave the power alone for about 15 seconds after that reboot.**

The uploader will not accept a `.uf2`, and the BOOTSEL drive will not
accept a `.bin`. If a flash seems to do nothing, check which one you
reached for.

## What an update leaves alone

Your WiFi credentials live in a CRC-protected sector at the very top of
flash, and your uploaded ROM library lives in a filesystem starting at
offset `0x180000`. Neither is inside the region these images write —
the firmware payload ends around `0x06A000` — so an update does not
disturb either. Keep your own copy of anything you cannot replace
regardless; that is a statement about where the bytes are, not a
warranty.

## No ROMs in here

These are firmware images only. No cartridge ROMs are included, baked
in, or referenced. You supply your own, through the web uploader.

## Checksums

```
5a5f212bb0b652907fd3af303c3ba5a755820e554a415299d14a8b9a65a8ef1f  coco-multicart-0.5.1.bin
37d6aaf982ad26ec69af35cc06f558a5ae39a6f43d716e97d754580159aa0cd6  coco-multicart-0.5.1.uf2
4b7c652f4f0fb131c6dce458e5ebd0d39e419c24fe7d8421a5bc891ec49e2c19  coco-videocart-0.58.0.bin
90df3d386f5586f8d01c815773075e055d37ae5d7cf5386b01df3f9bdb130f4f  coco-videocart-0.58.0.uf2
```

Verify with `sha256sum -c`, or `shasum -a 256` on a Mac.

## Building it yourself instead

See **Building** in the main README. `make multicart` and
`make videocart` produce exactly these files.
