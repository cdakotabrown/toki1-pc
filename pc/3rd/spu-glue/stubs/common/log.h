/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal logging stub. All log calls become no-ops.
#pragma once
#include "types.h"
#include "fmt/format.h"

// Channel registration macro (no-op).
#define Log_SetChannel(channel_name) /* no-op */

// fmt-style logging: all arguments discarded.
// Intentionally permissive -- accept any number of args.
#define SPU_CORE_LOG_NOOP(...) ((void)0)

#define TRACE_LOG(...)   SPU_CORE_LOG_NOOP(__VA_ARGS__)
#define DEBUG_LOG(...)   SPU_CORE_LOG_NOOP(__VA_ARGS__)
#define DEV_LOG(...)     SPU_CORE_LOG_NOOP(__VA_ARGS__)
#define INFO_LOG(...)    SPU_CORE_LOG_NOOP(__VA_ARGS__)
#define WARNING_LOG(...) SPU_CORE_LOG_NOOP(__VA_ARGS__)
#define ERROR_LOG(...)   SPU_CORE_LOG_NOOP(__VA_ARGS__)
#define VERBOSE_LOG(...) SPU_CORE_LOG_NOOP(__VA_ARGS__)

// spu.cpp also invokes Log_DebugFmt directly (non-macro form).
#define Log_DebugFmt(...) SPU_CORE_LOG_NOOP(__VA_ARGS__)

// DebugAssert -- swallow and ignore (assert.h also does this; keep consistent).
#ifndef DebugAssert
#define DebugAssert(cond) ((void)0)
#endif
#ifndef Assert
#define Assert(cond) ((void)0)
#endif
#ifndef Panic
#define Panic(msg) ((void)0)
#endif
