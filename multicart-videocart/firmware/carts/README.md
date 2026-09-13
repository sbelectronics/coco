# carts/ — ROMs baked into debug images

Drop `.rom` / `.ccc` / `.bin` cartridge images here, then build a
firmware image with them pre-installed in the filesystem partition:

```
make multicart-carts       # or videocart-carts
make flash-multicart-carts # firmware + carts in one go
```

The carts appear in the OLED menu and web UI exactly as if uploaded —
no upload step per debug cycle.

Selection is either every file in this directory (default metadata), or,
if `../carts.conf` exists, exactly the files it lists with the metadata
it gives. See `../carts.conf.example`.

ROM files here are **not** checked in (copyright); this README and a
`.gitkeep` are the only tracked contents. The filesystem image only
touches the data partition — flashing carts does not disturb the
firmware slot or your saved WiFi credentials.
