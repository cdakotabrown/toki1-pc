# The SPU core: fetch it before building the PC port

The PC port's sound chip (the PlayStation SPU: voices, ADSR, reverb, SPU RAM)
is **DuckStation's SPU** (<https://github.com/stenzek/duckstation>, © 2019-2024
Connor McLaughlin, GPL-3.0), built as a small static library (`spu_psx.a`) and
linked into `tokimeki-pc`. **Its source is not in this repository.** You fetch
six files from DuckStation at one pinned commit; this project's own pieces (stub headers, a C shim, three
patches, a checksum list and a fetch script) are in the tree under
`pc/3rd/spu-glue/`.

Without the fetched core, `make -f pc/Makefile all` stops with:

    error: the SPU core is missing or out of date in .../pc/3rd/spu-core.
           Fetch it with pc/3rd/spu-glue/fetch.sh (see pc/3rd/SPU-CORE.md).

## Quick path (one command)

From the repository root:

```sh
pc/3rd/spu-glue/fetch.sh          # needs network: curl (or wget), sha256sum, patch
make -f pc/Makefile all           # builds pc/3rd/spu-core/spu_psx.a, then the port
make -f pc/Makefile test          # the native tests (PC_TESTS_NO_DISC=1 without a disc)
```

`fetch.sh` prints each file, `OK` for each checksum, each patch, and
`SPU core ready: ...`. Anything else is a failure: see Troubleshooting.

The rest of this document is the same procedure spelled out, so you (or a
coding agent) can do it by hand, check each step, or do it offline.

## Source and pin

The source is DuckStation, at the commit below. These lines are read by
`fetch.sh`, so the URL and the pin are written once.

```
SPU_CORE_REPO=https://github.com/stenzek/duckstation.git
SPU_CORE_RAW=https://raw.githubusercontent.com/stenzek/duckstation
SPU_CORE_COMMIT=aa955b8ae28314ae061613f0ddf13183a98aca03
```

**Why this commit.** It is the parent of DuckStation commit `7f4e5d55d`
("Misc: Update copyright headers"), the last revision in which
`src/core/spu.cpp` and `src/core/spu.h` carry the dual licence
`SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)`. Later revisions are
CC-BY-NC-ND only, which this project cannot use. We take the files under
**GPL-3.0**. Do not move the pin forward. Check it:

```sh
head -2 pc/3rd/spu-core/pristine/spu.cpp
# // SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
# // SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)
```

## Files

| upstream path at the pin | local name | sha256 |
|---|---|---|
| `src/core/spu.cpp`      | `spu.cpp`      | `a76fcaa961a73384e6346da98fc073f9f1eb20669b97cc556bd0d70bd99c5a9f` |
| `src/core/spu.h`        | `spu.h`        | `e8d252545f165d4809d314e806c4e3c397cbd7a201b5b97faac9c8a5d6613aa2` |
| `src/common/types.h`    | `types.h`      | `1567fa5655cd1b2a03f023c7dbbc52056dfaf8c33bed7242c4fd6a73802cd151` |
| `src/core/types.h`      | `core_types.h` | `2963f3389c066f0f4f90fb12febe5df1d956fc4753fc4db91facc6737a619108` |
| `src/common/bitfield.h` | `bitfield.h`   | `1b56f86cbc2e38b9e8b2830ef793b3384c9b686f43bc406fa22569e85f50f4a7` |
| `src/common/bitutils.h` | `bitutils.h`   | `8e13065559eac70e5b3567d1b12128b9c21610042d3f251401d1ca287e84a8c8` |

The same list is `pc/3rd/spu-glue/SHA256SUMS` (for `sha256sum -c`). The two
`types.h` are different files; the local names keep them apart.

## Layout

```
pc/3rd/SPU-CORE.md            this document                        (tracked)
pc/3rd/spu-glue/              this project's code                  (tracked)
    fetch.sh                  the procedure below, as a script
    SHA256SUMS                checksums of the six pristine files
    patches/000N-*.patch      the local patches, applied in order
    stubs/                    stub headers + C shim the core compiles against
    Makefile                  builds spu_psx.a from the core + stubs
pc/3rd/spu-core/              the fetched core                     (gitignored)
    pristine/<local name>     the six files, byte for byte
    upstream/<local name>     the same with the patches applied (what is compiled)
    .fetched                  stamp written by fetch.sh after verify + patch
    obj/, spu_psx.a           build output of pc/3rd/spu-glue/Makefile
```

`SPU_CORE_DIR` moves `pc/3rd/spu-core/` elsewhere, for `fetch.sh` and the
Makefiles alike: `SPU_CORE_DIR=/abs/path pc/3rd/spu-glue/fetch.sh`, then
`make -f pc/Makefile SPU_CORE_DIR=/abs/path all`.

## Manual procedure (what `fetch.sh` does)

Every block below starts from the repository root (each `cd` is inside a
`( ... )` subshell), so the blocks can be pasted one after another.

