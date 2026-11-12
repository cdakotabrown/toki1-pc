# PSY-Z provenance

This directory keeps upstream PSY-Z's `LICENSE` and `README.md` for
attribution. PSY-Z's libraries were never compiled into the PC port; the HAL
implements the PSY-Q API directly. The full source and samples live upstream
at <https://github.com/Xeeynamo/psyz>.

The PSY-Q-style SDK
headers in `pc/include/` (`libgpu.h`, `psyq/libgpu.h`, `common.h`,
`malloc.h`, `setjmp.h`, `psyz.h`, `psyz/*.h`) are taken from PSY-Z's
`psyz/include/` (most of them unchanged, some edited for this port), and
the port compiles against them. `THIRD-PARTY.md` lists them. (PSY-Z's
other SDK headers and its glad OpenGL loader, which no file of the port
includes, are not carried.)
