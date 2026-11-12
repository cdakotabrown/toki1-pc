# Mednafen PSX GPU rasterizer (optional parity backend)

**This directory is not tracked.** The backend is a verification reference for
`pc/hal/gpu.c`'s rasterizer, not a shipped path, so the 2026-11 public-release
cleanup (plan D7) took it out of the tree and left the interface behind:

- tracked: `shim/mdfn_gpu_shim.h` (the C interface `pc/hal/gpu.c` is written
  against) and this README;
- not tracked: `upstream/` (the vendored rasterizer TUs), `shim/mdfn_gpu_shim.cpp`,
  `shim/beetle_psx_globals.h`, `git.h`, `SHIM-DESIGN.md`.

Without them the port builds and its tests pass: `pc/hal/mdfn_gpu_stub.c`
satisfies the link and `mdfn_gpu_enabled()` is a constant 0, so `MDFN_GPU=1` has
no effect. To get the parity backend back, drop a copy here and build with

    make -f pc/Makefile MDFN_GPU_BACKEND=1

which compiles `shim/mdfn_gpu_shim.cpp` (C++11, scoped include dirs) instead of
the stub, and turns `MDFN_GPU=1` back into a runtime switch. A missing copy is a
build error, not a silent fallback.


Source: `libretro/beetle-psx-libretro@master:mednafen/psx/` — the
Beetle-PSX libretro fork of Mednafen. Files live in `upstream/`
verbatim from that tree, with only trivial diffs needed to compile
standalone (tracked in `shim.cpp`).

License: GNU GPL v2 or later (compatible with this project's GPL-3).

## Files vendored

| File                   | Purpose                           | LOC  |
|------------------------|-----------------------------------|------|
| gpu.h                  | `PS_GPU` struct + API prototypes  |  290 |
| gpu_common.h           | Pixel helpers (PlotPixelBlend...) |  337 |
| gpu_polygon.cpp        | Triangle rasterizer (templated)   |  898 |
| gpu_polygon_sub.cpp    | Polygon command decoders          |  386 |
| gpu_sprite.cpp         | Rect/sprite rasterizer            |  271 |
| gpu_line.cpp           | Line rasterizer                   |  244 |

Total rasterizer code: ~2,400 LOC. The `.cpp` files are designed to be
`#include`d from a single translation unit (they contain `static`
templated functions) — this is how Mednafen's `gpu.cpp` uses them.

## Shim

`mdfn_gpu_shim.cpp` stands up a trimmed `PS_GPU` instance, stubs the
helper functions referenced by the rasterizers, and exposes C entry
points that `pc/hal/gpu.c` can call:

```c
void mdfn_gpu_init(u16 *vram_backing_store);
void mdfn_gpu_shutdown(void);
/* submit one primitive; returns immediately after drawing */
void mdfn_draw_triangle(const MdfnTri *t);
void mdfn_draw_sprite(const MdfnSprite *s);
void mdfn_draw_line(const MdfnLine *l);
/* state */
void mdfn_set_drawing_area(int x0, int y0, int x1, int y1);
void mdfn_set_draw_offset(int x, int y);
void mdfn_set_texpage(u16 packed);
void mdfn_set_clut(u16 packed);
```

## Not vendored

`gpu.cpp` itself (command FIFO + DMA + display timing) is NOT
vendored. Our `pc/hal/gpu.c` already parses PS1 GP0 packets and
splits them into primitives — we bypass Mednafen's command path and
hand parsed primitives directly to the raster functions.

CPU/bus/timer modules from Mednafen are not vendored. The raster
files reference `gpu->DrawTimeAvail` which the shim leaves at a large
constant (no timing simulation on PC).
