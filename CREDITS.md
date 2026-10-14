# Credits

TamaPoke is a **non-commercial, personal-use** project. It does not sell or
commercially redistribute any copyrighted material. Pokémon and all related
names, designs and characters are trademarks and © of **Nintendo / Game Freak /
The Pokémon Company**.

This project is not affiliated with or endorsed by any of those companies.

## Sprites and data

| Resource | Source | Use in the project |
|---|---|---|
| **All sprites** (idle, walk, sleep, eat, hurt, attack…) | [PMD Sprite Collaboration (PMDCollab/SpriteCollab)](https://github.com/PMDCollab/SpriteCollab) | Mystery-Dungeon-style animated sprites used everywhere: main screen, stat card, minigame, and the Pokédex grid + detail view |
| **Gen 1 base stats** | [PokéAPI](https://pokeapi.co) | Real ATK/DEF/SPD/HP for each species |
| **Gen 4 (Sinnoh, #387-493) data** (since 6.27) | names, typings, evolutions, base stats: [crenberries428/TamaPoke](https://github.com/crenberries428/TamaPoke) (MIT), checked against [PokéAPI](https://pokeapi.co) data; German names: PokéAPI | Dex entries 387-493 in `dex.h`, German names in `DEX_NAME_DE` |

The **SpriteCollab** sprites are the work of its community of artists under their
own terms (Creative Commons Attribution-NonCommercial 4.0). Per-species/per-author
credit is in the original repository's
[tracker.json](https://github.com/PMDCollab/SpriteCollab/blob/master/tracker.json).
Huge thanks to that whole community for an enormous amount of work.

> **Important if you reuse this repo:** the packaged sprite files
> (`tools/sdcard/mons/*.bin`) are derived from the sources above. Don't
> redistribute them commercially. If you publish the project, the clean approach
> is to distribute **only the code and scripts**, and have each user download and
> package the sprites from the original sources with `tools/pack_*.py` (or the web
> installer).

## Software / hardware

| Component | Author / source |
|---|---|
| GFX Library for Arduino | [moononournation](https://github.com/moononournation/Arduino_GFX) |
| SensorLib (CST9217 touch, PCF85063 RTC) | [Lewis He / lewisxhe](https://github.com/lewisxhe/SensorLib) |
| XPowersLib (AXP2101 PMU) | [Lewis He / lewisxhe](https://github.com/lewisxhe/XPowersLib) |
| Board and pinout | [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) |
| Web installer | [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (Nabu Casa) |
| Optik B (since 6.0): sun, moon, stars, poop with flies, floating heart, snoring Zs, bath bubbles, eating animation; (since 6.1) clouds, care-button tiles and icons for food/light/clean/berries/candy (the minigame pokeball icon is drawn from the project owner's own pixel image); (since 6.2) the seven background scenes; (since 6.23) the small background creatures (PMD SpriteCollab sprites, CC BY-NC, walk animations only, embedded in bgmons.h) | adapted from [crenberries428/TamaPoke](https://github.com/crenberries428/TamaPoke) (MIT), itself a fork of [socquique/TamaPoke](https://github.com/socquique/TamaPoke) |

TamaPoke's own code (firmware and tools) is original work.