1. **Download the six files** into `pc/3rd/spu-core/pristine/`, renamed per the
   table:

   ```sh
   C=$(sed -n 's/^SPU_CORE_COMMIT=//p' pc/3rd/SPU-CORE.md)
   R=$(sed -n 's/^SPU_CORE_RAW=//p' pc/3rd/SPU-CORE.md)
   rm -rf pc/3rd/spu-core && mkdir -p pc/3rd/spu-core/pristine
   ( cd pc/3rd/spu-core/pristine &&
     curl -fsSLo spu.cpp      "$R/$C/src/core/spu.cpp" &&
     curl -fsSLo spu.h        "$R/$C/src/core/spu.h" &&
     curl -fsSLo types.h      "$R/$C/src/common/types.h" &&
     curl -fsSLo core_types.h "$R/$C/src/core/types.h" &&
     curl -fsSLo bitfield.h   "$R/$C/src/common/bitfield.h" &&
     curl -fsSLo bitutils.h   "$R/$C/src/common/bitutils.h" )
   ```

   Offline, or with a clone you already have: `git -C <clone> show
   $C:src/core/spu.cpp > pc/3rd/spu-core/pristine/spu.cpp`, and so on for
   the table (`fetch.sh --from-clone <clone>` does this). The clone needs that
   commit (`git -C <clone> fetch origin $C` if it is shallow). Make sure the
   files keep LF line endings (`core.autocrlf=false`), or the checksums will
   not match.

2. **Verify** (must print `OK` six times; a mismatch means the wrong commit, a
   truncated download, or converted line endings: do not continue):

   ```sh
   ( cd pc/3rd/spu-core/pristine && sha256sum -c ../../spu-glue/SHA256SUMS )
   ```

3. **Patch**: copy `pristine/` to `upstream/` and apply the series in order.
   Each patch must apply without fuzz or rejects (`patch` prints only
   `patching file upstream/...`):

   ```sh
   ( cd pc/3rd/spu-core && rm -rf upstream && cp -R pristine upstream &&
     for p in ../spu-glue/patches/*.patch; do patch -p1 -i "$p" || exit 1; done )
   ```

4. **Stamp**. The build refuses a core without the stamp, or one older than
   any patch in `pc/3rd/spu-glue/patches/`:

   ```sh
   date > pc/3rd/spu-core/.fetched
   ```

5. **Build and test**:

   ```sh
   make -f pc/Makefile all     # "Built .../pc/3rd/spu-core/spu_psx.a", then LINK
   make -f pc/Makefile test    # every test passes
   ```

   The archive alone: `make -C pc/3rd/spu-glue` (three objects, warning-free).

## Verification

```sh
(cd pc/3rd/spu-core/pristine && sha256sum -c ../../spu-glue/SHA256SUMS)   # 6 x OK
grep -c 'tokimeki-pc patch' pc/3rd/spu-core/upstream/spu.cpp              # 13
make -f pc/Makefile all && make -f pc/Makefile test                       # passes
PC_TESTS_NO_DISC=1 make -f pc/Makefile test                               # without a disc
```

The audio unit tests (`pc/tools/test_audio_quality.py`, `test_music_pack.py`,
`test_spu_mmio.py`, `test_spu_reset.py`, `test_soundfont.py`,
`test_audio_realtime.py`) build against the same fetched core
(`pc/tools/spu_core.py`).

## Local patches

All three are this project's own changes (GPL-3.0, as modifications of the
upstream files). Each is a unified diff against `upstream/<file>`; they apply
on top of each other in order. Hunks in `spu.cpp` that belong to 0002 and 0003
carry a `tokimeki-pc patch` comment.

- **`0001-standalone-library.patch`** (`spu.h`, `spu.cpp`): adds
  `SPU::GenerateSamples(num_frames)`, which runs `SPU::Execute` for exactly
  that many output frames without the CPU tick scheduler, and
  `SPU::SetExternalOutputStream`, which lets the host own the output stream.
  This is what makes the core usable as a library driven by the port's HAL
  (`pc/hal/spu.c`).
- **`0002-music-split.patch`** (music packs, `pc/hal/music_pack.c`): `ds_spu_music_split(on,
  voice_mask, replace)` / `ds_spu_mix_split(out, music, n)`. With the split on,
  the sample loop also sums a *rest* mix of the voices outside the mask (dry and
  reverb input, plus CD input) and runs it through a shadow reverb unit (the
  same `ProcessReverb` with its own resampling buffers, current address and a
  512 KiB RAM copy, synced from the real unit when the split turns on); with
  `replace` the output is the rest mix, and `music` receives full minus rest.
  Every voice is still sampled and the real reverb unit still runs, so SPU
  RAM, ADSR, ENDX and SPUSTAT do not depend on the split. With the split off
  the output is the unpatched computation. Its state is one static,
  `SPU::s_pc_split`, outside `SPUState` (save-state layout unchanged).
