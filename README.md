# toki1-pc

A native PC port (Linux and Windows) of **Tokimeki Memorial: Forever with
You** (Konami, 1995, PlayStation, SLPS-00065), released to players as
*Heartthrob Memorial PC Enhanced*. It compiles the decompiled game C and
replaces the console hardware with its own layer (`pc/hal/`).

Unofficial fan project, not affiliated with Konami or Sony. No game data is
included or packaged: everything is read from your own disc.

## Disc

Japan, Rev 1, raw BIN/CUE. No BIOS needed. The port checks it and extracts
the game data on first run; it accepts only:

| file | size | SHA-1 |
|---|---:|---|
| Track 1 | 702,104,928 | `7b2f72d1319db27d0700070aa0bffe886b5ef7a4` |
| Track 2 (optional) | 45,151,344 | `6cdd8500168ab4ecb18b78ef7589586d26bc4501` |
| `SLPS_000.65` (inside Track 1) | 690,176 | `2d8d474816e4bf937b09dc3b017f550f7ce4e14b` |

Players: put the `.cue` and `.bin` files in a `disk` folder beside the
program, or drag the `.cue` onto the setup screen. See
`docs/pc-port/PLAYER-GUIDE.md`.

## Build

Needs `../toki1-decomp` (the game's C) with the disc in its `disks/`.
`../toki1-english` is optional: with it the build is in English.
Linux or WSL with `gcc`, `g++`, `make`, `clang`, SDL2 development files,
`curl`, `patch`, Python 3.10+.

```bash
(cd ../toki1-decomp && make setup)            # disc check, extraction, venv
. ../toki1-decomp/.venv/bin/activate && pip install -r requirements.txt
pc/3rd/spu-glue/fetch.sh                      # once: the sound core
make -f pc/Makefile                           # -> pc/build/tokimeki-pc (about 8 minutes the first time)
make -f pc/Makefile assets DISC_PATH='../toki1-decomp/disks/Tokimeki Memorial - Forever with You (Japan) (Rev 1) (Track 1).bin'
make -f pc/Makefile test                      # 64/64
./run_game.sh --disc '../toki1-decomp/disks/Tokimeki Memorial - Forever with You (Japan) (Rev 1).cue'
```

English:

```bash
(cd ../toki1-decomp && make setup-ido52 check-clean-main check-exn-all)
(cd ../toki1-english && pip install -r requirements.txt && make patch)
python3 pc/tools/translation_pack.py build    # -> pc/build/translation/en.tktp
make -f pc/Makefile                           # embeds it
```

Full test battery, in a checkout you do not play from:
`PY=$(which python3) pc/tools/run_battery.sh build/battery`.

Packages: `pc/tools/setup_windows_toolchain.sh`, `make -f pc/Makefile
windows`, `python3 pc/tools/package_windows.py`. For Linux:
`pc/tools/setup_linux_pkg_root.sh`, `make -f pc/Makefile linux-package`,
`python3 pc/tools/package_linux.py`.

## Known issues

- Three optional pieces of signage art are still Japanese in the English
  build (culture-festival drama act cards and hero-show logo, a tiny line
  on the album cover); the PS1 patch has the same gap.
- After an ending, the system save differs from a console's in 8 bytes
  (not yet traced).
- Not frame-exact everywhere: some scenes drop frames differently from the
  console, and the title seeds the random generator with a slightly
  different frame count, so random events can differ from a console run on
  the same input.
- The automated tests reach about 56% of the game's live code.

## Layout

| path | what |
|---|---|
| `pc/hal/` | hardware layer, settings, setup screen, translation applier |
| `pc/src/` | entry point, globals, overlay overrides |
| `src/main/pc-port/` | PC overrides of main-binary functions |
| `pc/tools/` | generators, audits, playthroughs, packagers, tests |
| `pc/tests/` | native unit tests |
| `pc/3rd/` | third-party code |
| `docs/pc-port/` | player guide, the PC layer's state machines (other `docs/` files cited in comments are unpublished notes) |

Rules for changing the port: `CLAUDE.md`.

## Licence

GPL-3.0-or-later (`LICENSE`). The decompiled C it compiles is
toki1-decomp's (MIT). Credits and third-party terms, including DuckStation
and Mednafen: `THIRD-PARTY.md`.
