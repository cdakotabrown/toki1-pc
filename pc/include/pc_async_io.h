/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_async_io.h -- file writes made during play, on a worker thread
 * (pc/hal/async_io.c).  Callers declare these weak, so a harness that
 * links a writer alone runs its jobs inline. */
#ifndef PC_ASYNC_IO_H
#define PC_ASYNC_IO_H
#include <stddef.h>

/* Queue fn(arg) on the writer thread (inline with TOKI_SYNC_WRITES=1).
 * `bytes`: memory the job holds until it runs (back-pressure bound). */
void pc_async_run(void (*fn)(void *), void *arg, size_t bytes);
/* Wait until every queued job has run. */
void pc_async_flush(void);
/* 1 when jobs run on the worker thread. */
int pc_async_threaded(void);
void pc_async_stats(unsigned long *ran, unsigned long *max_queued);

/* For callers: run through the queue when async_io.c is linked. */
#define PC_ASYNC_DECLARE_WEAK                                                  \
    extern void pc_async_run(void (*)(void *), void *, size_t) __attribute__((weak)); \
    extern void pc_async_flush(void) __attribute__((weak));
#endif
