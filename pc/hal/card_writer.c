/* SPDX-License-Identifier: GPL-3.0-or-later */
/* card_writer.c -- writes the memory card file (saves/card1.mcr) off the
 * game thread (pc/hal/memcard.c card_flush hands it the image).
 *
 * The card image in memory is the machine's (memcard.c g_card); the file is
 * only its copy on disk.  Writing that copy on the game thread made a slow
 * host write a frame stall: on a player's Windows machine the first card
 * write took over 5 s inside a system DLL (antivirus scanning the new file).
 * Here the game thread copies the 128 KB image and returns; one worker
 * writes it to CARD.tmp and replaces the card with it (an interrupted write
 * can no longer leave a truncated card), and a newer image submitted while
 * one is being written replaces the pending one, so the file always ends up
 * as the last image submitted.
 *
 * card_writer_wait() drains the queue; every orderly exit calls it
 * (pc/hal/system.c hal_shutdown, pc/hal/trace.c PC_TRACE_FRAMES and its
 * quit, the toolkit's `quit`).  PC_CARD_SYNC=1 writes on the game thread
 * as before (measurements).
 *
 * Statics: the host's side of a card write (a thread, the image being
 * written), kept out of a save state (pc/hal/dbg_state.c KEEP_FILES).
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CARD_MAX 0x20000

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static int g_started, g_start_failed;
static unsigned char g_pending[CARD_MAX], g_writing[CARD_MAX];
static size_t g_pending_n;
static char g_pending_path[1024];
static int g_have_pending, g_busy;
static unsigned long g_writes, g_coalesced;

static unsigned long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

/* write N bytes to PATH.tmp and replace PATH with it; 0 ok */
static int write_card(const char *path, const unsigned char *img, size_t n) {
    char tmp[1100];
    FILE *fp;
    int ok;
    unsigned long long t0 = now_ms();
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fp = fopen(tmp, "wb");
    if (!fp) {
        fprintf(stderr, "[memcard] flush: cannot write %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    ok = fwrite(img, 1, n, fp) == n;
    ok = fflush(fp) == 0 && ok;
    ok = fclose(fp) == 0 && ok;
    if (ok) {
        int tries;
        /* Windows: a scanner holding the old file open makes the replace
         * fail for a moment; try again before giving up */
        for (tries = 0; tries < 20 && rename(tmp, path); tries++) usleep(50 * 1000);
        ok = tries < 20;
    }
    if (!ok) {
        fprintf(stderr, "[memcard] flush: cannot replace %s: %s\n", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    /* a slow host write (antivirus scanning a new file, a synced folder)
     * shows here, off the game thread */
    if (now_ms() - t0 > 250)
        fprintf(stderr, "[memcard] flush took %llu ms (%s), off the game thread\n", now_ms() - t0,
                path);
    return 0;
}

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        char path[sizeof g_pending_path];
        size_t n;
        while (!g_have_pending) pthread_cond_wait(&g_cv, &g_mu);
        n = g_pending_n;
        memcpy(g_writing, g_pending, n);
        memcpy(path, g_pending_path, sizeof path);
        g_have_pending = 0;
        g_busy = 1;
        pthread_mutex_unlock(&g_mu);
        write_card(path, g_writing, n);
        pthread_mutex_lock(&g_mu);
        g_busy = 0;
        g_writes++;
        pthread_cond_broadcast(&g_cv);
    }
    return NULL;
}

static int sync_mode(void) {
    const char *e = getenv("PC_CARD_SYNC");
    return e && *e == '1';
}

/* Queue the card image for PATH; the game thread returns at once. */
void card_writer_submit(const char *path, const unsigned char *img, size_t n) {
    if (n > CARD_MAX) n = CARD_MAX;
    pthread_mutex_lock(&g_mu);
    if (!g_started && !g_start_failed && !sync_mode()) {
        if (pthread_create(&g_thread, NULL, worker, NULL) == 0) {
            pthread_detach(g_thread);
            g_started = 1;
        } else {
            g_start_failed = 1;
        }
    }
    if (!g_started) {
        pthread_mutex_unlock(&g_mu);
        write_card(path, img, n);      /* no thread: as before, on this one */
        return;
    }
    if (g_have_pending) g_coalesced++;
    memcpy(g_pending, img, n);
    g_pending_n = n;
    snprintf(g_pending_path, sizeof g_pending_path, "%s", path);
    g_have_pending = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

/* Until every submitted image is on disk. */
void card_writer_wait(void) {
    pthread_mutex_lock(&g_mu);
    while (g_started && (g_have_pending || g_busy)) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
}

/* 1 while a write is queued or running (tests, the toolkit) */
int card_writer_busy(void) {
    int b;
    pthread_mutex_lock(&g_mu);
    b = g_have_pending || g_busy;
    pthread_mutex_unlock(&g_mu);
    return b;
}
