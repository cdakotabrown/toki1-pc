/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A watchdog interval covers one frame of host work, not an entire
 * standalone scene. Refresh only after VSync and presentation complete:
 * computation/render stalls that never finish a frame still time out.
 */
#include "pc_watchdog.h"

#ifndef WATCHDOG_SEC
#define WATCHDOG_SEC 5
#endif

#ifdef _WIN32
/* Windows: no SIGALRM.  A watchdog thread keeps the deadline the frame
 * refreshes; when it passes, the thread suspends the game thread and hands
 * its context to pc/src/main.c (pc_win_alarm_redirect), which points it at
 * the same report-and-recover path the Linux SIGALRM handler runs, on the
 * game thread.  Like setitimer's one-shot timer, a fired deadline stays
 * disarmed until the next refresh. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

extern void pc_win_alarm_redirect(void *context);       /* pc/src/main.c */
extern unsigned long pc_win_game_thread_id(void);

static volatile LONG g_armed;
static volatile LONG64 g_deadline;   /* GetTickCount64() ms; 0 = disarmed */
static HANDLE g_thread;

static DWORD WINAPI watchdog_main(LPVOID unused)
{
    (void)unused;
    for (;;) {
        LONG64 d = InterlockedCompareExchange64(&g_deadline, 0, 0);
        if (d && (LONG64)GetTickCount64() >= d &&
            InterlockedCompareExchange64(&g_deadline, 0, d) == d) {
            HANDLE game = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                     THREAD_SET_CONTEXT, FALSE,
                                     (DWORD)pc_win_game_thread_id());
            if (game && SuspendThread(game) != (DWORD)-1) {
                CONTEXT c;
                memset(&c, 0, sizeof c);
                c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(game, &c)) {
                    HMODULE mod = NULL;
                    char name[MAX_PATH] = "?";
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCSTR)(uintptr_t)c.Rip, &mod) && mod)
                        GetModuleFileNameA(mod, name, sizeof name);
                    if (mod && mod != GetModuleHandleA(NULL)) {
                        /* Stalled inside Windows (a file write an antivirus
                         * is scanning, a driver): jumping out of a system
                         * DLL can leave a loader or heap lock held, so wait
                         * for it to return instead (the game code itself
                         * never runs there).  Seen on real Windows during
                         * the first memory-card write, 2026-11-02. */
                        fprintf(stderr, "[WATCHDOG] host stall in %s (rip %p): waiting\n",
                                name, (void *)(uintptr_t)c.Rip);
                        fflush(stderr);
                        InterlockedExchange64(&g_deadline, (LONG64)GetTickCount64() +
                                                           WATCHDOG_SEC * 1000LL);
                    } else {
                        pc_win_alarm_redirect(&c);
                        SetThreadContext(game, &c);
                    }
                }
                ResumeThread(game);
            }
            if (game) CloseHandle(game);
        }
        Sleep(20);
    }
    return 0;
}

void pc_watchdog_presented(void)
{
    if (!g_armed) {
        return;
    }
    InterlockedExchange64(&g_deadline, (LONG64)GetTickCount64() + WATCHDOG_SEC * 1000LL);
    if (!g_thread)
        g_thread = CreateThread(NULL, 64 << 10, watchdog_main, NULL, 0, NULL);
}

void pc_watchdog_vblank(void)
{
    pc_watchdog_presented();
}

void pc_watchdog_arm(void)
{
    g_armed = 1;
    pc_watchdog_presented();
}

void pc_watchdog_disarm(void)
{
    g_armed = 0;
    InterlockedExchange64(&g_deadline, 0);
}
#else
#include <signal.h>
#include <sys/time.h>

static volatile sig_atomic_t g_armed;

void pc_watchdog_presented(void)
{
    struct itimerval timer = {{0, 0}, {0, 0}};

    if (!g_armed) {
        return;
    }
    timer.it_value.tv_sec = WATCHDOG_SEC;
    setitimer(ITIMER_REAL, &timer, 0);
}

/* An emulated VBlank also refreshes it.  A game loop that spins inside one
 * frame on the VBlank-fed pad buffer (RPG_BAT rpg_bat_pause: pause until
 * START, a pad_get() loop with no VSync) is the game waiting for the
 * player, as on the console: its emulated clock runs and its VBlanks are
 * delivered.  Until 2026-11-01 the 5 s alarm fired there and the recovery
 * longjmp'd out of the pause (explorer, card-end_02_yuina).  A host stall
 * -- work that never advances the emulated clock, a render that never
 * returns -- still expires; a game loop that waits for ever on emulated
 * time is the explorer's / playthrough's hang (no game frame for N
 * VBlanks). */
void pc_watchdog_vblank(void)
{
    pc_watchdog_presented();
}

void pc_watchdog_arm(void)
{
    g_armed = 1;
    pc_watchdog_presented();
}

void pc_watchdog_disarm(void)
{
    struct itimerval timer = {{0, 0}, {0, 0}};

    g_armed = 0;
    setitimer(ITIMER_REAL, &timer, 0);
}
#endif /* _WIN32 */
