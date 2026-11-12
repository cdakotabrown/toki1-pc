# SPDX-License-Identifier: GPL-3.0-or-later
# Tokimeki Memorial: PC Port Build System
#
# Prerequisites:
#   - SDL2 development libraries (libsdl2-dev)
#   - OpenGL development libraries (libgl-dev)
#   - Game assets extracted: python3 pc/tools/extract_assets.py /path/to/disc.bin pc/assets/
#
# Usage:
#   make              # Build the game
#   make run          # Build and run
#   make clean        # Remove build artifacts
#   make assets       # Extract assets from disc (requires DISC_PATH)

CC = gcc
CXX = g++
.DEFAULT_GOAL := all

# ---- the other parts of the project (docs/REPO-SPLIT-PLAN.md) --------------
# The port compiles the decompiled C of toki1-decomp (src/main, src/overlay,
# include/, config/) and, when it is there, the English patch of
# toki1-english (src/patch, tools/patch, config/patch).  The same rule as
# tools/repo_paths.py: the environment variable TOKI_DECOMP_DIR /
# TOKI_ENGLISH_DIR, else this checkout when it holds that part (the
# monorepo: all three parts), else the sibling ../toki1-decomp /
# ../toki1-english.  D and E are the prefixes every path into those parts
# carries: empty when the part is this checkout, so the monorepo's paths
# and compile lines are the ones it always had.  Build output stays here.
NO_PART_GOALS = clean print-objs print-excludes
ifneq ($(TOKI_DECOMP_DIR),)
  DECOMP_DIR := $(realpath $(TOKI_DECOMP_DIR))
  ifeq ($(wildcard $(DECOMP_DIR)/src/main/matched),)
    $(error TOKI_DECOMP_DIR=$(TOKI_DECOMP_DIR) does not hold src/main/matched)
  endif
else ifneq ($(wildcard src/main/matched),)
  DECOMP_DIR := $(CURDIR)
else ifneq ($(wildcard ../toki1-decomp/src/main/matched),)
  DECOMP_DIR := $(realpath ../toki1-decomp)
else ifneq ($(filter-out $(NO_PART_GOALS),$(or $(MAKECMDGOALS),all)),)
  $(error toki1-decomp not found: check it out next to this repository     ($(abspath ../toki1-decomp)) or set TOKI_DECOMP_DIR)
endif
# (the English part: config/patch in the monorepo, sheets/ in the flat
# public toki1-english)
ifneq ($(TOKI_ENGLISH_DIR),)
  ENGLISH_DIR := $(realpath $(TOKI_ENGLISH_DIR))
  ifeq ($(wildcard $(ENGLISH_DIR)/config/patch $(ENGLISH_DIR)/sheets),)
    $(error TOKI_ENGLISH_DIR=$(TOKI_ENGLISH_DIR) does not hold config/patch or sheets)
  endif
else ifneq ($(wildcard config/patch),)
  ENGLISH_DIR := $(CURDIR)
else ifneq ($(wildcard ../toki1-english/config/patch ../toki1-english/sheets),)
  ENGLISH_DIR := $(realpath ../toki1-english)
endif
D := $(if $(filter $(CURDIR),$(DECOMP_DIR)),,$(DECOMP_DIR)/)
E := $(if $(filter $(CURDIR),$(ENGLISH_DIR)),,$(ENGLISH_DIR)/)
# The English part's directories as its checkout spells them: the monorepo
# keeps src/patch/main, tools/patch, config/patch; the public toki1-english
# is flat (patch/, tools/, config/).  tools/repo_paths.py ENGLISH_LAYOUT is
# the one table; E_SRC, E_TOOLS, E_CONFIG carry the E prefix.
ifneq ($(ENGLISH_DIR),)
  E_DIRS := $(shell TOKI_ENGLISH_DIR='$(ENGLISH_DIR)' TOKI_DECOMP_DIR='$(DECOMP_DIR)' \
              python3 tools/repo_paths.py --english src/patch/main tools/patch config/patch 2>/dev/null)
  ifneq ($(words $(E_DIRS)),3)
    $(error tools/repo_paths.py --english failed for $(ENGLISH_DIR))
  endif
  E_SRC    := $(E)$(word 1,$(E_DIRS))
  E_TOOLS  := $(E)$(word 2,$(E_DIRS))
  E_CONFIG := $(E)$(word 3,$(E_DIRS))
endif
# 1 when the English patch is present: its engine is compiled in and the
# translation pack embedded.  Without it the port is the Japanese game
# (the language setting falls back to ja; pc/hal/lang.c).
HAVE_ENGLISH := $(if $(ENGLISH_DIR),1,)
export TOKI_DECOMP_DIR := $(DECOMP_DIR)
ifneq ($(ENGLISH_DIR),)
export TOKI_ENGLISH_DIR := $(ENGLISH_DIR)
endif
# A sibling checkout is read, never written (docs/REPO-SPLIT-PLAN.md rule
# 4): the Python this build runs from it (or that imports its modules)
# writes no __pycache__ there.
ifneq ($(D)$(E),)
export PYTHONDONTWRITEBYTECODE := 1
endif
.PHONY: print-parts
print-parts:
	@echo "DECOMP_DIR=$(DECOMP_DIR)"
	@echo "ENGLISH_DIR=$(ENGLISH_DIR)"

