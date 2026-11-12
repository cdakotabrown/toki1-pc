# Third-party components

This repository (toki1-pc, the native PC port) is GPL-3.0-or-later (`LICENSE`;
`pc/LICENSE` is the same text), and `pc/hal/gpu.c` and `pc/hal/cd_audio.c`
are GPL-3.0-only because they contain DuckStation-derived code. The
decompiled C the port compiles comes from the sibling `toki1-decomp`
checkout, which is MIT and lists its own third-party components; the
optional English patch (`toki1-english`) is MIT likewise. Everything below is
someone else's, and keeps its own terms. This list exists so the boundary is
checkable rather than remembered: a component is either here with a licence,
or it is not in the tree at all.

## The game

**Tokimeki Memorial: Forever with You** (SLPS-00065, Konami, 1995) is © Konami;
"Tokimeki Memorial" is a Konami trademark. The project is not affiliated with
Konami or Sony. This repository holds no disc images and none of the game's
data files, Japanese script text, art, audio or video: the port reads all of
it from the player's own disc at run time (`pc/assets`, `data/` beside a
packaged program; never tracked, never packaged). The English translation
pack a build may embed is a recipe of the project's own bytes, applied to
the player's own files (`pc/hal/translation.c`).

## Native PC port

| Component | Where | Terms | Notes |
|---|---|---|---|
| **SDL2** | statically linked at build time | zlib licence — `pc/licenses/SDL2.txt` | window, input, audio device |
| **DuckStation SPU** (audio core) | **not tracked**: fetched into gitignored `pc/3rd/spu-core/` by `pc/3rd/spu-glue/fetch.sh` (`pc/3rd/SPU-CORE.md`) | **DuckStation**, <https://github.com/stenzek/duckstation>, © 2019-2024 Connor McLaughlin. GPL-3.0, commit `aa955b8ae28314ae061613f0ddf13183a98aca03` (the last revision whose files are dual-licensed GPL-3.0 / CC-BY-NC-ND-4.0; taken under GPL-3.0) — `pc/licenses/GPL-3.0.txt` | `src/core/spu.cpp`, `spu.h` and four support headers; always compiled into the binary. This project's stubs, shim and patches are `pc/3rd/spu-glue/` (the patches are GPL-3.0, as modifications of DuckStation's files) |
| **DuckStation-derived code** | `pc/hal/cd_audio.c` (the 18.9 kHz XA resampler: `k_half` table, `cd_xa_resample`); `pc/hal/gpu.c` (draw-time rules) | DuckStation, © Connor McLaughlin, GPL-3.0 (resampler from rev `ee538ae0e`; draw-time rules at commit `aa955b8ae`). Both files are GPL-3.0-only and say so in their headers | ported into the port's own HAL. `pc/hal/cd.c` (CD drive seek-timing structure) and `pc/hal/mdec_native.c` (MDEC fixed-point model) also credit DuckStation's research; their code is this project's own |
| **Mednafen-derived code** | `pc/hal/gpu.c`: the polygon rasteriser (`raster_xfp`, `raster_xfp_step`, `raster_xfp_int`, `raster_delta`, `raster_span`, `raster_span_hires`, `raster_triangle`), the texture/CLUT cache, the size limits; the 18.9 kHz interpolation weights in `pc/hal/cd_audio.c` | Mednafen PSX GPU (`gpu_polygon.c`, `gpu_common.h`), © Mednafen Team, **GPL-2.0-or-later**, used under GPL-3.0 | credited in the files' headers |
| **Mednafen GPU** (parity backend) | `pc/3rd/mednafen-gpu/` — **not tracked**; opt-in `MDFN_GPU_BACKEND=1` | Mednafen, © Mednafen Team, GPL-2.0 or later, used under GPL-3.0 | excluded from the tree by the cleanup (D7) because it is a parity reference, not a shipped path; `pc/3rd/mednafen-gpu/README.md` explains how to drop your own copy in |
| **TinySoundFont** | `pc/3rd/tsf.h` | MIT — `pc/licenses/TinySoundFont.txt` | soundfont synth for SEQ music |
| **stb_image / stb_image_write / stb_vorbis** | `pc/3rd/stb/`, `pc/3rd/audio-decoders/stb_vorbis.c` | Public domain or MIT — `pc/licenses/stb.txt` | image and Vorbis decoding |
| **dr_flac / dr_wav** | `pc/3rd/audio-decoders/` | Public domain or MIT-0 — `pc/licenses/dr_libs.txt` | audio decoding |
| **PSY-Z** (PS1→PC porting SDK) | `pc/3rd/psyz/`: attribution files only (`LICENSE`, `README.md`, `VENDORED.md`), no PSY-Z library code | per directory, see PSY-Z's `LICENSE` (`pc/3rd/psyz/LICENSE`) | the port does not link PSY-Z's libraries. **But** the PSY-Q-style SDK headers in `pc/include/` (`libgpu.h`, `common.h`, `malloc.h`, `setjmp.h`, `psyz.h`, `psyz/*.h`) come from PSY-Z's `psyz/include` (`pc/3rd/psyz/VENDORED.md`) |
| **MinGW-w64 runtime** | Windows build only | GPL-3.0 with the GCC runtime exception; winpthreads MIT/BSD-3 | statically linked into `tokimeki-pc.exe`; `pc/licenses/MinGW-runtime.txt` |
| **rsx_intf.h shim** | `pc/3rd/rsx/` | Project-written stub | satisfies the Mednafen include path when the opt-in backend is enabled |
| **SHA-1 / SHA-256** | `pc/hal/pc_hash.c`, `pc_hash.h` | written for this port; public domain | |

The port's sources are GPL-3.0-or-later, and the executable you build links the
fetched GPL-3.0 DuckStation SPU core, so **the binary is GPL-3.0 as a combined
work**. The program says so in `--licenses` and in its About page;
`pc/licenses/THIRD-PARTY.txt` is the copy that ships inside the package. Each
player package is written with a matching source archive next to it
(`<package>-source.tar.gz`: the committed trees that were built -- toki1-pc,
toki1-decomp and, when used, toki1-english -- plus the fetched and patched SPU
core; `pc/tools/source_archive.py`), which is the corresponding
source GPLv3 section 6 asks for.

## Everything else

- `pc/font/latin12.txt`: the English patch's hand-authored 12-px Latin font
  (toki1-english `tools/font/latin12.txt`, MIT, this project's own
  work), kept here as an exact copy for the PC layer's UI text.
- `pc/assets`, `pc/assets-en`, `disks/` (in toki1-decomp), `pc/3rd/spu-core/`,
  `pc/3rd/mednafen-gpu/upstream/`: never tracked. See `.gitignore`.

If you find a third-party file that is not on this list, that is a bug: add it with
its terms or take it out.
