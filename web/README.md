# TamaPoke web installer — Mrs. Spock - Version - Germany

A one-click page that flashes the firmware and loads the sprites from the browser
(Chrome/Edge), with no Arduino or drivers. It uses
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) to flash and **Web
Serial** to push the sprites to the SD card with the firmware's `PUT` protocol.

## Contents

| File | Purpose |
|---|---|
| `index.html` | the page (flashing + sprite loader) |
| `manifest.json` | ESP Web Tools config: the four firmware parts and their flash offsets. Its `version` is shown in the page heading |
| `firmware/TamaPoke.ino.bootloader.bin` | flashed at `0x0` |
| `firmware/TamaPoke.ino.partitions.bin` | flashed at `0x8000` |
| `firmware/boot_app0.bin` | flashed at `0xe000` |
| `firmware/TamaPoke.ino.bin` | the program, flashed at `0x10000` |
| `gen1-kanto.pak`, `gen2-johto.pak`, `gen3-hoenn.pak`, `gen4-sinnoh.pak` | the four sprite packs (TPAK bundles), see below |

The firmware is flashed as four parts, **not** as one merged 16 MB image. The merged
image also contains the empty save-game area and would erase the saved Pokémon even
when "Erase" is not ticked. With the four parts, a normal install keeps the save.

Everything else in an Arduino export folder (`.elf`, `.map`, `.merged.bin`,
`*_flashed.bin`, `sdkconfig`, `flash_args`, `partitions.csv`, `build.options.json`)
is not used by the page and can be deleted.

## The four sprite packs

| Pack | File | Size | Contents |
|---|---|---|---|
| 1 of 4 | `gen1-kanto.pak` | ≈ 42 MB | Generation 1 · Pokémon #1–151 |
| 2 of 4 | `gen2-johto.pak` | ≈ 29 MB | Generation 2 · Pokémon #152–251 |
| 3 of 4 | `gen3-hoenn.pak` | ≈ 40 MB | Generation 3 · Pokémon #252–386 |
| 4 of 4 | `gen4-sinnoh.pak` | ≈ 35 MB | Generation 4 · Pokémon #387–493 |

About 147 MB in total.

- Each pack has its own button. If a pack is interrupted, press **its** button again:
  it continues with the file where it stopped. "start this pack over" restarts it.
  The other packs do not have to be repeated.
- **`thumbs.bin`** (the Pokédex thumbnails of all 493 Pokémon) is contained in every pack,
  always the same complete version. Therefore the order of the packs does not matter and
  repeating a pack can never overwrite it with an older one.

## Updating the firmware

1. In the Arduino IDE: **Sketch → Export Compiled Binary**.
2. Copy `TamaPoke.ino.bin`, `TamaPoke.ino.bootloader.bin`, `TamaPoke.ino.partitions.bin`
   and `boot_app0.bin` from the export folder into `firmware/`.
3. Set the new `version` in `manifest.json`. The page heading and tab title follow it.
   (`index.html` contains the same number once as a fallback in `<span id="ver">` and
   in `<title>`; change it too if you open the page without a server.)

## Test locally

Web Serial and ESP Web Tools need a **secure context**: `https://` or
`http://localhost`. Opening `index.html` by double-click (`file://`) does not work.

```bash
cd web && python3 -m http.server 8000
# open http://localhost:8000 in Chrome or Edge
```

## End-user flow

1. **Install TamaPoke** flashes the firmware (pick the USB port; leave "Erase device"
   unticked to keep your Pokémon, tick it on a fresh board).
2. **Connect board**, then **Load pack 1 / 2 / 3 / 4** one after another. Close the step-1
   tab first: only one program can use the port at a time.
3. Restart the board (hold PWR to switch off, press PWR again), choose your starter
   and play.

The firmware switches itself off after 5 minutes without a touch, even on USB, and a
transfer is not counted as a touch. While loading, tap the board's screen every few
minutes. If it switched off anyway, switch it on again, press **Connect board**, and
continue with the pack that was interrupted.

## Hosting

- The page works on any static host with HTTPS (for example GitHub Pages).
- GitHub warns about files above 50 MB and rejects files above 100 MB. The largest
  pack is about 42 MB.

All sprites are from PMD SpriteCollab, CC BY-NC (non-commercial sharing with
attribution is allowed). Pokémon is a ™ of Nintendo / Game Freak. Non-commercial project.

## Limitations

- Desktop **Chrome/Edge** only (Web Serial is not available in Firefox or Safari).
