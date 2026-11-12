/* SPDX-License-Identifier: GPL-3.0-or-later */
// Minimal assert stub (pulled in by fifo_queue.h).
#pragma once
#ifndef DebugAssert
#define DebugAssert(cond) ((void)0)
#endif
#ifndef Assert
#define Assert(cond) ((void)0)
#endif
#ifndef Panic
#define Panic(msg) ((void)0)
#endif
#ifndef AssertMsg
#define AssertMsg(cond, msg) ((void)0)
#endif