- **`0003-sound-quality.patch`** (`pc/hal/spu_quality.c`, `pc/hal/soundfont.c`,
  ENHANCED-MODE.md "Sound quality" / "SoundFont mode"; all off by default):
  interpolation modes 1 (cubic, Catmull-Rom) and 2 (8-tap Kaiser sinc, cutoff
  scaled with pitch) via `PcInterpolate`, with the previous block's last 8
  samples per voice in `s_pc_hist`; a floating-point reverb
  (`PcProcessReverbHQ`) that replaces the reverb in the output while the real
  unit still runs on SPU RAM; `ds_spu_inject(frames)` (SoundFont voices added
  to the split's rest mix); `ds_spu_voice_on_mask()`. With every setting at
  its console value the output, SPU RAM and registers equal the core before
  the patch (`pc/tools/test_audio_quality.py` CoreTests builds both: the
  "before" core is `pristine/` + 0001 + 0002).

To change a patch: edit `pc/3rd/spu-core/upstream/`, then regenerate the
patch from `pristine/` plus the earlier patches (`diff -u` with labels
`a/upstream/<file>` and `b/upstream/<file>`), and re-run `fetch.sh` to check
the series applies from scratch.

## Stubs (this project's code, in `pc/3rd/spu-glue/stubs/`)

The upstream `spu.cpp` is written for a whole emulator. Every coupling point
has a small stand-in so it compiles as a library. None of them allocates,
communicates or does I/O at run time.

- `types.h`, `common/types.h`, `common/bitfield.h`, `common/bitutils.h`:
  redirect the upstream include paths to `upstream/<local name>`;
  `bitutils.h` also brings in the scalar `GSVector4i`.
- `common/gsvector.h`: scalar s32x4 stand-in for the upstream SIMD vector
  type (reverb FIR); bit-exact, not vectorised.
- `common/assert.h`, `common/log.h`, `fmt/format.h`, `host.h`,
  `common/error.h`, `common/path.h`: asserts, logging, formatting, error
  reporting and paths as no-ops.
- `common/fifo_queue.h`: a minimal `InlineFIFOQueue<T,N>` (transfer FIFO).
- `cdrom.h`: `CDROM::GetAudioFrame()` asks the host's CD audio source
  (`spu_core_set_cd_audio_source`, `pc/hal/cd_audio.c`) for one frame;
  silence when none is registered or its queue is empty.
- `dma.h`, `interrupt_controller.h` (forwards to the hook registered with
  `spu_core_set_irq_hook`), `settings.h`, `system.h`, `timing_event.h`
  (callback kept, never scheduled; `InvokeEarly` calls it directly).
- `imgui.h`, `util/imgui_manager.h`, `util/audio_stream.h`,
  `util/media_capture.h`, `util/wav_writer.h`, `util/state_wrapper.h`:
  debug UI, output stream, capture, WAV dump and save-state plumbing reduced to
  what `spu.cpp` needs to compile (save states of the core go through the
  port's own `pc/hal/dbg_state.c`).
- `spu_stubs.cpp`: `g_settings`, the IRQ and CD-audio hook registration.
- `spu_shim.cpp`: the C ABI the port calls (`ds_spu_init`, `ds_spu_reset`,
  `ds_spu_write_reg`, `ds_spu_read_reg`, `ds_spu_dma_write`,
  `ds_spu_dma_read`, `ds_spu_mix`, the split / quality / inject entry points);
  `pc/hal/spu_psx.c` wraps it as `spu_psx_*`.

## Licence

The fetched files are DuckStation's, © 2019-2024 Connor McLaughlin, used under
**GPL-3.0** (`pc/LICENSE`, `pc/licenses/GPL-3.0.txt`). Once you build the port
with them, the resulting **binary is a combined work under GPL-3.0**. The rest
of `pc/` is GPL-3.0-or-later (`LICENSE`, `pc/LICENSE`). The stubs, shim and
patches in `pc/3rd/spu-glue/` are this project's code; the patches, as
modifications of DuckStation's files, are GPL-3.0. A binary release carries
the fetched and patched files in its source archive
(`pc/tools/source_archive.py`; `pc/licenses/THIRD-PARTY.txt`).

## Other builds

- **Windows** (`make -f pc/Makefile PLATFORM=windows all`) and the **old-root
  Linux package** (`linux-package`) compile the same three sources with the
  same include order into their own build directory (`$(BUILD_DIR)/spu/`); they
  need the same fetched core.
- **CI** (`.github/workflows/build.yaml`, PC job) runs `pc/3rd/spu-glue/fetch.sh`
  before building.

## Troubleshooting

- `no SPU_CORE_RAW= line`: run `fetch.sh` from a checkout that has this file.
- `curl: (22) ... 404`: the commit or a path is mistyped; the values above are
  exact.
- `FAILED` from `sha256sum -c`: wrong commit, a proxy rewriting content, or
  CRLF line endings. Delete `pc/3rd/spu-core/` and fetch again.
- `does not apply`: the files in `pristine/` are not the pinned ones, or a
  patch was edited without regenerating it.
- `the SPU core is missing or out of date` after a `git pull`: a patch
  changed; run `fetch.sh` again (it starts from scratch every time).
