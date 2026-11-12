# CLAUDE.md

Rules for agents working in **toki1-pc**. These instructions override
default behavior: follow them exactly.

## Repository purpose

**toki1-pc** is the native PC port (SDL2; Linux and Windows) of **Tokimeki
Memorial: Forever with You** (SLPS-00065, PS1, 1995), published as
*Heartthrob Memorial PC Enhanced*. It compiles the game's decompiled C
natively and replaces the console hardware with a HAL (`pc/hal/`).

The disc, setup and packaging commands are in `README.md`. It builds on:

- `../toki1-decomp` (**required**): the decompiled C (`src/main/matched`,
  `semi-matched`, `archive`, `src/overlay`), `include/`, `config/` (symbol
  ledger, `.sym` maps, function registry), the decomp tools.
- `../toki1-english` (optional): the English patch (`patch/`, `tools/`,
  `config/`, `sheets/`, `art/`). Without it the port is the Japanese game.

`tools/repo_paths.py` resolves the three roots (`TOKI_DECOMP_DIR`,
`TOKI_ENGLISH_DIR` override); `pc/tools/pc_paths.py` is the pc/tools view of
it (`DECOMP`, `ENGLISH`, `decomp()`, `english()`, `spell()`, `decomp_key()`,
`disc_input()`), and `pc/Makefile` applies the same rule (`DECOMP_DIR`,
`ENGLISH_DIR`, prefixes `D`/`E`; `make -f pc/Makefile print-parts`).
toki1-english is laid out flat while code names its files by their monorepo
paths (`config/patch/english.json`): `english()`/`english_path()` and the
Makefile's `E_SRC`/`E_TOOLS`/`E_CONFIG` map them through
`tools/repo_paths.py` `ENGLISH_LAYOUT`; never join a path onto `ENGLISH`.
**Never build a path into another part from this repository's root, and
never write into a sibling checkout**: output stays in `pc/build`, `pc/build-*` and
`build/`.

**Legal**: the player supplies the disc. No game data, no disc-derived file,
no screenshot of the game is ever committed or packaged.

## Layout

