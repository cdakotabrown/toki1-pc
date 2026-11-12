/* SPDX-License-Identifier: GPL-3.0-or-later */
/* async_io.c -- file writes made during play, off the game thread.
 *
 * A file the player's features write while the game runs (texture dumps,
 * gallery pictures, screenshots) used to be encoded and written on the game
 * thread.  On Linux that costs a few milliseconds; on a Windows desktop an
 * on-access virus scan, a OneDrive-synced folder or a slow disk can hold a
 * single CreateFile/CloseHandle for much longer, and the game thread then
 * misses VBlanks (audio underruns, a visible hitch).  Here a job -- a
 * function and its argument, usually a filled buffer the job owns and frees
 * -- runs on one worker thread, in submission order.
 *
 *   pc_async_run(fn, arg, bytes)   queue fn(arg); `bytes` is the memory the
 *                                  job holds until it ran (back-pressure:
 *                                  past 64 MB queued, the caller waits)
 *   pc_async_flush()               wait until every queued job has run
 *                                  (also at exit, and before the trace
 *                                  harness's _exit)
 *
 * TOKI_SYNC_WRITES=1 runs every job inline on the caller's thread (the old
 * behaviour; the negative control of pc/tools/test_async_io.py).  Callers
 * reach this through weak references, so a harness that links a writer
 * without this file runs the job inline too.  Nothing the game reads goes
 * through here: game state, traces and saves are written as before. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc_async_io.h"

typedef struct Job {
    void (*fn)(void *);
    void *arg;
    size_t bytes;
    struct Job *next;
} Job;

#define QUEUE_LIMIT (64u << 20)

static SDL_mutex *g_lock;
static SDL_cond *g_wake, *g_idle;
static SDL_Thread *g_thread;
static Job *g_head, *g_tail;
static size_t g_queued;
static int g_busy, g_mode = -1;          /* -1 unknown, 0 inline, 1 thread */
static unsigned long g_ran, g_max_queued;

static int worker(void *unused) {
    (void)unused;
    SDL_LockMutex(g_lock);
    for (;;) {
        Job *j;
        while (!g_head) SDL_CondWait(g_wake, g_lock);
        j = g_head;
        g_head = j->next;
        if (!g_head) g_tail = NULL;
        g_busy = 1;
        SDL_UnlockMutex(g_lock);
        j->fn(j->arg);
        SDL_LockMutex(g_lock);
        g_queued -= j->bytes;
        g_busy = 0;
        g_ran++;
        free(j);
        SDL_CondBroadcast(g_idle);
    }
    return 0;
}

static void start(void) {
    const char *e = getenv("TOKI_SYNC_WRITES");
    g_mode = 0;
    if (e && *e && *e != '0') {
        printf("[ASYNC] TOKI_SYNC_WRITES: files are written on the game thread\n");
        return;
    }
    g_lock = SDL_CreateMutex();
    g_wake = SDL_CreateCond();
    g_idle = SDL_CreateCond();
    if (g_lock && g_wake && g_idle)
        g_thread = SDL_CreateThread(worker, "file-writer", NULL);
    if (!g_thread) {
        fprintf(stderr, "[ASYNC] no writer thread (%s): writing inline\n", SDL_GetError());
        return;
    }
    SDL_DetachThread(g_thread);
    g_mode = 1;
    atexit(pc_async_flush);
}

int pc_async_threaded(void) {
    if (g_mode < 0) start();
    return g_mode == 1;
}

void pc_async_run(void (*fn)(void *), void *arg, size_t bytes) {
    Job *j;
    if (!pc_async_threaded() || !(j = (Job *)malloc(sizeof *j))) {
        fn(arg);
        return;
    }
    j->fn = fn;
    j->arg = arg;
    j->bytes = bytes;
    j->next = NULL;
    SDL_LockMutex(g_lock);
    /* back-pressure: a burst of dumps may not hold unbounded memory */
    while (g_queued && g_queued + bytes > QUEUE_LIMIT) SDL_CondWait(g_idle, g_lock);
    if (g_tail) g_tail->next = j; else g_head = j;
    g_tail = j;
    g_queued += bytes;
    if (g_queued > g_max_queued) g_max_queued = g_queued;
    SDL_CondSignal(g_wake);
    SDL_UnlockMutex(g_lock);
}

void pc_async_flush(void) {
    if (g_mode != 1) return;
    SDL_LockMutex(g_lock);
    while (g_head || g_busy) SDL_CondWait(g_idle, g_lock);
    SDL_UnlockMutex(g_lock);
}

void pc_async_stats(unsigned long *ran, unsigned long *max_queued) {
    if (ran) *ran = g_ran;
    if (max_queued) *max_queued = g_max_queued;
}