# ---- target platform -------------------------------------------------------
# PLATFORM=linux (default) builds pc/build/tokimeki-pc with the host gcc.
# Parallel by default: with no -j on the command line, use every core.
ifeq ($(filter -j%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(shell nproc 2>/dev/null || echo 4)
endif
# PLATFORM=windows cross-compiles pc/build-win/tokimeki-pc.exe with MinGW-w64
# (`make -f pc/Makefile windows` is the shorthand).  Toolchain, SDL2 and the
# portability notes: docs/pc-port/WINDOWS-BUILD.md.
PLATFORM ?= linux
ifeq ($(PLATFORM),windows)
  # The posix-threads flavour of MinGW-w64 gcc (winpthreads: the HAL's
  # pthread/clock_gettime calls).  A system install (apt: gcc-mingw-w64-
  # x86-64-posix) is on PATH; pc/tools/setup_windows_toolchain.sh unpacks
  # the same Ubuntu packages without root into build/mingw-root.
  MINGW_ROOT ?= $(firstword $(wildcard build/mingw-root/usr))
  MINGW_BIN  ?= $(if $(shell command -v x86_64-w64-mingw32-gcc-posix 2>/dev/null),,$(if $(MINGW_ROOT),$(MINGW_ROOT)/bin/))
  CC  = $(MINGW_BIN)x86_64-w64-mingw32-gcc-posix
  CXX = $(MINGW_BIN)x86_64-w64-mingw32-g++-posix
  WIN_AR = $(MINGW_BIN)x86_64-w64-mingw32-ar
  # The official SDL2 MinGW development release (SDL2-devel-<v>-mingw.tar.gz,
  # unpacked by pc/tools/setup_windows_toolchain.sh; never committed).
  SDL2_WIN ?= $(firstword $(wildcard build/sdl2-mingw/SDL2-*/x86_64-w64-mingw32))
endif
# Note: globals_data.h is NOT force-included — it causes macro conflicts
# with extern declarations in game code. Instead, globals resolve at link
# time against globals.c (and the generated globals, whose retail
# initial values come from PSX.EXE at run time: pc/src/retail_init.c).
CFLAGS = -std=gnu17 -fpermissive -w -funsigned-char -fno-strict-aliasing -fno-aggressive-loop-optimizations -O2 -D_GNU_SOURCE -DPC_PORT \
         -include pc/include/game/types.h \
         -I pc/include/game -I pc/include -I $(D)include

# Allow callers to inject extra flags: `WATCHDOG_SEC=120 make ...`
ifdef WATCHDOG_SEC
  CFLAGS += -DWATCHDOG_SEC=$(WATCHDOG_SEC)
endif

# Per analyst V3 D1: TRACE=1 enables -finstrument-functions for per-fn entry/
# exit logging via pc/hal/instrument.c. Combine with ASAN=1 only when latent
# undef-refs are fixed; ASan instrumentation can unmask refs the optimizer
# was previously eliminating.
ifeq ($(TRACE),1)
  CFLAGS  += -g3 -fno-omit-frame-pointer -finstrument-functions \
             -finstrument-functions-exclude-file-list=pc/3rd,pc/include,pc/hal \
             -finstrument-functions-exclude-function-list=vram_area_clear,ClearImage,DrawSync,LoadImage,PutDispEnv,PutDrawEnv,DrawOTag,GetTPage,GetClut \
             -DWATCHDOG_SEC=60
  LDFLAGS_SAN = -rdynamic
endif
ifeq ($(ASAN),1)
  CFLAGS  += -fsanitize=address,undefined -g3 -fno-omit-frame-pointer
  LDFLAGS_SAN = -fsanitize=address,undefined
endif
# Mednafen GPU shim (C++, scoped include dirs — does NOT share CFLAGS).
MDFN_DIR = pc/3rd/mednafen-gpu
MDFN_CXXFLAGS = -w -std=c++11 -O2 -fno-strict-aliasing -D_GNU_SOURCE \
                -I $(MDFN_DIR) -iquote $(MDFN_DIR)/shim
ifeq ($(PLATFORM),windows)
# SDL_MAIN_HANDLED: pc/src/main.c is the entry point (no SDL2main).
# __USE_MINGW_ANSI_STDIO: C99 printf/scanf (%zu, %lld, %g formatting the
# same as glibc), so traces and logs are byte-comparable with Linux.
# _POSIX_THREAD_SAFE_FUNCTIONS: MinGW's localtime_r/gmtime_r.
# -ffunction-sections -fdata-sections: every global in a section of its own,
# so pc/tools/pe_rebind_relocs.py can give each object ELF's binding of its
# references to its own globals before the link (WINDOWS-BUILD.md "Symbol
# binding": the COFF assembler binds them to the object's private copy).
# pc/include/win holds the POSIX compatibility headers (dlfcn.h,
# execinfo.h, sys/stat.h, windows.h) over pc/hal/win/*.c.
SDL_CFLAGS = -I $(SDL2_WIN)/include
# One self-contained exe (docs/pc-port/WINDOWS-BUILD.md "Single executable"):
# SDL2 is linked statically from the release's libSDL2.a, with the system
# libraries its sdl2.pc lists (all part of every Windows 10/11 install;
# pc/tools/test_windows_imports.py checks the import table).
# WIN_SDL=dll keeps the old SDL2.dll build (the import test's negative control).
WIN_SDL ?= static
ifeq ($(WIN_SDL),dll)
SDL_LIBS = -L $(SDL2_WIN)/lib -lSDL2
else
SDL_LIBS = $(SDL2_WIN)/lib/libSDL2.a -ldinput8 -ldxguid -ldxerr8 -luser32 -lgdi32 -lwinmm \
           -limm32 -lole32 -loleaut32 -lshell32 -lsetupapi -lversion -luuid -lcfgmgr32
endif
SDL_LIBS += -lcomdlg32
CFLAGS += -DSDL_MAIN_HANDLED -D__USE_MINGW_ANSI_STDIO=1 -D_POSIX_THREAD_SAFE_FUNCTIONS \
          -ffunction-sections -fdata-sections -I pc/include/win \
          -include pc/include/win/pc_win_compat.h
MDFN_CXXFLAGS += -D__USE_MINGW_ANSI_STDIO=1
else ifeq ($(LINUX_PKG),1)
# The Linux player package (`make -f pc/Makefile linux-package`;
# docs/pc-port/LINUX-PACKAGE.md): linked against glibc 2.31 so it runs on
# Ubuntu 20.04 / Debian 11 and everything newer.  The pinned Ubuntu 20.04
# root of pc/tools/setup_linux_pkg_root.sh (gcc 9.4, glibc 2.31) is
#   - the C compiler's system headers: the C is compiled by the HOST gcc,
#     the dev build's own compiler, with --sysroot=<root>.  The same
#     compiler matters: parity's CPU-cost model charges retail time at
#     every instrumented function entry, so a compiler that inlines
#     differently shifts the emulated timing (gcc 9 boots two frames
#     early);
#   - the C++ compiler (the SPU core and the Mednafen GPU shim) and the
#     linker, run inside the root through pc/tools/linux_pkg_exec.sh: its
#     crt files, libc, ld 2.34, and gcc 9's static libstdc++ / libgcc,
#     whose glibc symbol versions are all <= 2.31 (gcc 15's are not).
# make and the Python generators stay on the host.  SDL2 is the static one
# built in that root (SDL loads X11 / Wayland / PulseAudio / ALSA itself
# with dlopen), so the binary needs glibc only; portable by default
# (pc_data_root.h).  LINUX_PKG_ROOT=host builds with the host's libraries
# and build/sdl2-linux instead (pc/tools/setup_linux_sdl.sh): the same
# layout, but tied to the host's glibc -- the glibc gate's negative
# control.
LINUX_PKG_ROOT ?= root
ifeq ($(LINUX_PKG_ROOT),host)
SDL2_LINUX ?= build/sdl2-linux
else
LINUX_PKG_ROOTFS ?= $(CURDIR)/build/linux-pkg-root
CC  = gcc --sysroot=$(LINUX_PKG_ROOTFS)
LINK_CC = sh pc/tools/linux_pkg_exec.sh gcc
CXX = sh pc/tools/linux_pkg_exec.sh g++
PKG_AR = sh pc/tools/linux_pkg_exec.sh ar
SDL2_LINUX ?= build/sdl2-linux-glibc231
endif
SDL_CFLAGS = -I $(SDL2_LINUX)/include -I $(SDL2_LINUX)/include/SDL2 -D_REENTRANT
SDL_LIBS = $(SDL2_LINUX)/lib/libSDL2.a -pthread -ldl
CFLAGS += -DPC_PORTABLE_DEFAULT=1
else
SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS = $(shell pkg-config --libs sdl2)
endif
# The SPU core is not in the repository: pc/3rd/spu-glue/fetch.sh puts the
# upstream sources in SPU_CORE_DIR (gitignored; pc/3rd/SPU-CORE.md), and
# pc/3rd/spu-glue/Makefile builds them with this project's stubs and shim.
SPU_CORE_DIR ?= pc/3rd/spu-core
SPU_GLUE_DIR = pc/3rd/spu-glue
SPU_PSX_LIB = $(SPU_CORE_DIR)/spu_psx.a
ifeq ($(PLATFORM),windows)
# The Windows build compiles the core with MinGW into its own build directory.
SPU_PSX_LIB = pc/build-win/spu/spu_psx.a
endif
ifneq ($(PKG_AR),)
# ... and so does the old-root Linux package (its own, older libstdc++).
SPU_PSX_LIB = $(BUILD_DIR)/spu/spu_psx.a
endif
# The globals model: data symbols inside the mapped overlay window resolve to
# their PS1 addresses so the code reads the real EXN bytes (the model that made
# OLH scene 58's end-sentinel work).  pc/tools/gen_psx_globals.py generates it.
ifeq ($(PLATFORM),windows)
# PE layout (docs/pc-port/WINDOWS-BUILD.md "Address space"): a fixed image
# base below 2 GB with no ASLR (no dynamicbase, no high-entropy VA), so code,
# data, the 64 MB main-thread stack and the heap sit where an s32 holds them,
# and the PS1 RAM at 0x80000000 is within rel32 reach of the code, as with
# Linux -no-pie.  dladdr/dlsym (-rdynamic on Linux) read the exe's own COFF
# symbol table (pc/hal/win/win_dl.c), so the exe is never stripped; its
# .file records stay too (the save states tell each static's source file by
# them, pc/hal/win/win_dbg_platform.c), and the link step removes only the
# .debug_* sections the static libraries bring.  --build-id writes the
# CodeView signature a save state is checked against (as GNU build-id on
# Linux).  ws2_32: the debug console's Winsock transport.
# malloc/calloc/realloc are wrapped to verify that placement (win_heap.c).
# -mwindows: a GUI program -- no console window behind the game; the
# player's log is tokimeki-pc.log (pc/include/pc_data_root.h), and the
# console modes (--check, --licenses) attach to the calling console.
# WIN_SUBSYSTEM=console restores the console program.
WIN_SUBSYSTEM ?= windows
LDFLAGS = $(SDL_LIBS) $(SPU_PSX_LIB) -static-libgcc -static-libstdc++ \
          -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic -lm -lws2_32 -m$(WIN_SUBSYSTEM) \
          -Wl,--build-id \
          -Wl,--image-base=0x400000 -Wl,--disable-dynamicbase \
          -Wl,--disable-high-entropy-va -Wl,--stack,0x4000000 \
          -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc \
          -Wl,--allow-multiple-definition -Wl,--just-symbols=$(CURDIR)/pc/ld/psx_globals.syms
else ifeq ($(LINUX_PKG),1)
# glibc stays shared (SDL dlopens the desktop's X11/Wayland/audio libraries).
# --no-relax: binutils 2.34 (the old root's) stops with "failed to convert
# GOTPCREL relocation" for a GOT load of a symbol at a PS1 address
# (psx_globals.syms, 0x80xxxxxx: out of rel32 reach); newer ld quietly keeps
# that load, which is what --no-relax asks for.
LDFLAGS = $(SDL_LIBS) $(SPU_PSX_LIB) -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic -static-libgcc -lm -ldl -no-pie -rdynamic -Wl,--no-relax -Wl,--allow-multiple-definition -Wl,--just-symbols=$(CURDIR)/pc/ld/psx_globals.syms
else
LDFLAGS = $(SDL_LIBS) $(SPU_PSX_LIB) -lstdc++ -lm -ldl -no-pie -rdynamic -Wl,--allow-multiple-definition -Wl,--just-symbols=$(CURDIR)/pc/ld/psx_globals.syms
endif
# Argument-fold wrappers (src/main/pc-port/prim_arg_fold.c): the matched
# bodies run unchanged; __wrap_ shims fold sign-extended PS1 pointer args
# with hal_ps1_ptr and tail-call __real_.  New victim of the s32-pointer
# class: add its name here and a shim there.
WRAP_SYMS = SetLineF2 SetLineF3 SetLineF4 SetLineG2 SetLineG3 SetLineG4 \
            SetPolyF3 SetPolyF4 SetPolyFT3 SetPolyFT4 SetPolyG3 SetPolyG4 \
            SetPolyGT3 SetPolyGT4 SetSprt SetSprt8 SetSprt16 \
            SetTile SetTile1 SetTile8 SetTile16 AddPrim AddPrims SetSemiTrans SetShadeTex
LDFLAGS += $(foreach s,$(WRAP_SYMS),-Wl,--wrap=$(s))
# Typed alias views of globals (now_year_v, cw96_active_a, girl0_flag_b, ...)
# are the SAME object as D_<ADDR>: pc/tools/gen_missing_globals.py writes them
# as linker-script PROVIDE()s, so they alias instead of becoming independent
# variables.  Passed as an implicit linker script (plain input file).
ALIAS_LD = $(wildcard $(PCSRC_DIR)/*.ld)   # missing_aliases.ld (typed views) + layout_aliases.ld (retail offsets inside screen_w/class_w/...)

BUILD_DIR = pc/build
TARGET_NAME = tokimeki-pc
ifeq ($(PLATFORM),windows)
BUILD_DIR = pc/build-win
TARGET_NAME = tokimeki-pc.exe
endif
ifeq ($(LINUX_PKG),1)
BUILD_DIR = pc/build-linux-pkg
ifeq ($(LINUX_PKG_ROOT),host)
BUILD_DIR = pc/build-linux-pkg-host
endif
endif
OBJ_DIR = $(BUILD_DIR)/obj
TARGET = $(BUILD_DIR)/$(TARGET_NAME)

# Source directories (priority order — pc-port overrides matched/semi-matched)
PCPORT_DIR = src/main/pc-port
MATCHED_DIR = $(D)src/main/matched
SEMI_DIR = $(D)src/main/semi-matched
OVERLAY_DIRS = $(wildcard $(D)src/overlay/*)
HAL_DIR = pc/hal
PCSRC_DIR = pc/src

# Collect all source files with override priority
# EXCLUDE every pc-port file named for an overlay-window address
# (func_8013xxxx..func_801Fxxxx): overlay code is the native module of the
# EXN that is loaded (pc/src/overlay_<OV>.c), reached through the generated
# trampolines (pc/src/overlay_symtab.c).  One address names a different
# function in each overlay, so a single override (363 of these were `{ }`
# stubs) was wrong for all but one EXN -- and, preceding the trampolines on
# the link line, it swallowed the call: NAME_ENT's intro never ran
# name_ent_intro_step (func_80144978).
PCPORT_SRCS_ALL = $(wildcard $(PCPORT_DIR)/*.c)
PCPORT_SRCS = $(filter-out $(wildcard $(PCPORT_DIR)/func_801[3-9A-Fa-f]*.c), $(PCPORT_SRCS_ALL))
PCPORT_NAMES = $(notdir $(basename $(PCPORT_SRCS)))

# Files excluded because HAL replaces them (they access PS1 hardware registers)
# Sound HAL stubs:
# 2026-10-30: libsnd's VAB transfer, sequence API and sequencer tick are the
# matched libsnd bodies (SsVabTransBody/Completed, SsSeqOpen/Play/Stop/Pause/
# Replay/SetVol, SsSeqCalledTbyT, SsSetTickMode, SsSetTableSize, SsEnd).  They
# reach the device only through SpuWrite/SpuIsTransferCompleted (timed DMA,
# pc/hal/spu_transfer.c) and the SPU register block, which pc/hal/spu.c
# forwards to the SPU core (spu_mmio_sync).  The tick is started by the HAL
# _SsStart (a PS1 code address cannot be a native VSync callback).
HAL_EXCLUDES = SpuQuit SsVabOpenHead \
               SpuInit SpuClearReverbWorkArea \
# PS1 entry point (src/main/matched/main.c, landed 2026-10): pc/src/main.c is the
# PC entry point and calls _main() itself.  Same basename, so main.o also has
# an explicit rule below (VPATH would otherwise hand the pattern rule the
# matched file).
HAL_EXCLUDES += main
# Sony names landed on matched files that the HAL implements (2026-10-15 audit:
# a matched object precedes pc/hal on the link line, so without this line the
# matched C silently won over the HAL replacement).
# 2026-10-19: GsSetWorkBase removed -- the HAL's empty stub shadowed the
# matched one-store setter and GsOUT_PACKET_P never initialised (all the
# libgs sorters allocated prims at NULL).  Phantom-exclude class.
# Display setup uses matched C: the old HAL relocated VRAM and mistook
# the GTE OFX/OFY setter for a display-scroll routine.
HAL_EXCLUDES += CdRead GsInitVcount IsIdMatrix PadInit PadRead \
                SpuIsTransferCompleted SpuSetTransferMode SsStart2 \
                ResetGraph SpuSetTransferStartAddr \
                SpuWrite SsSetMVol PushMatrix VSync
# libstr / libpress: pc/hal/libpress.c implements the streaming + MDEC API over
# the .STR file and the native decoder (FMV exception; see that file's header).
HAL_EXCLUDES += DecDCTin DecDCTout DecDCTReset DecDCToutCallback DecDCTinCallback DecDCTvlc \
                StSetRing StSetStream StSetMask StSetChannel StGetNext StFreeRing StUnSetRing \
                StCdInterrupt CdRead2
# CD HAL:
HAL_EXCLUDES += CdControlB CdControlF CdSync CdMix CD_init \
                CdReadyCallback CdSyncCallback \
                CdSearchFile CdReadSync CdFlush CdSetDebug CdDiskReady CdGetSector \
                CdReadCallback CdDataCallback CdPosToInt CdIntToPos CdGetToc CdPlay \
# libcd internals behind that API (CD_cw writes the controller registers,
# getintr/callback are its interrupt handler): the HAL's model replaces them.
HAL_EXCLUDES += CD_cw CD_sync CD_readm CD_readsync CD_datasync cd_read_retry getintr callback \
                cb_read cd_init_retry CD_getsector CD_ready CdReady CD_shell CD_vol
# GPU HAL:
HAL_EXCLUDES += SetDefDrawEnv SetDefDispEnv PutDrawEnv PutDispEnv SetDispMask \
                DrawSync DrawSyncCallback ClearImage LoadImage StoreImage MoveImage \
                ClearOTag ClearOTagR DrawOTag GetTPage GetClut SetDrawMode \
# Sound HAL conflicts:
# libetc/libgs timing on the system clock (pc/hal/system.c):
HAL_EXCLUDES += GsGetVcount GsClearVcount StartRCnt StopRCnt ResetRCnt
HAL_EXCLUDES += SetIntrMask GetIntrMask
HAL_EXCLUDES += ResetCallback StopCallback InterruptCallback VSyncCallback
# 2026-10-31: SsUtSetReverbType / SsUtReverbOn / SsUtReverbOff are the
# matched libsnd bodies (SpuSetReverb / SpuSetReverbModeParam / _spu_ioctl /
# _spu_setReverbAttr write the reverb registers; pc/hal/spu.c forwards them).
HAL_EXCLUDES += SsStart _SsStart
# 2026-10-31: SsSetSerialAttr / SsSetSerialVol are the matched bodies
# (_spu_ioctl 256 -> _spu_setCommonAttr: SPUCNT bit 0 CD audio enable, CD
# volume 0x1F801DB0/2); the HAL's no-op stubs kept the XA voice out of the SPU.
# Other conflicts (HAL vs matched):
# (2026-10-15: SsInit, cmd_voice5-8, system_moji_set, week_day_init0,
#  func_8008FBD8/800984C4/800A03BC/800A0488 were listed here although nothing
#  in pc/ defines them -- excluding them only made the symbol undefined.
#  Audit: python3 pc/tools/audit_pc_link.py, and the HAL_EXCLUDES check in
#  its docstring.)
# 2026-10-19: SpuVmInit removed from this line -- nothing in pc/ defined it
# (the only provider was the mislabeled guess in wait_sub_sub.c, now deleted),
# so the exclude only kept the matched src/main/matched/SpuVmInit.c out and
# SD_Init crashed in the guess.  Same phantom-exclude class as the 2026-10-15
# audit found below.
# 2026-10-31: the rest of this line went -- cursor_select and hizuke_advance
# (nothing in pc/ implemented them: the exclude only dropped the matched C
# and `return 0` stubs answered), and eight phantom names (change_yoshio,
# func_80044F8C, func_80048858, func_80076544, func_80086D60, func_8008C810,
# func_8008EF3C, func_80092FBC) that name no matched/semi/archive file.
# Audit: python3 pc/tools/audit_pc_stubs.py, audit_pc_link.py --hal-excludes.
HAL_EXCLUDES += CdControl

MATCHED_SRCS = $(filter-out $(addprefix $(MATCHED_DIR)/,$(addsuffix .c,$(PCPORT_NAMES))) \
                            $(addprefix $(MATCHED_DIR)/,$(addsuffix .c,$(HAL_EXCLUDES))), \
                $(wildcard $(MATCHED_DIR)/*.c))
# Demoted-from-matched files whose symbols the HAL provides (2026-08-01
# fabrication demotion): must stay out of the build from the semi tier too.
SEMI_HAL_EXCLUDES = SsEnd
# Sony LIBGTE name recovered 2026-08-23 (config/psyq_lib_symbols.json).
# semi-matched/RotMatrix.c still carries a pre-identification placeholder body
# (it is an open C candidate, not a blocker); pc/hal/gte.c has the real one.
SEMI_HAL_EXCLUDES += RotMatrix
# Comment-only ASM-ONLY markers (no body): their faithful C lives in the
# archive tier, which a same-named semi file would otherwise shadow.
SEMI_MARKERS = _mk_xpndsp GsMapModelingData
# HAL_EXCLUDES applies to this tier as well (2026-10-15: it did not, so a
# semi-matched SsSeqStop.c / PushMatrix.c reached the link and, preceding
# pc/hal, beat the HAL replacement that the matched exclusion was meant to let win).
# Address-named semi files whose address has a landed name with matched C
# (the naming passes moved the function to that name; the semi copy is a stale
# no-op or partial body that the address spelling still reached --
# scene_entry_set, a no-op, shadowed scene_entry_set for every caller that
# spells the address, so the overlay loader never left step 2).  The address
# spelling gets a trampoline to the matched name (gen_missing_funcs.py).
# Recompute: python3 pc/tools/find_shadowed_tiers.py
# 2026-10-19: the list used to carry the LANDED names (semi/scene_entry_set.c
# and friends -- files that do not exist), so the filter was a no-op and the
# address-named stale bodies kept shadowing.  It now carries the FILE
# basenames find_shadowed_tiers.py reports, mapped to the landed name each
# one shadows:  func_80042D48->pad_type_check, func_80053364->grp_table_calc,
# func_8005C3C4->save_format_main, func_800766C0->machiawase_init,
# func_800790FC->scene_entry_set (the overlay loader never left step 2),
# func_8007A110->SD_CmdDispatch, func_8007AC70->seq_vol_fade,
# func_8007E778->fade_down, func_80083D2C->girl_season_load,
# func_8008FA10->SsUtSetReverbType.  With the stale bodies gone the address
# spellings fall to gen_missing_funcs.py's tail-call trampolines.
SEMI_SHADOWED = func_80042D48 func_80053364 func_8005C3C4 func_800766C0 \
                func_800790FC func_8007A110 func_8007AC70 func_8007E778 \
                func_80083D2C func_8008FA10
# Stale lower-tier bodies nothing references (2026-10-31): each one's
# recorded address lies inside the window of a registered matched function
# under another name, and no source, overlay module or trampoline names it
# (the link proves it).  They compiled in as dead code whose calls pass
# fewer arguments than the callee reads (audit_ps1_call_args.py SHORT-READ):
#   semi func_80067490 -- a loop-exit fragment of update_anim_state (0x80066D38)
#   semi func_80079DCC -- an alt entry inside SD_VabLoad (0x80079DB8)
#   semi func_800926F0 -- a phantom mid-function arm of Snd_decrescendo (0x80092170)
#   archive face_change (0x80091A20) -- inside Snd_pause (0x80091A10)
#   archive load_vram (0x80066D38) -- the old name of update_anim_state
#   archive system_init (0x8004C50C) -- the old name of col2sepia
SEMI_STALE = func_80067490 func_80079DCC func_800926F0
ARCHIVE_STALE = face_change load_vram system_init
SEMI_SRCS = $(filter-out $(addprefix $(SEMI_DIR)/,$(addsuffix .c,$(PCPORT_NAMES))) \
                         $(addprefix $(SEMI_DIR)/,$(addsuffix .c,$(HAL_EXCLUDES) $(SEMI_HAL_EXCLUDES) $(SEMI_MARKERS) $(SEMI_SHADOWED) $(SEMI_STALE))), \
             $(wildcard $(SEMI_DIR)/*.c))

# Remove semi-matched if pc-port or matched version exists
MATCHED_NAMES = $(notdir $(basename $(MATCHED_SRCS)))
SEMI_SRCS := $(filter-out $(addprefix $(SEMI_DIR)/,$(addsuffix .c,$(MATCHED_NAMES))), $(SEMI_SRCS))

# Archive tier (lowest priority): C demoted from matched/ by the 2026-08
# custom_script fabrication audits. The C is semantically faithful — it was
# demoted because its *byte match* relied on banned PostProc, which is
# irrelevant to a native build. Used only when no pc-port/matched/semi
# body defines the symbol.
ARCHIVE_DIR = $(D)src/main/archive
ARCHIVE_SRCS_ALL = $(wildcard $(ARCHIVE_DIR)/*_painted.c)
ARCHIVE_NAMES_ALL = $(patsubst %_painted,%,$(notdir $(basename $(ARCHIVE_SRCS_ALL))))
SEMI_NAMES = $(notdir $(basename $(SEMI_SRCS)))
ARCHIVE_NAMES = $(filter-out $(PCPORT_NAMES) $(MATCHED_NAMES) $(SEMI_NAMES) $(HAL_EXCLUDES) $(SEMI_HAL_EXCLUDES) $(ARCHIVE_STALE),$(ARCHIVE_NAMES_ALL))
ARCHIVE_SRCS = $(addprefix $(ARCHIVE_DIR)/,$(addsuffix _painted.c,$(ARCHIVE_NAMES)))

# HAL and PC-specific sources
HAL_SRCS = $(filter-out $(HAL_DIR)/mdfn_gpu_stub.c,$(wildcard $(HAL_DIR)/*.c))
ifeq ($(PLATFORM),windows)
# The debug toolkit and the save states (docs/pc-port/DEBUG-TOOLS.md) are
# portable C over a small host layer (pc/hal/dbg_platform.h): Linux's is
# pc/hal/dbg_platform_posix.c, Windows' pc/hal/win/win_dbg_platform.c.
WIN_HAL_DIR = $(HAL_DIR)/win
HAL_SRCS := $(filter-out $(HAL_DIR)/dbg_platform_posix.c,$(HAL_SRCS)) \
            $(wildcard $(WIN_HAL_DIR)/*.c)
endif
# The modules are generated (from the decompiled overlay C of toki1-decomp and
# the PC overrides in pc/src/overlay), and toki1-pc does not track them: the
# decompiled C lives in toki1-decomp only.  A checkout without them generates
# them before its first build (~6 minutes); after a toki1-decomp update,
# `make -f pc/Makefile overlay-modules` (or pc/tools/run_battery.sh, whose
# first stage does it) regenerates them.  The monorepo tracks them, so this
# never fires there.
# This block stands BEFORE the first $(wildcard) of pc/src and tests with the
# shell: GNU make 4.3 caches a directory's listing at its first read, and a
# module generated after that was missing from OVERLAY_SRCS (undefined
# overlay functions at the link, on a clean checkout only).
OVERLAY_MODULE_SKIP = $(NO_PART_GOALS) overlay-modules print-parts test windows linux-package
ifneq ($(filter-out $(OVERLAY_MODULE_SKIP),$(or $(MAKECMDGOALS),all)),)
ifeq ($(shell ls $(PCSRC_DIR)/overlay_[A-Z]*.c 2>/dev/null | head -1),)
$(info GEN  pc/src/overlay_<OV>.c (pc/tools/generate_overlay_modules.py, from $(DECOMP_DIR)))
OVERLAY_GEN_FAILED := $(shell python3 pc/tools/generate_overlay_modules.py >&2 || echo failed)
ifneq ($(OVERLAY_GEN_FAILED),)
$(error pc/tools/generate_overlay_modules.py failed)
endif
endif
endif
PC_SRCS = $(wildcard $(PCSRC_DIR)/*.c)
# The English entry points call the patch engine: only with toki1-english.
ifneq ($(HAVE_ENGLISH),1)
PC_SRCS := $(filter-out $(PCSRC_DIR)/lang_patch_entries.c,$(PC_SRCS))
endif

# Overlay sources — unified per-overlay modules generated by
# pc/tools/generate_overlay_modules.py. Each overlay's functions are
# prefixed to avoid symbol conflicts (overlays share 0x80132000 on PS1).
OVERLAY_SRCS = $(wildcard $(PCSRC_DIR)/overlay_*.c)
.PHONY: overlay-modules
overlay-modules:
	python3 pc/tools/generate_overlay_modules.py

# Overlay init-routine replays (prefixed per-overlay to avoid the
# 0x80132000 symbol collision). Generated by
# tools/overlay/extract_init_routines.py.
OVERLAY_INIT_DIR = $(PCSRC_DIR)/overlay_inits
OVERLAY_INIT_SRCS = $(wildcard $(OVERLAY_INIT_DIR)/*.c)

# Mednafen GPU shim (C++) — single .cpp includes 3 raster TUs.
#
# The backend itself is NOT in the tree (see THIRD-PARTY.md): it is a parity
# reference for the rasterizer, not a shipped path, so it is opt-in. With
# MDFN_GPU_BACKEND=1 and a pc/3rd/mednafen-gpu/upstream/ checkout present the
# real rasterizer is compiled and linked; otherwise pc/hal/mdfn_gpu_stub.c
# satisfies the interface and mdfn_gpu_enabled() is a constant 0.
MDFN_GPU_BACKEND ?= 0
MDFN_SHIM_SRC = $(MDFN_DIR)/shim/mdfn_gpu_shim.cpp
MDFN_SHIM_OBJ = $(OBJ_DIR)/mdfn_gpu_shim.o
ifeq ($(MDFN_GPU_BACKEND),1)
  ifeq ($(wildcard $(MDFN_SHIM_SRC)),)
    $(error MDFN_GPU_BACKEND=1 needs the backend sources: pc/3rd/mednafen-gpu/upstream/\
      and shim/mdfn_gpu_shim.cpp are not in the tree (see THIRD-PARTY.md). \
      Drop your own copy in pc/3rd/mednafen-gpu/ or build without MDFN_GPU_BACKEND.)
  endif
  CFLAGS += -DMDFN_GPU_BACKEND=1
  MDFN_BUILD_OBJ = $(MDFN_SHIM_OBJ)
  MDFN_STUB_SRC =
else
  MDFN_BUILD_OBJ =
  MDFN_STUB_SRC = $(HAL_DIR)/mdfn_gpu_stub.c
endif

# All sources
ALL_SRCS = $(PCPORT_SRCS) $(MATCHED_SRCS) $(SEMI_SRCS) $(ARCHIVE_SRCS) $(HAL_SRCS) $(PC_SRCS) $(OVERLAY_SRCS) $(OVERLAY_INIT_SRCS) $(MDFN_STUB_SRC)

# Generate object file paths
ALL_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(ALL_SRCS))) $(MDFN_BUILD_OBJ)
# Source-specific dependency names also survive pc-port/matched tier switches.
source_dep = $(OBJ_DIR)/$(subst /,_,$(basename $(1))).d
DEPFLAGS = -MMD -MP -MF $(@:.o=.d) -MT $@
.PHONY: pc-deps-bootstrap

# Reuse the unchanged SDK display command producer behind a native table.
SDK_DISPLAY_OBJ = $(OBJ_DIR)/gpu_sdk_put_disp_env.o
ALL_OBJS += $(SDK_DISPLAY_OBJ)
$(SDK_DISPLAY_OBJ): $(MATCHED_DIR)/PutDispEnv.c pc/Makefile | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -DPutDispEnv=gpu_sdk_put_disp_env \
	    -Dgraph_jtb=gpu_sdk_display_jtb -c $< -o $@

# Reuse _clr and its diagnostic preflight without the raw MIPS table/DMA.
# Common storage binds to the existing PS1 RAM label (or fixture alias).
SDK_CLEAR_OBJS = $(OBJ_DIR)/gpu_sdk_clear_image.o \
                 $(OBJ_DIR)/gpu_sdk_check_rect.o $(OBJ_DIR)/gpu_sdk_set_alarm.o
ALL_OBJS += $(SDK_CLEAR_OBJS)
$(OBJ_DIR)/gpu_sdk_clear_image.o: $(MATCHED_DIR)/_clr.c pc/Makefile | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -fcommon -D_clr=gpu_sdk_clear_image \
	    -D_param=gpu_clear_get_info -D_cwc=gpu_clear_submit -c $< -o $@
$(OBJ_DIR)/gpu_sdk_check_rect.o: $(MATCHED_DIR)/checkRECT.c pc/Makefile | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -DcheckRECT=gpu_sdk_check_rect \
	    -DGPU_printf=gpu_sdk_gpu_printf -c $< -o $@
$(OBJ_DIR)/gpu_sdk_set_alarm.o: $(MATCHED_DIR)/set_alarm.c pc/Makefile | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dset_alarm=gpu_sdk_set_alarm \
	    -DVSync=gpu_alarm_vsync -c $< -o $@

$(ALL_OBJS): pc/include/hal.h pc/include/libgpu.h

# Sony BIOS thunk names (strncpy, printf, rand, open, read, ...) are libc names on PC.
# Game translation units get the scoped alias header (maps them back to the
# bios_trampolines.c entry of the same address); HAL, pc/src and pc-port overrides
# are compiled without it and keep the real libc.  See pc/include/game/sony_bios_alias.h.
GAME_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(MATCHED_SRCS) $(SEMI_SRCS) $(ARCHIVE_SRCS) $(OVERLAY_SRCS)))
# Retail data is linked at PS1 addresses. GCC may otherwise realign arrays
# for vectorization/memset synthesis even when declarations request less
# alignment, emitting faulting movaps stores to addresses ending in 4.
PS1_LAYOUT_CFLAGS = -fno-tree-vectorize -fno-tree-loop-distribute-patterns
$(GAME_OBJS): CFLAGS += -include pc/include/game/sony_bios_alias.h $(PS1_LAYOUT_CFLAGS)
$(GAME_OBJS): pc/Makefile
# Game-code CPU time (pc/hal/cpu_cost.c): every PS1 function the port runs
# natively reports its entry, and the port charges retail's measured time.
# bios_trampolines.c models its BIOS calls' time itself.
COST_OBJS = $(GAME_OBJS) $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(filter-out $(PCPORT_DIR)/bios_trampolines.c,$(PCPORT_SRCS))))
$(COST_OBJS): CFLAGS += -pg -mfentry -fno-builtin-abs
# Calls stay calls (timing round 6): the cost table's caller rows
# (cpu_cost.c ticks_for, '... from SD_SerialVolInit') key on __fentry__'s
# caller return address, and IDO never turns a call into a jump.  GCC -O2
# did (`jmp cd_command_issue` at the end of SD_SerialVolInit), so the callee
# saw its caller's caller and charged the scene mean (274) instead of the
# measured 167.  `make COST_SIBCALLS=1` restores GCC's sibling calls (the
# negative control; a clean build).
ifneq ($(COST_SIBCALLS),1)
$(COST_OBJS): CFLAGS += -fno-optimize-sibling-calls
endif
# The PS1 C library's abs() is a call on retail (title_obj_move: 13 a
# frame); GCC inlines it.  Game code calls it, cpu_cost.c __wrap_abs
# charges retail's time (HAL_SDK_CPU=0: not charged).
LDFLAGS += -Wl,--wrap=abs
$(COST_OBJS): pc/Makefile
# PS1 globals that carry a second C name (a member alias inside screen_w /
# class_w / gSDInfo / cdq_*, a typed view at the same address): game code
# reaches them through an opaque address, so GCC cannot assume the two names
# are disjoint objects and reorder a write through one around a read through
# the other (NAME_ENT's birth month).  pc/tools/gen_ps1_alias_refs.py writes
# the header and pc/src/ps1_alias_refs.c from the two .ld files.
$(COST_OBJS): CFLAGS += -include pc/include/game/ps1_alias_refs.h
$(COST_OBJS): pc/include/game/ps1_alias_refs.h
$(OBJ_DIR)/spu.o $(OBJ_DIR)/spu_transfer.o $(OBJ_DIR)/spu_reset.o $(OBJ_DIR)/cd.o: pc/hal/spu_transfer.h
$(OBJ_DIR)/spu.o $(OBJ_DIR)/spu_reset.o: pc/hal/spu_reset.h
$(OBJ_DIR)/gpu.o $(OBJ_DIR)/gpu_reset.o $(OBJ_DIR)/system.o: pc/hal/gpu_reset.h
$(OBJ_DIR)/gpu.o $(OBJ_DIR)/gpu_reset.o $(OBJ_DIR)/gpu_queue.o $(OBJ_DIR)/gpu_clear.o: pc/hal/gpu_queue.h
$(OBJ_DIR)/gpu.o $(OBJ_DIR)/gpu_clear.o: pc/hal/gpu_clear.h
$(OBJ_DIR)/irq.o $(OBJ_DIR)/sdk_irq.o $(OBJ_DIR)/system.o $(OBJ_DIR)/bios_trampolines.o $(OBJ_DIR)/main.o: pc/include/pc_irq.h
$(OBJ_DIR)/trapIntrVSync.o: CFLAGS += -Dlibapi_setintrvsync_var_8cfc=hal_sdk_vsync_hook -Dlibapi_vsync_callback=hal_sdk_vsync_hook -DD_800E8CFC=hal_sdk_vsync_hook
# libsnd's current-VAB pointer globals _svm_vh/_svm_pg/_svm_tn (and their
# D_80131618/1C/20 aliases) are 4-byte PS1 slots; their users declare them as
# pointers, so a native 8-byte access folds the neighbouring slot and a store
# clobbers it.  Every user gets one native pointer global per name instead
# (defined in pc/hal/spu.c; values are zero-extended PS1 addresses, so the
# s32-typed spellings read/write its low half consistently).
SND_PTR_USERS = SpuVmGetProgPan SpuVmGetProgVol SpuVmKeyOn SpuVmKeyOnNow \
                SpuVmPitchBend SpuVmSelectToneAndVag SpuVmSetProgPan SpuVmSetProgVol \
                SpuVmVSetUp SsUtGetProgAtr SsUtGetVabHdr SsUtGetVagAtr SsUtSetVagAtr \
                note2pitch2 SpuVmDoAllocate
SND_PTR_GLOBALS = -D_svm_vh=hal_svm_vh -DD_80131618=hal_svm_vh \
                  -D_svm_pg=hal_svm_pg -DD_8013161C=hal_svm_pg \
                  -D_svm_tn=hal_svm_tn -DD_80131620=hal_svm_tn
$(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(SND_PTR_USERS))): CFLAGS += $(SND_PTR_GLOBALS)
# libpress's sync waits poll DMA0/DMA1 CHCR (0x1F801088 / 0x1F801098) through
# `u32 *` cells at 0x800D0990 / 0x800D099C.  A native 8-byte read of the
# 4-byte cell joins it with its neighbour (0x1F8010A0_1F801098, a
# non-canonical address); MDEC_out_sync only survived through main.c's
# zero-page fault emulation.  Each user gets the cell's value as a native
# pointer instead (pc/hal/libpress.c).
MDEC_SYNC_USERS = MDEC_in_sync MDEC_out_sync
$(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(MDEC_SYNC_USERS))): CFLAGS += \
    -Dlibpress_mdec_in_ptr_0990=hal_mdec_dma0_chcr -Dlibpress_dma0_chcr_reg_ptr=hal_mdec_dma0_chcr -DD_800D0990=hal_mdec_dma0_chcr -Dlibpress_mdec_out_sync_ptr_099c=hal_mdec_dma1_chcr -Dlibpress_dma1_chcr_reg_ptr=hal_mdec_dma1_chcr -DD_800D099C=hal_mdec_dma1_chcr
# libspu reaches the SPU and DMA4 through 4-byte PS1 pointer cells
# (0x800E3170 _spu_RXX, 0x800E3174..84 DMA4 MADR/BCR/CHCR, DPCR, DICR,
# 0x800E3188 SPU_DELAY).  A native 8-byte load joins two cells into a
# non-canonical address: _spu_r (SpuRead, scene 17's SD_CDLevelRead)
# crashed on DICR, and every other access ran through main.c's zero-page
# emulation.  Each user gets native pointers holding the cells' retail
# values (pc/hal/spu_transfer.c, which also services the DMA4 channel).
SPU_REG_USERS = _spu_r _spu_r_ _spu_w _spu_setDelayR _spu_setDelayW \
                _spu_setCommonAttr _spu_getCommonAttr _spu_reset SysSpu_getRegister \
                SysSpu_setRegister _spu_interruptDMA _spu_setVoiceAttr _spu_ioctl \
                _spu_write _spu_init _spu_setReverbAttr _spu_writeByIO
$(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(SPU_REG_USERS))): CFLAGS += \
    -D_spu_RXX=hal_spu_rxx -Dlibspu_spu_r_ptr_3174=hal_spu_dma4_madr -Dlibspu_dma4_madr_reg_ptr=hal_spu_dma4_madr -DD_800E3174=hal_spu_dma4_madr \
    -Dlibspu_spu_r_ptr_3178=hal_spu_dma4_bcr -Dlibspu_dma4_bcr_reg_ptr=hal_spu_dma4_bcr -DD_800E3178=hal_spu_dma4_bcr -Dlibspu_spu_r_ptr_317c=hal_spu_dma4_chcr -Dlibspu_dma4_chcr_reg_ptr=hal_spu_dma4_chcr -DD_800E317C=hal_spu_dma4_chcr \
    -Dlibspu_spu_r_ptr_3180=hal_spu_dma_dpcr -Dlibspu_dma_dpcr_reg_ptr=hal_spu_dma_dpcr -DD_800E3180=hal_spu_dma_dpcr -Dlibspu_spu_r_ptr_3184=hal_spu_dma_dicr -Dlibspu_dma_dicr_reg_ptr=hal_spu_dma_dicr -DD_800E3184=hal_spu_dma_dicr \
    -Dlibspu_dev_delay_reg_ptr=hal_spu_delay -DD_800E3188=hal_spu_delay
$(addprefix $(OBJ_DIR)/,$(addsuffix .o,InitSoundSeq ReadDeltaValue GetSeqData GetMetaEvent \
    SetControlChange SetPitchBend Snd_nextseq)): src/main/pc-port/snd_score.h
# CdInit spells its callbacks by name; only its error printf is swapped.
$(OBJ_DIR)/CdInit.o: CFLAGS += -Dfunc_800B4850=cd_init_error_printf
$(OBJ_DIR)/cd.o $(OBJ_DIR)/bios_trampolines.o $(OBJ_DIR)/bios_printf.o $(OBJ_DIR)/gpu_reset.o $(OBJ_DIR)/gpu_clear.o: pc/include/bios_printf.h
$(OBJ_DIR)/overlay_TT.o: pc/include/tt_draw_buffer.h pc/include/tt_serve_unit.h
$(OBJ_DIR)/watchdog.o $(OBJ_DIR)/system.o $(OBJ_DIR)/main.o: pc/include/pc_watchdog.h
$(OBJ_DIR)/parity.o $(OBJ_DIR)/spu.o $(OBJ_DIR)/trace.o: pc/hal/parity.h


# VPATH for finding sources
VPATH = $(PCPORT_DIR):$(MATCHED_DIR):$(SEMI_DIR):$(ARCHIVE_DIR):$(HAL_DIR):$(PCSRC_DIR):$(OVERLAY_INIT_DIR):$(foreach dir,$(OVERLAY_DIRS),$(dir))
ifeq ($(PLATFORM),windows)
VPATH := $(VPATH):$(WIN_HAL_DIR)
endif

.PHONY: all run clean assets count test

# HAL unit tests: minimal harness, links a focused subset of the HAL
# (no SDL / no full game). Run via `make -f pc/Makefile test`.
# Mouse helpers resolve by basename through the same tier order the link uses
# (pc-port > matched > semi): an override can retire at any time and leave the
# matched C as the winner, so never hardcode a tier path here.
tier_src = $(firstword $(wildcard $(PCPORT_DIR)/$(1).c $(MATCHED_DIR)/$(1).c $(SEMI_DIR)/$(1).c))
TEST_TARGET := pc/build/run_tests
TEST_SRCS   := $(wildcard pc/tests/test_*.c) \
               pc/hal/mdec_native.c pc/hal/file_id.c \
               pc/hal/nbn_trace.c \
               pc/src/file_lba_table.c pc/src/pc_debug.c \
               pc/hal/pad_device.c pc/src/input_globals.c \
               pc/hal/settings.c pc/hal/present_layout.c pc/hal/present_crt.c pc/hal/menu.c \
               pc/hal/save_browser.c pc/hal/ui_font.c pc/build/gen/ui_font_data.c \
               pc/hal/rewind_codec.c pc/hal/seen_text.c pc/hal/controls_text.c pc/hal/pad_map.c \
               pc/hal/lang.c pc/hal/translation.c \
               $(MATCHED_DIR)/pad_main.c $(MATCHED_DIR)/pad_type_check.c \
               $(MATCHED_DIR)/mouse_pad_set.c $(MATCHED_DIR)/pad_get.c \
               $(call tier_src,MouseState) $(call tier_src,mouse_pos_clamp) \
               $(MATCHED_DIR)/RangeMouse.c $(MATCHED_DIR)/SetMouse.c \
               $(MATCHED_DIR)/SenseMouse.c $(call tier_src,InitMouse) \
               $(call tier_src,MouseRead) pc/tests/head_forwarders.c \
               src/main/pc-port/GsSortObject.c \
               src/main/pc-port/GsSortObject2.c \
               src/main/pc-port/GsSortObject4.c \
               src/main/pc-port/palette_load_vram.c \
               src/main/pc-port/title_init_ModelingData.c \
               src/main/pc-port/initModelingData_init.c \
               src/main/pc-port/model_slot_set.c \
               $(D)src/overlay/OPTION/option_row_pos_set.c
TEST_CFLAGS := -fno-pie -no-pie -std=gnu17 -fpermissive -O0 -g -w -funsigned-char -fno-strict-aliasing -D_GNU_SOURCE \
               -I pc/include/game -I pc/include -I $(D)include \
               -include pc/include/game/types.h

TEST_DEPS := src/main/pc-port/_make_packet.c \
             src/main/pc-port/GsSortFastSprite.c src/main/pc-port/GsLinkObject4.c \
             src/main/pc-port/cal_font_load.c pc/src/overlay/TT/tt_anim.c

$(TEST_TARGET): $(TEST_SRCS) $(TEST_DEPS) | pc/build
	@$(CC) $(TEST_CFLAGS) $(TEST_SRCS) -lm -o $@

# Four tests (MDEC, NBN TOC, file-id LBA) read retail files out of
# pc/assets/raw/CDROM, which `make -f pc/Makefile assets DISC_PATH=<track 1 .bin>`
# extracts (run_game.sh --prepare-only writes the player's copy elsewhere).
test: $(TEST_TARGET)
	@if [ ! -d pc/assets/raw/CDROM/FUKUTEST ]; then \
		echo "NOTE: pc/assets/raw/CDROM is missing: 4 tests will fail. Run"; \
		echo "      make -f pc/Makefile assets DISC_PATH=<your track 1 .bin> first."; \
	fi
	@./$(TEST_TARGET)

# The PC layer's UI font (pc/include/pc_ui.h): pc/font/latin12.txt (the
# English patch's font, tools/patch/font/latin12.txt, kept here as an exact
# copy so the port builds without toki1-english; pc/tools/test_ui_font.py
# fails when the two differ) and the PC-only symbols of
# pc/font/ui_extra12.txt, compiled into C at build time -- never by hand.
UI_FONT_DEPS = pc/tools/gen_ui_font.py pc/font/latin12.txt \
               pc/font/ui_extra12.txt
UI_FONT_OBJ  = $(BUILD_DIR)/gen/ui_font_data.o
%/gen/ui_font_data.c: $(UI_FONT_DEPS)
	@mkdir -p $(dir $@)
	@python3 pc/tools/gen_ui_font.py --out $@
$(UI_FONT_OBJ): $(BUILD_DIR)/gen/ui_font_data.c
	@$(CC) $(CFLAGS) -c $< -o $@
$(TARGET): $(UI_FONT_OBJ)
.SECONDARY: $(BUILD_DIR)/gen/ui_font_data.c pc/build/gen/ui_font_data.c


all: $(TARGET)

# pc/hal/licenses.c embeds the licence texts with .incbin (pc/include/pc_setup.h)
# LICENSE: the project's own (MIT) licence, toki1-decomp's -- the decompiled
# sources the program is built from.
$(OBJ_DIR)/licenses.o: $(wildcard pc/licenses/*.txt) $(D)LICENSE
ifneq ($(D),)
$(OBJ_DIR)/licenses.o: CFLAGS += -DPC_PROJECT_LICENSE='"$(D)LICENSE"'
endif

# Windows shorthand: `make -f pc/Makefile windows` (= PLATFORM=windows all).
.PHONY: windows
windows:
	@$(MAKE) --no-print-directory -f pc/Makefile PLATFORM=windows all

# Linux player package binary: pc/build-linux-pkg/tokimeki-pc (glibc 2.31,
# static SDL2, portable; pc/tools/setup_linux_pkg_root.sh first, then
# pc/tools/package_linux.py).  LINUX_PKG_ROOT=host: the host-glibc build.
.PHONY: linux-package
linux-package:
	@if [ "$(LINUX_PKG_ROOT)" != host ] && [ ! -x build/linux-pkg-root/usr/bin/gcc -o ! -f build/sdl2-linux-glibc231/lib/libSDL2.a ]; then \
	    echo "linux-package: run pc/tools/setup_linux_pkg_root.sh first (the glibc 2.31 build root)"; exit 2; fi
	@$(MAKE) --no-print-directory -f pc/Makefile LINUX_PKG=1 all

ifneq ($(filter windows,$(PLATFORM))$(PKG_AR),)
# The SPU core, compiled from source for MinGW and for the old-root Linux
# package (same sources, include order and flags as pc/3rd/spu-glue/Makefile).
SPU_WIN_DIR = $(BUILD_DIR)/spu
SPU_WIN_SRCS = $(SPU_CORE_DIR)/upstream/spu.cpp $(SPU_GLUE_DIR)/stubs/spu_stubs.cpp $(SPU_GLUE_DIR)/stubs/spu_shim.cpp
SPU_WIN_OBJS = $(patsubst %.cpp,$(SPU_WIN_DIR)/%.o,$(notdir $(SPU_WIN_SRCS)))
SPU_WIN_CXXFLAGS = -std=c++17 -O2 -fno-exceptions -fno-rtti -w \
                   -I $(SPU_CORE_DIR) -I $(SPU_GLUE_DIR)/stubs -I $(SPU_CORE_DIR)/upstream
ifeq ($(PLATFORM),windows)
SPU_WIN_CXXFLAGS += -D__USE_MINGW_ANSI_STDIO=1
else
WIN_AR = $(PKG_AR)
endif
$(SPU_WIN_DIR)/%.o: $(SPU_CORE_DIR)/.fetched $(SPU_CORE_DIR)/upstream/%.cpp pc/Makefile
	@mkdir -p $(SPU_WIN_DIR)
	@echo "CXX $(filter %.cpp,$^)"
	@$(CXX) $(SPU_WIN_CXXFLAGS) -c $(filter %.cpp,$^) -o $@
$(SPU_WIN_DIR)/%.o: $(SPU_CORE_DIR)/.fetched $(SPU_GLUE_DIR)/stubs/%.cpp pc/Makefile
	@mkdir -p $(SPU_WIN_DIR)
	@echo "CXX $(filter %.cpp,$^)"
	@$(CXX) $(SPU_WIN_CXXFLAGS) -c $(filter %.cpp,$^) -o $@
$(SPU_PSX_LIB): $(SPU_WIN_OBJS)
	@rm -f $@
	@$(WIN_AR) rcs $@ $^
else
# The native build: pc/3rd/spu-glue/Makefile, into SPU_CORE_DIR (the archive
# the audio unit tests link too, pc/tools/spu_core.py).
SPU_CORE_SRCS = $(wildcard $(SPU_GLUE_DIR)/stubs/*.cpp $(SPU_GLUE_DIR)/stubs/*.h \
                $(SPU_GLUE_DIR)/stubs/*/*.h $(SPU_GLUE_DIR)/patches/*.patch \
                $(SPU_CORE_DIR)/upstream/spu.cpp) $(SPU_GLUE_DIR)/Makefile
$(SPU_PSX_LIB): $(SPU_CORE_DIR)/.fetched $(SPU_CORE_SRCS)
	@$(MAKE) --no-print-directory -C $(SPU_GLUE_DIR) SPU_CORE_DIR=$(abspath $(SPU_CORE_DIR))
	@touch $@
endif

# A missing or stale SPU core (no stamp, or a patch newer than it) stops the
# build here with the fix, instead of a "No rule to make target".
define spu_core_missing
	@echo "error: the SPU core is missing or out of date in $(SPU_CORE_DIR)." >&2
	@echo "       Fetch it with pc/3rd/spu-glue/fetch.sh (see pc/3rd/SPU-CORE.md)." >&2
	@exit 1
endef
$(SPU_CORE_DIR)/.fetched: $(wildcard $(SPU_GLUE_DIR)/patches/*.patch)
	$(spu_core_missing)
$(SPU_CORE_DIR)/upstream/%:
	$(spu_core_missing)


ifeq ($(PLATFORM),windows)
# The icon, the manifest (UTF-8 code page, DPI awareness) and version block.
WIN_RES = $(OBJ_DIR)/tokimeki-pc.res.o
$(WIN_RES): pc/win/tokimeki-pc.rc pc/win/tokimeki-pc.manifest pc/win/tokimeki-pc.ico | $(OBJ_DIR)
	@echo "WINDRES $<"
	@$(MINGW_BIN)x86_64-w64-mingw32-windres --preprocessor=$(CC) --preprocessor-arg=-E \
	    --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED $< -O coff -o $@
endif

$(TARGET): $(ALL_OBJS) $(OBJ_DIR)/.srcmap $(SPU_PSX_LIB) $(WIN_RES) | $(BUILD_DIR)
	@echo "LINK $@"
ifeq ($(PLATFORM),windows)
	@python3 pc/tools/pe_rebind_relocs.py $(filter %.o,$(filter-out $(OBJ_DIR)/.srcmap $(SPU_PSX_LIB) $(MDFN_SHIM_OBJ) $(WIN_RES),$^))
endif
	@$(or $(LINK_CC),$(CC)) $(filter-out $(OBJ_DIR)/.srcmap $(SPU_PSX_LIB) $(STATE_INV_C),$^) $(ALIAS_LD) $(LDFLAGS) $(LDFLAGS_SAN) -o $@
ifeq ($(PLATFORM),windows)
	@$(MINGW_BIN)x86_64-w64-mingw32-objcopy --remove-section='.debug_*' $@
ifeq ($(WIN_SDL),dll)
	@# the negative-control build loads SDL2.dll from its own folder
	@cp $(SDL2_WIN)/bin/SDL2.dll $(BUILD_DIR)/SDL2.dll
else
	@rm -f $(BUILD_DIR)/SDL2.dll
endif
endif
	@echo "Built: $@ ($$(ls -lh $@ | awk '{print $$5}'))"

# ---- English translation (docs/pc-port/TRANSLATION-ON-PC.md) -------------
# The patch engine (src/patch/main, the PS1 English patch's C) compiled
# natively, the generated hook stubs and pins, and -- when the build machine
# has one -- the translation pack embedded in the executable.  Japanese is
# unaffected: the stubs jump straight to the retail functions while
# g_hal_lang_en is 0 (pc/hal/lang.c).
# All of it needs the English patch (toki1-english, ENGLISH_DIR).  Without
# it the build has no engine, no hooks and an empty pack: the Japanese game
# (pc/hal/lang.c "this build has no patch engine"; every reference to the
# engine from pc/hal and src/main/pc-port is weak).
LANG_DIR      = $(BUILD_DIR)/translation
LANG_GEN_DIR  = $(LANG_DIR)/gen
LANG_OBJ_DIR  = $(LANG_DIR)/obj
# The pack: pc/tools/translation_pack.py build (needs the English patch
# build and the disc on THIS machine; never committed).  It writes the one
# pack pc/build/translation/en.tktp, which every platform (linux, windows,
# linux-package) embeds; TKTP= overrides.
TKTP ?= pc/build/translation/en.tktp
ifeq ($(HAVE_ENGLISH),1)
TKTP_IN = $(wildcard $(TKTP))
PATCH_MAIN    = text_vwf text_vwf_draw text_reveal text_msg text_field text_event \
                text_misc text_names text_choice text_name_ent text_ov_labels \
                text_ov_date2 text_ov_bunkasai
PATCH_CFLAGS  = -include pc/include/game/sony_bios_alias.h -I $(E_SRC) \
                -Dset_kanji_string=pt_set_kanji_string -Dload_string=pt_load_string \
                -Ddisp_string=pt_disp_string
LANG_OBJS     = $(patsubst %,$(LANG_OBJ_DIR)/patch_%.o,$(PATCH_MAIN)) \
                $(LANG_OBJ_DIR)/patch_font_latin.o $(LANG_OBJ_DIR)/lang_hooks.o
# Overlay modules with English hooks: their adapted source goes through
# gen_lang_hooks.py --ovl-filter (calls -> <sym>__langhook stubs).
LANG_OVL_SRCS = $(foreach o,NAME_ENT SHOUGATU BUNKA_SD BUNKASAI DATE2 ENDING ETC,$(PCSRC_DIR)/overlay_$(o).c)
$(patsubst $(PCSRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(LANG_OVL_SRCS)): pc/tools/gen_lang_hooks.py $(E_CONFIG)/english.json
LANG_GEN_DEPS = pc/tools/gen_lang_hooks.py $(E_CONFIG)/english.json \
                $(E_CONFIG)/symbols.json $(D)config/level3_functions.json

$(LANG_GEN_DIR)/lang.mk $(LANG_GEN_DIR)/lang_hooks.c $(LANG_GEN_DIR)/patch_pins.ld: $(LANG_GEN_DEPS)
	@mkdir -p $(LANG_GEN_DIR)
	@python3 pc/tools/gen_lang_hooks.py $(LANG_GEN_DIR)
	@printf 'LANG_WRAP_FLAGS = %s\n' "$$(cat $(LANG_GEN_DIR)/wrap.flags)" > $(LANG_GEN_DIR)/lang.mk
ifneq ($(filter-out clean print-objs print-excludes print-parts,$(or $(MAKECMDGOALS),all)),)
-include $(LANG_GEN_DIR)/lang.mk
endif
LDFLAGS += $(LANG_WRAP_FLAGS)

$(LANG_GEN_DIR)/font_latin.c: $(E_TOOLS)/fontgen.py $(E_TOOLS)/font/latin12.txt \
                              $(E_TOOLS)/font/icons12.txt $(E_TOOLS)/textcodec.py
	@mkdir -p $(LANG_GEN_DIR)
	@python3 $(E_TOOLS)/fontgen.py --out $(LANG_GEN_DIR) > /dev/null

$(LANG_GEN_DIR)/src/%.c: $(E_SRC)/%.c pc/tools/adapt_patch_sources.py
	@mkdir -p $(dir $@)
	@python3 pc/tools/adapt_patch_sources.py $< $@

$(LANG_OBJ_DIR)/patch_%.o: $(LANG_GEN_DIR)/src/%.c $(E_SRC)/patch_text.h pc/Makefile
	@mkdir -p $(LANG_OBJ_DIR)
	@echo "CC  (patch) $(notdir $<)"
	@$(CC) $(CFLAGS) $(PS1_LAYOUT_CFLAGS) $(PATCH_CFLAGS) -c $< -o $@

$(LANG_OBJ_DIR)/patch_font_latin.o: $(LANG_GEN_DIR)/font_latin.c
	@mkdir -p $(LANG_OBJ_DIR)
	@$(CC) $(CFLAGS) -c $< -o $@

$(LANG_OBJ_DIR)/lang_hooks.o: $(LANG_GEN_DIR)/lang_hooks.c
	@mkdir -p $(LANG_OBJ_DIR)
	@$(CC) $(CFLAGS) -c $< -o $@

LANG_PINS_LD = $(LANG_GEN_DIR)/patch_pins.ld
.SECONDARY: $(patsubst %,$(LANG_GEN_DIR)/src/%.c,$(PATCH_MAIN)) $(LANG_GEN_DIR)/font_latin.c
else
# No English part: no engine, no hooks; an existing pc/build*/translation/
# en.tktp is NOT embedded (there is no engine to apply it).
TKTP_IN =
LANG_OBJS =
LANG_PINS_LD =
LANG_OVL_SRCS =
# pc/hal reaches the engine's data through weak references, which read as
# NULL when nothing defines them (ELF).  PE links no undefined weak
# reference, so the Windows build gives each one the address 0 explicitly.
LANG_WEAK_SYMS = patch_dte_table patch_str_table patch_kanji_sub_codes \
                 patch_kanji_sub_glyphs patch_kanji_sub_count patch_kanji_limit \
                 patch_entry_adv patch_glyph_x patch_font_rows patch_draw_hook \
                 g_lang_ovl_redirect pt_name_hold pt_name_put pt_strncat_nick
ifeq ($(PLATFORM),windows)
LDFLAGS += $(foreach s,$(LANG_WEAK_SYMS),-Wl,--defsym,$(s)=0)
endif
$(info NOTE toki1-english not found (../toki1-english, TOKI_ENGLISH_DIR): building the Japanese game without the English patch)
endif
# Always linked: without a pack it is an empty one (start == end), which
# translation.c reads as "no pack".  (Its weak references to the pack's
# symbols do not link on PE when the object is missing.)
LANG_OBJS    += $(LANG_OBJ_DIR)/translation_pack.o

# ELF only: the embedded pack's object marks a non-executable stack
ifeq ($(PLATFORM),windows)
TKTP_NOTE =
else
TKTP_NOTE = .section .note.GNU-stack,"",@progbits\n
endif
# Pack stamp: what the object embeds -- whether English is on, the resolved
# pack path and the pack's identity (SHA-1).  The object depends on it, not
# on the pack's mtime, so switching TKTP=, adding or removing toki1-english,
# or a new pack re-embeds, and an unchanged choice rebuilds nothing.  The
# stamp is rewritten only when its content changes (same rules as the
# compile-flags stamp below: never from a dry run or the wrappers).
TKTP_STAMP = $(LANG_OBJ_DIR)/.pack-stamp
ifneq ($(filter-out clean print-objs print-excludes print-parts windows linux-package,$(or $(MAKECMDGOALS),all)),)
ifeq ($(findstring n,$(firstword -$(MAKEFLAGS))),)
$(shell mkdir -p $(LANG_OBJ_DIR))
$(file >$(TKTP_STAMP).new,english=$(or $(HAVE_ENGLISH),0) pack=$(if $(TKTP_IN),$(abspath $(TKTP_IN)) sha1=$(firstword $(shell sha1sum '$(TKTP_IN)')),none))
$(shell cmp -s $(TKTP_STAMP).new $(TKTP_STAMP) && rm -f $(TKTP_STAMP).new || mv -f $(TKTP_STAMP).new $(TKTP_STAMP))
endif
endif
$(TKTP_STAMP): ;
$(LANG_OBJ_DIR)/translation_pack.o: $(TKTP_STAMP) $(TKTP_IN) pc/Makefile
	@mkdir -p $(LANG_OBJ_DIR)
ifeq ($(TKTP_IN),)
	@printf '.section .rodata\n.balign 16\n.globl tktp_embedded_start\ntktp_embedded_start:\n.globl tktp_embedded_end\ntktp_embedded_end:\n$(TKTP_NOTE)' > $(LANG_OBJ_DIR)/translation_pack.S
	@$(CC) -c $(LANG_OBJ_DIR)/translation_pack.S -o $@
	@echo "EMBED (no translation pack: English unavailable in this build)"
else
	@printf '.section .rodata\n.balign 16\n.globl tktp_embedded_start\ntktp_embedded_start:\n.incbin "%s"\n.globl tktp_embedded_end\ntktp_embedded_end:\n$(TKTP_NOTE)' "$(abspath $(TKTP))" > $(LANG_OBJ_DIR)/translation_pack.S
	@$(CC) -c $(LANG_OBJ_DIR)/translation_pack.S -o $@
	@echo "EMBED $(TKTP) ($$(stat -c %s $(TKTP)) bytes)"
endif

$(TARGET): $(LANG_OBJS) $(LANG_PINS_LD)

# Every object names its source explicitly.  Objects are keyed by basename
# and the tiers shadow each other by basename (pc-port > matched > semi >
# archive), so a VPATH pattern rule both picked the wrong file for a shared
# basename (pc/src/main.c vs the PS1 main) and, when an override was deleted
# or added, kept the object of the file that USED to win: it was newer than
# the new winner, so make never rebuilt it.  pc/tools/obj_srcmap.py records
# which source each object came from and deletes an object whose source
# changed, so an incremental build links what a clean build would.
# PS1 pointer tables (4-byte cells on the console) that game C declares as
# `extern T *name[]`: _ss_score (0x80131380, libsnd's score record per
# sequence; SsSetTableSize writes it 4 bytes per entry).  Every matched /
# semi / archive source that declares one is compiled through
# pc/tools/adapt_ps1_cells.py, which reads each `name[i]` as a 4-byte cell
# (the overlay module generator's rewrite); the source is not edited.
PS1_POINTER_TABLES = _ss_score
PS1_CELL_DIR = $(BUILD_DIR)/ps1_cells
ps1_empty :=
ps1_space := $(ps1_empty) $(ps1_empty)
ps1_comma := ,
PS1_CELL_TABLES_CSV := $(subst $(ps1_space),$(ps1_comma),$(strip $(PS1_POINTER_TABLES)))
PS1_CELL_TABLES_RE := $(subst $(ps1_space),|,$(strip $(PS1_POINTER_TABLES)))
PS1_CELL_SRCS := $(shell grep -lE 'extern[^;]*[*][[:space:]]*($(PS1_CELL_TABLES_RE))[[:space:]]*[[]' \
                   $(MATCHED_SRCS) $(SEMI_SRCS) $(ARCHIVE_SRCS) /dev/null)
# Game sources that read 4-byte PS1 pointer cells through casts:
# `((T **)x)[i]` chains and `*(T **)(addr)` (audit_ps1_pointer_casts.py
# CELL-READ; serifu_cmd_exec's per-girl message table, srn_vram_set's
# tile map).  The same filter reads each such cell 4 bytes wide.
PS1_CHAIN_SRCS = $(MATCHED_DIR)/serifu_cmd_exec.c $(MATCHED_DIR)/srn_vram_set.c \
                 $(ARCHIVE_DIR)/Fade_Out_Main_painted.c
# Game sources that turn a signed 32-bit PS1 address into a pointer
# (audit_ps1_pointer_casts.py SEXT-PTR: `(u8 *)tbl` with an `s32 tbl`,
# `(T *)(base + i * 8)`).  gcc SIGN-extends that conversion on LP64
# (0x800B9A00 -> 0xFFFFFFFF800B9A00); the console's address is the 32-bit
# value.  pc/tools/adapt_ps1_addresses.py rewrites each conversion from
# clang's AST of the translation unit to zero-extend (`(T *)(unsigned
# int)expr`); the source is not edited.  Every overlay module (generated)
# and the main-binary sources below go through it; a new SEXT-PTR row
# elsewhere fails audit_ps1_pointer_casts.py.  Before this filter,
# pc/src/main.c folded the sign-extended value at the fault, so a
# dereference worked but a comparison, a range check or a pointer
# difference on the value did not.
PS1_ADDR_DIR = $(BUILD_DIR)/ps1_addr
PS1_ADDR_SRCS = $(OVERLAY_SRCS) \
    $(MATCHED_DIR)/CD_cachefile.c \
    $(MATCHED_DIR)/SpuInitMalloc.c $(MATCHED_DIR)/SpuMalloc.c \
    $(MATCHED_DIR)/_debug_number_show.c $(MATCHED_DIR)/_debug_string_show.c \
    $(MATCHED_DIR)/bg_show_girl_out.c $(MATCHED_DIR)/card_file_save.c \
    $(MATCHED_DIR)/data_load_confirm.c $(MATCHED_DIR)/debug_hex_show.c \
    $(MATCHED_DIR)/dma_execute.c $(MATCHED_DIR)/goto_tpage.c \
    $(MATCHED_DIR)/iris_show.c $(MATCHED_DIR)/join_club_class_init.c \
    $(MATCHED_DIR)/machiawase_menu_set.c $(MATCHED_DIR)/machiawase_show.c \
    $(MATCHED_DIR)/mod_set_gpu.c $(MATCHED_DIR)/search_tpage_multi.c \
    $(MATCHED_DIR)/spot_light.c $(MATCHED_DIR)/sprintf.c \
    $(MATCHED_DIR)/sprite_scatter_show.c $(MATCHED_DIR)/sprite_set_gpu_sprt2.c \
    $(MATCHED_DIR)/srn_set.c $(MATCHED_DIR)/startIntr.c \
    $(MATCHED_DIR)/xa_set_m.c $(MATCHED_DIR)/xa_wait.c \
    $(SEMI_DIR)/SD_BranchCTRL.c $(SEMI_DIR)/func_80052600.c
# Main-binary sources with PC-build-only exact edits (pc/tools/
# ps1_source_edits.py): the argument the console's listing passes in a
# MIPS register the C does not name (call_args_plan.json; audit_ps1_call_
# args.py SHORT-READ) and PS1-width members of records that live in PS1
# memory (record_width_plan.json; audit_ps1_pointer_casts.py RECORD-PTR)
# and K&R variadic bodies that walk their arguments as stack words
# (varargs_plan.json; pc/src/ps1_varargs.c), and void functions whose
# callers read the $v0 their last call left (return_plan.json; audit_ps1_
# call_args.py VOID-READ).
# It is the first filter stage (before the cell and address stages); the
# source is not edited.  The overlay generator applies the overlay entries.
PS1_EDIT_DIR = $(BUILD_DIR)/ps1_edit
PS1_EDIT_SRCS := $(shell python3 pc/tools/ps1_source_edits.py --sources)
PS1_EDIT_DEPS = pc/tools/ps1_source_edits.py pc/tools/call_args_plan.json pc/tools/record_width_plan.json \
    pc/tools/varargs_plan.json pc/tools/function_cells_plan.json pc/tools/return_plan.json \
    pc/tools/stack_width_plan.json
# the input of the cell stage / of the address stage
ps1_cell_in = $(if $(filter $(1),$(PS1_EDIT_SRCS)),$(PS1_EDIT_DIR)/$(notdir $(1)),$(1))
ps1_addr_in = $(if $(filter $(1),$(PS1_CELL_SRCS) $(PS1_CHAIN_SRCS)),$(PS1_CELL_DIR)/$(notdir $(1)),$(call ps1_cell_in,$(1)))
ps1_edit_stage = $(if $(filter $(1),$(PS1_EDIT_SRCS)),@mkdir -p $(PS1_EDIT_DIR) && python3 pc/tools/ps1_source_edits.py $(1) $(PS1_EDIT_DIR)/$(notdir $(1)))
define OBJ_RULE
ifneq ($(filter $(1),$(PS1_ADDR_SRCS)),)
$(OBJ_DIR)/$(notdir $(basename $(1))).o: $(1) pc/tools/adapt_ps1_addresses.py pc/tools/audit_ps1_pointer_casts.py $(if $(filter $(1),$(PS1_CELL_SRCS) $(PS1_CHAIN_SRCS)),pc/tools/adapt_ps1_cells.py pc/tools/ps1_pointer_cells.py) $(if $(filter $(1),$(PS1_EDIT_SRCS)),$(PS1_EDIT_DEPS)) | $(OBJ_DIR)
	@mkdir -p $(PS1_ADDR_DIR) $(PS1_CELL_DIR)
	$(call ps1_edit_stage,$(1))
	$(if $(filter $(1),$(PS1_CELL_SRCS) $(PS1_CHAIN_SRCS)),@python3 pc/tools/adapt_ps1_cells.py $(if $(filter $(1),$(PS1_CELL_SRCS)),--tables $(PS1_CELL_TABLES_CSV)) $(if $(filter $(1),$(PS1_CHAIN_SRCS)),--chains) $(call ps1_cell_in,$(1)) $(PS1_CELL_DIR)/$(notdir $(1)))
	@python3 pc/tools/adapt_ps1_addresses.py $(call ps1_addr_in,$(1)) $(PS1_ADDR_DIR)/$(notdir $(1)) -- \
	    $$(CFLAGS) $$(SDL_CFLAGS) -iquote $(dir $(1))
	$(if $(filter $(1),$(LANG_OVL_SRCS)),@python3 pc/tools/gen_lang_hooks.py --ovl-filter $(PS1_ADDR_DIR)/$(notdir $(1)))
	@$$(CC) $$(CFLAGS) $$(SDL_CFLAGS) -iquote $(dir $(1)) -MMD -MP \
	    -MF $(call source_dep,$(1)) -MT $$@ -c $(PS1_ADDR_DIR)/$(notdir $(1)) -o $$@
else ifneq ($(filter $(1),$(PS1_CELL_SRCS) $(PS1_CHAIN_SRCS)),)
$(OBJ_DIR)/$(notdir $(basename $(1))).o: $(1) pc/tools/adapt_ps1_cells.py pc/tools/ps1_pointer_cells.py $(if $(filter $(1),$(PS1_EDIT_SRCS)),$(PS1_EDIT_DEPS)) | $(OBJ_DIR)
	@mkdir -p $(PS1_CELL_DIR)
	$(call ps1_edit_stage,$(1))
	@python3 pc/tools/adapt_ps1_cells.py $(if $(filter $(1),$(PS1_CELL_SRCS)),--tables $(PS1_CELL_TABLES_CSV)) $(if $(filter $(1),$(PS1_CHAIN_SRCS)),--chains) $(call ps1_cell_in,$(1)) $(PS1_CELL_DIR)/$(notdir $(1))
	@$$(CC) $$(CFLAGS) $$(SDL_CFLAGS) -iquote $(dir $(1)) -MMD -MP \
	    -MF $(call source_dep,$(1)) -MT $$@ -c $(PS1_CELL_DIR)/$(notdir $(1)) -o $$@
else ifneq ($(filter $(1),$(PS1_EDIT_SRCS)),)
$(OBJ_DIR)/$(notdir $(basename $(1))).o: $(1) $(PS1_EDIT_DEPS) | $(OBJ_DIR)
	$(call ps1_edit_stage,$(1))
	@$$(CC) $$(CFLAGS) $$(SDL_CFLAGS) -iquote $(dir $(1)) -MMD -MP \
	    -MF $(call source_dep,$(1)) -MT $$@ -c $(PS1_EDIT_DIR)/$(notdir $(1)) -o $$@
else
$(OBJ_DIR)/$(notdir $(basename $(1))).o: $(1) | $(OBJ_DIR)
	@$$(CC) $$(CFLAGS) $$(SDL_CFLAGS) -MMD -MP \
	    -MF $(call source_dep,$(1)) -MT $$@ -c $$< -o $$@
endif
ifeq ($(wildcard $(call source_dep,$(1))),)
$(OBJ_DIR)/$(notdir $(basename $(1))).o: pc-deps-bootstrap
endif
endef
$(foreach s,$(sort $(ALL_SRCS)),$(eval $(call OBJ_RULE,$(s))))
# Compiler-flags stamp: every object depends on the exact compiler command
# line, so a flag change (a new -D, the Windows force-include, ...) rebuilds
# what it affects with no manual clean, and an unchanged line rebuilds
# nothing.  The stamp is rewritten only when its content changes.  A build
# without the English patch says so in it: the overlay modules compile
# without the English call filter, so switching rebuilds them.
FLAGS_STAMP = $(OBJ_DIR)/.compile-flags
# (never from a dry run: the audits list compile commands with make -n;
# never from the `windows` / `linux-package` wrappers, which only re-invoke
# make for another build directory -- two of them at once raced on this one)
ifneq ($(filter-out clean print-objs print-excludes windows linux-package,$(or $(MAKECMDGOALS),all)),)
ifeq ($(findstring n,$(firstword -$(MAKEFLAGS))),)
$(shell mkdir -p $(OBJ_DIR))
$(file >$(FLAGS_STAMP).new,$(CC) $(CFLAGS) $(SDL_CFLAGS)$(if $(HAVE_ENGLISH),, [no English patch]))
$(shell cmp -s $(FLAGS_STAMP).new $(FLAGS_STAMP) && rm -f $(FLAGS_STAMP).new || mv -f $(FLAGS_STAMP).new $(FLAGS_STAMP))
endif
endif
$(foreach s,$(sort $(ALL_SRCS)),$(eval $(OBJ_DIR)/$(notdir $(basename $(s))).o: $(FLAGS_STAMP)))
$(FLAGS_STAMP): ;
# Queries must not delete objects; mixed all+test builds still refresh tiers.
ifneq ($(filter-out clean test print-objs print-excludes,$(or $(MAKECMDGOALS),all)),)
$(shell python3 pc/tools/obj_srcmap.py $(OBJ_DIR) $(ALL_SRCS))
endif

$(MDFN_SHIM_OBJ): $(MDFN_SHIM_SRC) | $(OBJ_DIR)
	@echo "CXX $<"
	@$(CXX) $(MDFN_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Existing objects without a dependency record rebuild once to acquire one.
$(foreach o,$(SDK_DISPLAY_OBJ) $(SDK_CLEAR_OBJS) $(MDFN_SHIM_OBJ),$(if $(wildcard $(o:.o=.d)),,$(eval $(o): pc-deps-bootstrap)))
-include $(foreach s,$(ALL_SRCS),$(call source_dep,$(s))) \
         $(SDK_DISPLAY_OBJ:.o=.d) $(SDK_CLEAR_OBJS:.o=.d) $(MDFN_SHIM_OBJ:.o=.d)

$(BUILD_DIR) $(OBJ_DIR):
	@mkdir -p $@

run: $(TARGET)
	@echo "Running $(TARGET)..."
	@./$(TARGET) pc/assets

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

assets:
	@if [ -z "$(DISC_PATH)" ]; then \
		echo "Usage: make -f pc/Makefile assets DISC_PATH=/path/to/disc.bin"; \
		exit 1; \
	fi
	python3 pc/tools/extract_assets.py "$(DISC_PATH)" pc/assets/

# Convert raw PS1 assets into PC-friendly formats. Requires `assets`
# already run (pc/assets/raw/CDROM populated). Safe to re-run; each
# sub-tool skips work whose output already exists.
#
#   TIM  -> PNG  (pc/assets/tim/)
#   VAB  -> SF2  (pc/assets/soundbanks/)
#   XA   -> WAV  (pc/assets/xa/)
#   TMD  -> OBJ  (pc/assets/tmd/)
convert-assets:
	@if [ ! -d pc/assets/raw/CDROM ]; then \
		echo "ERROR: pc/assets/raw/CDROM not found. Run 'make -f pc/Makefile assets DISC_PATH=...' first."; \
		exit 1; \
	fi
	@echo "[convert-assets] TIM -> PNG"
	@python3 pc/tools/tim_tool.py batch pc/assets/raw/CDROM pc/assets/tim/ 2>&1 | tail -5
	@echo "[convert-assets] XA -> WAV"
	@python3 pc/tools/xa_extract.py --batch pc/assets/raw/CDROM/XA_G pc/assets/xa/ 2>&1 | tail -5 || true
	@echo "[convert-assets] TMD -> OBJ"
	@python3 pc/tools/tmd_to_obj.py --batch pc/assets/raw/CDROM/TMD pc/assets/tmd/ 2>&1 | tail -5 || true
	@echo "[convert-assets] VAB -> SF2"
	@python3 pc/tools/vab2sf2.py 2>&1 | tail -5 || true
	@echo "[convert-assets] done"

# One-shot pipeline: dumpsxiso -> organize -> convert. Requires
# DISC_PATH. Idempotent.
setup-assets: assets convert-assets
	@echo "[setup-assets] complete. Build with: make -f pc/Makefile"

.PHONY: assets convert-assets setup-assets

count:
	@echo "Source files:"
	@echo "  PC-port overrides: $(words $(PCPORT_SRCS))"
	@echo "  Matched:           $(words $(MATCHED_SRCS))"
	@echo "  Semi-matched:      $(words $(SEMI_SRCS))"
	@echo "  HAL:               $(words $(HAL_SRCS))"
	@echo "  PC src:            $(words $(PC_SRCS))"
	@echo "  Overlays:          $(words $(OVERLAY_SRCS))"
	@echo "  Total:             $(words $(ALL_SRCS))"

# Validate overlay sources before building
.PHONY: validate-overlays
validate-overlays:
	@echo "Validating overlay source files..."
	@python3 $(D)tools/overlay/validate_overlay_sources.py 2>&1 | tail -20
	@echo "Note: Run 'python3 $(D)tools/overlay/validate_overlay_sources.py' for full report"

# Test that semi-matched C files compile
.PHONY: test-semi-matched
test-semi-matched:
	@echo "Testing semi-matched file compilation..."
	@failed=0; \
	for f in $(SEMI_DIR)/*.c; do \
		gcc -fsyntax-only -std=c89 -w -I $(D)include -I $(D)include/psyq "$$f" 2>/dev/null || { echo "FAIL: $$f"; failed=$$((failed+1)); }; \
	done; \
	echo "Semi-matched validation: $$failed failures"
debug-srcs:
	@echo "PCPORT_SRCS: $(words $(PCPORT_SRCS))"
	@echo "MATCHED_SRCS: $(words $(MATCHED_SRCS))"
	@echo "SEMI_SRCS: $(words $(SEMI_SRCS))"
	@echo "HAL_SRCS: $(words $(HAL_SRCS))"
	@echo "PC_SRCS: $(words $(PC_SRCS))"
	@echo "OVERLAY_SRCS: $(words $(OVERLAY_SRCS))"
	@echo "ALL_SRCS: $(words $(ALL_SRCS))"
	@echo "ALL_OBJS: $(words $(ALL_OBJS))"

# ---- save-state inventory (pc/hal/dbg_state.c) ---------------------------
# The source files and global data objects of every object linked into the
# executable: a save state holds the project's own native data and never a
# host library's (pc/tools/gen_state_inventory.py).
ifeq ($(PLATFORM),windows)
STATE_NM = $(MINGW_BIN)x86_64-w64-mingw32-nm
else
STATE_NM = nm
endif
STATE_INV_C   = $(BUILD_DIR)/gen/state_inventory.c
STATE_INV_OBJ = $(BUILD_DIR)/gen/state_inventory.o
STATE_INV_IN  = $(ALL_OBJS) $(LANG_OBJS) $(UI_FONT_OBJ) $(SPU_PSX_LIB)
$(STATE_INV_C): $(STATE_INV_IN) pc/tools/gen_state_inventory.py
	@mkdir -p $(dir $@)
	@printf '%s\n' $(STATE_INV_IN) > $@.list
	@python3 pc/tools/gen_state_inventory.py --nm $(STATE_NM) --out $@ @$@.list
$(STATE_INV_OBJ): $(STATE_INV_C)
	@$(CC) $(CFLAGS) -c $< -o $@
$(TARGET): $(STATE_INV_OBJ)