| path | what |
|---|---|
| `pc/hal/` | the hardware layer: GPU, SPU (DuckStation core), CD, pad, timers, IRQ, presenter, settings, setup screen, translation applier |
| `pc/src/` | PC entry point, globals, generated symbol tables; `overlay/<OV>/` overlay overrides; `overlay_inits/` |
| `pc/src/overlay_<OV>.c` | **generated, not tracked**: the per-overlay modules (`pc/tools/generate_overlay_modules.py` from toki1-decomp's `src/overlay`); the first build makes them, `make -f pc/Makefile overlay-modules` refreshes them |
| `src/main/pc-port/` | overrides of main-binary functions (each modelled on the matched C) |
| `pc/tools/` | generators, audits, playthroughs, scene matrix, packagers, unit tests |
| `pc/tests/` | native unit tests (`make -f pc/Makefile test`) |
| `docs/pc-port/` | `PLAYER-GUIDE.md` (players), `STATE-MACHINES.md` (the PC layer's state machines: owners, threads, invariants, tests) |

## PC Port Policies (read before touching `src/main/pc-port/` or `pc/`)

**NO ASSET CHEATING**: the port must render via the original game code driven by extracted
assets (VAB/SEQ/TIM/NBN/TMD/XA).
- BANNED: pre-rendered bitmaps, hardcoded "look-correct" textures, shims that
  short-circuit a screen's real render pipeline.
- REQUIRED: every override in `src/main/pc-port/` calls the same wrapper functions the PS1
  JALs target, in the same order, with the same args. PC-specific code goes in HAL modules
  (`pc/hal/*`) and adapts data formats, not gameplay/render logic.
- If a real function renders wrong on PC, fix the HAL or missing globals; don't paper over
  it. **FMV exception**: pre-baked `.STR`->`.fmv`+`.wav` via `pc/tools/bake_fmv.py` is
  allowed (the PS1 API contract stays intact, only the decoder is swapped).

**PS1 IS SOURCE OF TRUTH**: overrides must reproduce PS1 control flow, global writes, side
effects, and scene/sub-state transitions.
- REQUIRED: before writing an override, read the matched C in toki1-decomp (or its
  disassembly) and model the override on it.
- BANNED: PC-only timers, auto-advances, or scene jumps that don't exist in the PS1 binary.
  If PS1 code waits on async events, supply those events via the HAL; don't fake the advance.
- Intentional divergence MUST be documented in the override header. Undocumented divergence
  is a bug.

**The decompiled C is not edited for the PC.** Fix-ups the native build
needs are applied at build time from plans (`pc/tools/ps1_source_edits.py`
with `pc/tools/*_plan.json`; `adapt_ps1_cells.py`, `adapt_ps1_addresses.py`)
keyed by the decomp file's repository path (`src/main/matched/x.c`); a plan
entry whose text no longer matches stops the build. A real bug in the
decompiled C is fixed in toki1-decomp.

## Build, run, test

```bash
pc/3rd/spu-glue/fetch.sh                  # once (the SPU core is fetched, never committed)
make -f pc/Makefile                       # uses every core; first build also generates the overlay modules
make -f pc/Makefile test                  # native unit tests
python3 pc/tools/run_unittests.py         # pc/tools tests, all cores
pc/tools/run_battery.sh build/battery     # the full battery
```

The battery (`pc/tools/run_battery.sh`): stage 1 regenerates the overlay
modules, rebuilds the English pack when toki1-english and its patch staging
are present, builds and runs the native tests; stage 2 runs the pc/tools
unit tests, the seven audits (`audit_ps1_{pointer_casts,wide_access,
pointer_records,call_args,pointer_cells}.py`, `audit_pc_stubs.py`,
`audit_switch_tables.py`), the scene matrix and the playthroughs
`new_game`, `date_save`, `spring_events` in parallel. Every one must pass
before you report a change done; quote its summary lines.

- **Run the battery in a separate checkout or worktree**, never in the one
  whose `pc/build` holds a player's game data and saves.
- **Never boot the game visibly**: automated runs use dummy SDL drivers
  (`SDL_VIDEODRIVER=dummy`, `SDL_AUDIODRIVER=dummy`); Wine runs with
  `DISPLAY`/`WAYLAND_DISPLAY` unset.
- Automated runs ignore the player's settings file and run in Japanese
  (parity), which stays the oracle.
- English-only tests skip by name without toki1-english.
- Every PC feature must work on Windows too (`make -f pc/Makefile windows`);
  a Linux-only feature is a bug.
- After toki1-decomp renames symbols: `pc/tools/sync_symbol_names.py --apply`,
  `make -f pc/Makefile overlay-modules`, and a clean build.
- Do not write `until ! pgrep -f X` wait loops (they match themselves), and
  never `pkill -f` a pattern that can match your own shell.

## Packaging

`pc/tools/package_windows.py` / `package_linux.py` (after `make -f pc/Makefile
windows` / `linux-package`): the program and `README.txt` only, licences
inside the binary. With toki1-english present the packagers require the
embedded English pack; without it they say they package the Japanese-only
build. Each package gets its GPL corresponding source
(`pc/tools/source_archive.py`: every checkout the binary was built from,
plus the fetched SPU core). Never package game data.

## Licence and credits

GPL-3.0-or-later (`LICENSE`); `pc/hal/gpu.c` and `pc/hal/cd_audio.c` are
GPL-3.0-only (DuckStation-derived). `THIRD-PARTY.md` credits DuckStation and
Mednafen by name, and every vendored component: never remove a credit, and
add any new third-party file to it with its terms.

## Code style

C for the HAL and overrides: `/* */` comments, 4-space indent, declare
variables at the top of a block. Overrides keep their divergence headers.
Game names follow toki1-decomp's symbol ledger (`config/symbol_names.json`).

## CI

`.github/workflows/build.yaml` builds the port and runs the native tests
without a disc (`PC_TESTS_NO_DISC=1`), with toki1-decomp checked out beside
this repository. Mirror: `tools/ci/run_ci_locally.sh` (a clean checkout of
HEAD next to a toki1-decomp checkout). Run it before every push; it must end
`CI would pass`. Change the workflow and the script in the same commit.

## Git hygiene

- Commit with explicit paths (`git commit -- <paths>`); never `git add -A`.
- Never commit `pc/assets`, `pc/assets-en`, `disks/` (in toki1-decomp), saves, or the
  generated overlay modules.
